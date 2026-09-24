/* Golden bytes from sibling ruuvi.endpoints.c/test/test_ruuvi_endpoint_{3,5,c5}.c.
 * Encrypted-format fixtures below exercise callback wiring only, NOT AES.
 */
#include "formats.h"
#include "ruuvi_endpoint_3.h"
#include "ruuvi_endpoint_5.h"
#include "ruuvi_endpoint_7.h"
#include "ruuvi_endpoint_8.h"
#include "ruuvi_endpoint_c5.h"
#include "ruuvi_endpoint_fa.h"
#include "ruuvi_endpoints.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <errno.h>
#include <math.h>
#include <string.h>

static void check(ruuvi_format_t format, const ruuvi_measurement_t *data,
                  const uint8_t *expected, size_t expected_size)
{
    uint8_t output[RUUVI_FORMAT_MAX_LENGTH] = {0};
    size_t size = sizeof(output);
    assert(ruuvi_format_encode(format, data, NULL, output, &size) == 0);
    assert(size == expected_size);
    assert(memcmp(output, expected, size) == 0);
}

static void test_rotation(void)
{
    const ruuvi_format_t order[] = {
        RUUVI_FORMAT_3, RUUVI_FORMAT_5, RUUVI_FORMAT_7,
        RUUVI_FORMAT_8, RUUVI_FORMAT_C5, RUUVI_FORMAT_FA,
    };
    assert(RUUVI_FORMAT_3 == 1U && RUUVI_FORMAT_5 == 2U &&
           RUUVI_FORMAT_7 == 4U && RUUVI_FORMAT_8 == 8U &&
           RUUVI_FORMAT_C5 == 16U && RUUVI_FORMAT_FA == 32U);
    assert(ruuvi_format_next(0, RUUVI_FORMAT_INVALID) == RUUVI_FORMAT_INVALID);
    assert(ruuvi_format_next(1U << 20, RUUVI_FORMAT_3) == RUUVI_FORMAT_INVALID);
    assert(ruuvi_format_next(RUUVI_FORMAT_ALL, RUUVI_FORMAT_INVALID) == order[0]);
    for (size_t i = 0; i < sizeof(order) / sizeof(order[0]); ++i) {
        assert(ruuvi_format_next(RUUVI_FORMAT_ALL, order[i]) ==
               order[(i + 1U) % (sizeof(order) / sizeof(order[0]))]);
        assert(ruuvi_format_next(order[i], order[i]) == order[i]);
    }
    assert(ruuvi_format_next(RUUVI_FORMAT_3 | RUUVI_FORMAT_7 | RUUVI_FORMAT_FA,
                             RUUVI_FORMAT_3) == RUUVI_FORMAT_7);
    assert(ruuvi_format_next(RUUVI_FORMAT_3 | RUUVI_FORMAT_7 | RUUVI_FORMAT_FA,
                             RUUVI_FORMAT_7) == RUUVI_FORMAT_FA);
    assert(ruuvi_format_next(RUUVI_FORMAT_3 | RUUVI_FORMAT_7 | RUUVI_FORMAT_FA,
                             RUUVI_FORMAT_FA) == RUUVI_FORMAT_3);
}

static ruuvi_measurement_t typical(void)
{
    return (ruuvi_measurement_t) {
        .temperature_c = 24.3f, .humidity_rh = 53.49f, .pressure_pa = 100044.0f,
        .accelerationx_g = 0.004f, .accelerationy_g = -0.004f,
        .accelerationz_g = 1.036f, .battery_v = 2.977f,
        .measurement_count = 205, .movement_count = 66,
        .address = 0xCBB8334C884FULL, .tx_power = 4,
    };
}

static void test_plain_formats(void)
{
    ruuvi_measurement_t data = typical();
    static const uint8_t df5[] = {
        0x05, 0x12, 0xFC, 0x53, 0x94, 0xC3, 0x7C, 0x00,
        0x04, 0xFF, 0xFC, 0x04, 0x0C, 0xAC, 0x36, 0x42,
        0x00, 0xCD, 0xCB, 0xB8, 0x33, 0x4C, 0x88, 0x4F,
    };
    static const uint8_t c5[] = {
        0xC5, 0x12, 0xFC, 0x53, 0x94, 0xC3, 0x7C,
        0xAC, 0x36, 0x42, 0x00, 0xCD,
        0xCB, 0xB8, 0x33, 0x4C, 0x88, 0x4F,
    };
    check(RUUVI_FORMAT_5, &data, df5, sizeof(df5));
    check(RUUVI_FORMAT_C5, &data, c5, sizeof(c5));

    data.humidity_rh = 20.5f;
    data.pressure_pa = 102766.0f;
    data.temperature_c = 26.3f;
    data.accelerationx_g = -1.0f;
    data.accelerationy_g = -1.726f;
    data.accelerationz_g = 0.714f;
    data.battery_v = 2.899f;
    static const uint8_t df3[] = {
        0x03, 0x29, 0x1A, 0x1E, 0xCE, 0x1E, 0xFC,
        0x18, 0xF9, 0x42, 0x02, 0xCA, 0x0B, 0x53,
    };
    check(RUUVI_FORMAT_3, &data, df3, sizeof(df3));

    data = typical();
    data.accelerationx_g = 0.259f;
    data.accelerationy_g = -0.5f;
    data.accelerationz_g = 0.826f;
    data.battery_v = 2.9f;
    data.luminosity_lux = 350.0f;
    data.color_temp_k = 4500.0f;
    data.measurement_count = 123;
    data.movement_count = 42;
    data.motion_intensity = 5;
    data.motion_detected = true;
    data.presence_detected = true;
    uint8_t df7[RE_7_DATA_LENGTH] = {0};
    size_t size = sizeof(df7);
    assert(ruuvi_format_encode(RUUVI_FORMAT_7, &data, NULL, df7, &size) == 0);
    assert(size == RE_7_DATA_LENGTH);
    assert(df7[RE_7_OFFSET_HEADER] == 0x07);
    assert(df7[RE_7_OFFSET_SEQ] == 123);
    assert(df7[RE_7_OFFSET_FLAGS] == 0x03);
    assert(df7[RE_7_OFFSET_TEMP_MSB] == 0x12 && df7[RE_7_OFFSET_TEMP_LSB] == 0xFC);
    assert(df7[RE_7_OFFSET_HUMI_MSB] == 0x53 && df7[RE_7_OFFSET_HUMI_LSB] == 0x94);
    assert(df7[RE_7_OFFSET_MOTION_CNT] == 42);
    assert(df7[RE_7_OFFSET_MAC_0] == 0x4C &&
           df7[RE_7_OFFSET_MAC_1] == 0x88 && df7[RE_7_OFFSET_MAC_2] == 0x4F);
    assert(df7[RE_7_OFFSET_CRC] == re_calc_crc8(df7, RE_7_OFFSET_CRC));
    /* Complete frame for the sibling endpoint_7 typical sample. */
    static const uint8_t expected7[] = {
        0x07, 0x7B, 0x03, 0x12, 0xFC, 0x53, 0x94, 0xC3, 0x7C,
        0x15, 0xD6, 0x01, 0x5E, 0x87, 0x95, 0x2A, 0x69,
        0x4C, 0x88, 0x4F,
    };
    assert(memcmp(df7, expected7, sizeof(expected7)) == 0);

    data.measurement_count = 255;
    data.movement_count = 255;
    size = sizeof(df7);
    assert(ruuvi_format_encode(RUUVI_FORMAT_7, &data, NULL, df7, &size) == 0);
    assert(df7[RE_7_OFFSET_SEQ] == 0 && df7[RE_7_OFFSET_MOTION_CNT] == 0);
    data = typical();
    data.measurement_count = 65535;
    data.movement_count = 255;
    uint8_t buffer[RUUVI_FORMAT_MAX_LENGTH];
    size = sizeof(buffer);
    assert(ruuvi_format_encode(RUUVI_FORMAT_5, &data, NULL, buffer, &size) == 0);
    assert(buffer[RE_5_OFFSET_SEQCTR_MSB] == 0 && buffer[RE_5_OFFSET_SEQCTR_LSB] == 0);
    assert(buffer[RE_5_OFFSET_MVTCTR] == 0);
}

/* Fixture simulates the endpoint's callback interface; it does not encrypt.
 * Production must provide an actual AES-128 ECB implementation and keys.
 */
static const uint8_t *fixture_cleartext;
static const uint8_t *fixture_key;
static uint32_t fixture_encrypt(const uint8_t *cleartext, uint8_t *ciphertext,
                                size_t size, const uint8_t *key, size_t key_size)
{
    static const uint8_t fixture_bytes[16] = {
        0x67, 0x61, 0x54, 0x69, 0x76, 0x75, 0x75, 0x52,
        0x6D, 0x6F, 0x43, 0x69, 0x76, 0x75, 0x75, 0x52,
    };
    assert(size == 16 && key_size == 16);
    assert(memcmp(cleartext, fixture_cleartext, 16) == 0);
    assert(memcmp(key, fixture_key, 16) == 0);
    memcpy(ciphertext, fixture_bytes, 16);
    return 0;
}

static uint32_t failing_encrypt(const uint8_t *cleartext, uint8_t *ciphertext,
                                size_t size, const uint8_t *key, size_t key_size)
{
    (void)cleartext;
    (void)ciphertext;
    (void)size;
    (void)key;
    (void)key_size;
    return 1;
}

static void test_crypto_boundary(void)
{
    ruuvi_measurement_t data = typical();
    uint8_t output[RUUVI_FORMAT_MAX_LENGTH];
    memset(output, 0xA5, sizeof(output));
    size_t size = sizeof(output);
    assert(ruuvi_format_encode(RUUVI_FORMAT_8, &data, NULL, output, &size) == -ENOTSUP);
    assert(ruuvi_format_encode(RUUVI_FORMAT_FA, &data, NULL, output, &size) == -ENOTSUP);
    assert(size == sizeof(output) && output[0] == 0xA5);
    assert(ruuvi_format_encode(RUUVI_FORMAT_8, &data, NULL, output, NULL) == -EINVAL);
    assert(ruuvi_format_encode(RUUVI_FORMAT_INVALID, &data, NULL, output, &size) == -EINVAL);
    assert(ruuvi_format_encode(RUUVI_FORMAT_5, &data, NULL, NULL, &size) == -EINVAL);
    size = RE_5_DATA_LENGTH - 1U;
    assert(ruuvi_format_encode(RUUVI_FORMAT_5, &data, NULL, output, &size) == -ENOSPC);
    assert(size == RE_5_DATA_LENGTH - 1U && output[0] == 0xA5);

    static const uint8_t base_key[16] = {
        0x52, 0x75, 0x75, 0x76, 0x69, 0x43, 0x6F, 0x6D,
        0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00, 0x11,
    };
    ruuvi_format_crypto_t crypto = {
        .encrypt = failing_encrypt, .key = base_key, .key_size = sizeof(base_key),
    };
    size = sizeof(output);
    assert(ruuvi_format_encode(RUUVI_FORMAT_8, &data, &crypto, output, &size) == -EIO);
    assert(ruuvi_format_encode(RUUVI_FORMAT_FA, &data, &crypto, output, &size) == -EIO);
    assert(size == sizeof(output) && output[0] == 0xA5);
    crypto.key_size = 15;
    assert(ruuvi_format_encode(RUUVI_FORMAT_8, &data, &crypto, output, &size) == -EINVAL);
    crypto.key_size = sizeof(base_key);
    crypto.encrypt = NULL;
    assert(ruuvi_format_encode(RUUVI_FORMAT_8, &data, &crypto, output, &size) == -ENOTSUP);

    static const uint8_t clear8[16] = {
        0x12, 0xFC, 0x53, 0x94, 0xC3, 0x7C, 0xAC, 0x36,
        0x19, 0xCF, 0x00, 0xCD, 0x00, 0x00, 0x00, 0x00,
    };
    static const uint8_t derived_key[16] = {
        0x52, 0x75, 0x75, 0x76, 0x69, 0x43, 0x6F, 0x6D,
        0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00, 0x11,
    };
    static const uint8_t expected8[RE_8_DATA_LENGTH] = {
        0x08, 0x67, 0x61, 0x54, 0x69, 0x76, 0x75, 0x75, 0x52,
        0x6D, 0x6F, 0x43, 0x69, 0x76, 0x75, 0x75, 0x52,
        0x3F, 0xCB, 0xB8, 0x33, 0x4C, 0x88, 0x4F,
    };
    static const uint8_t xor_key[16] = {
        0x5A, 0x74, 0x77, 0x75, 0x6D, 0x46, 0x69, 0x6A,
        0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00, 0x11,
    };
    crypto.encrypt = fixture_encrypt;
    fixture_cleartext = clear8;
    fixture_key = derived_key;
    data.movement_count = 6607;
    size = sizeof(output);
    assert(ruuvi_format_encode(RUUVI_FORMAT_8, &data, &crypto, output, &size) == 0);
    assert(size == sizeof(expected8) && memcmp(output, expected8, size) == 0);
    data.device_id = 0x0706050403020108ULL;
    fixture_key = xor_key;
    size = sizeof(output);
    assert(ruuvi_format_encode(RUUVI_FORMAT_8, &data, &crypto, output, &size) == 0);
    assert(memcmp(output, expected8, size) == 0);

    static const uint8_t clear_fa[16] = {
        0x29, 0x1A, 0x1E, 0xCE, 0x1E, 0xFC, 0x18, 0xF9,
        0x42, 0x02, 0xCA, 0x0B, 0x53, 0x01, 0x00, 0x00,
    };
    static const uint8_t expected_fa[RE_FA_DATA_LENGTH] = {
        0xFA, 0x67, 0x61, 0x54, 0x69, 0x76, 0x75, 0x75, 0x52,
        0x6D, 0x6F, 0x43, 0x69, 0x76, 0x75, 0x75, 0x52,
        0xC0, 0x00, 0xDE, 0xAD, 0xBE, 0xEF,
    };
    data = (ruuvi_measurement_t) {
        .humidity_rh = 20.5f, .pressure_pa = 102766.0f, .temperature_c = 26.3f,
        .accelerationx_g = -1.0f, .accelerationy_g = -1.726f,
        .accelerationz_g = 0.714f, .battery_v = 2.899f,
        .measurement_count = 1, .address = 0xC000DEADBEEFULL,
    };
    fixture_cleartext = clear_fa;
    fixture_key = base_key;
    size = sizeof(output);
    assert(ruuvi_format_encode(RUUVI_FORMAT_FA, &data, &crypto, output, &size) == 0);
    assert(size == sizeof(expected_fa) && memcmp(output, expected_fa, size) == 0);
}

int main(void)
{
    test_rotation();
    test_plain_formats();
    test_crypto_boundary();
    return 0;
}
