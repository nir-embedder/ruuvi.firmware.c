/* Golden vectors from nrf52_oldsdk/src/ruuvi.endpoints.c/test/test_ruuvi_endpoint_5.c. */
#include "../../nrf52_oldsdk/src/ruuvi.endpoints.c/src/ruuvi_endpoint_5.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

static void check(const re_5_data_t *sample, const uint8_t expected[RE_5_DATA_LENGTH])
{
    uint8_t actual[RE_5_DATA_LENGTH] = {0};
    assert(re_5_encode(actual, sample) == RE_SUCCESS);
    assert(memcmp(actual, expected, sizeof(actual)) == 0);

    uint8_t legacy_advertisement[31] = { 2, 1, 4, 27, 0xff, 0x99, 0x04 };
    memcpy(&legacy_advertisement[7], actual, sizeof(actual));
    assert(re_5_check_format(legacy_advertisement));
}

int main(void)
{
    const re_5_data_t typical = {
        .humidity_rh = 53.49f, .pressure_pa = 100044.0f,
        .temperature_c = 24.3f, .accelerationx_g = 0.004f,
        .accelerationy_g = -0.004f, .accelerationz_g = 1.036f,
        .battery_v = 2.977f, .measurement_count = 205,
        .movement_count = 66, .address = 0xCBB8334C884FULL, .tx_power = 4,
    };
    const uint8_t typical_bytes[RE_5_DATA_LENGTH] = {
        0x05, 0x12, 0xFC, 0x53, 0x94, 0xC3, 0x7C, 0x00,
        0x04, 0xFF, 0xFC, 0x04, 0x0C, 0xAC, 0x36, 0x42,
        0x00, 0xCD, 0xCB, 0xB8, 0x33, 0x4C, 0x88, 0x4F,
    };
    check(&typical, typical_bytes);

    const re_5_data_t minimum = {
        .humidity_rh = 0.0f, .pressure_pa = 50000.0f,
        .temperature_c = -163.835f, .accelerationx_g = -32.767f,
        .accelerationy_g = -32.767f, .accelerationz_g = -32.767f,
        .battery_v = 1.6f, .measurement_count = 0,
        .movement_count = 0, .address = 0xCBB8334C884FULL, .tx_power = -40,
    };
    const uint8_t minimum_bytes[RE_5_DATA_LENGTH] = {
        0x05, 0x80, 0x01, 0x00, 0x00, 0x00, 0x00, 0x80,
        0x01, 0x80, 0x01, 0x80, 0x01, 0x00, 0x00, 0x00,
        0x00, 0x00, 0xCB, 0xB8, 0x33, 0x4C, 0x88, 0x4F,
    };
    check(&minimum, minimum_bytes);

    const re_5_data_t unavailable = {
        .humidity_rh = NAN, .pressure_pa = NAN, .temperature_c = NAN,
        .accelerationx_g = NAN, .accelerationy_g = NAN, .accelerationz_g = NAN,
        .battery_v = NAN, .measurement_count = UINT16_MAX,
        .movement_count = UINT8_MAX, .address = 0xFFFFFFFFFFFFULL,
        .tx_power = RE_5_INVALID_POWER,
    };
    const uint8_t unavailable_bytes[RE_5_DATA_LENGTH] = {
        0x05, 0x80, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0x80,
        0x00, 0x80, 0x00, 0x80, 0x00, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    };
    check(&unavailable, unavailable_bytes);
    return 0;
}
