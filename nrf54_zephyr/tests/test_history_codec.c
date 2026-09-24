#include "history_codec.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>

static uint8_t slots[RUUVI_HISTORY_RECORD_SLOTS][RUUVI_HISTORY_RECORD_SIZE];
static ruuvi_history_record_t source;
static ruuvi_history_record_t decoded;

static void test_known_vector(void)
{
    static const uint8_t header_and_first_sample[] = {
        0x40, 0x30, 0x20, 0x10, /* start timestamp */
        0x80, 0x70, 0x60, 0x50, /* end timestamp */
        0x01, 0x00, 0x00, 0x00, /* 32-bit num_samples */
        0x2C, 0x01, 0x01, 0x00, /* interval 300, overflow true, padding */
        0x80, 0x00, 0x05, 0x00, /* humidity/pressure/temperature bits */
        0x44, 0x33, 0x22, 0x11, /* sample timestamp */
        0x00, 0x00, 0xC0, 0xBF, /* temperature -1.5f */
        0x00, 0x00, 0x49, 0x42, /* humidity 50.25f */
        0x00, 0x50, 0xC3, 0x47, /* pressure 100000.0f */
    };
    assert(RUUVI_HISTORY_RECORD_SLOTS == 14U);
    assert(RUUVI_HISTORY_SAMPLES_PER_RECORD == 250U);
    assert(RUUVI_HISTORY_RECORD_SIZE == 4020U);
    assert(RUUVI_HISTORY_DEFAULT_FIELDS == 0x00050080U);

    memset(&source, 0, sizeof(source));
    source.start_timestamp_s = 0x10203040U;
    source.end_timestamp_s = 0x50607080U;
    source.num_samples = 1U;
    source.block_configuration.interval_s = RUUVI_HISTORY_DEFAULT_INTERVAL_S;
    source.block_configuration.overflow = RUUVI_HISTORY_DEFAULT_OVERFLOW;
    source.block_configuration.fields = RUUVI_HISTORY_DEFAULT_FIELDS;
    source.storage[0] = (ruuvi_history_element_t) {
        .timestamp_s = 0x11223344U,
        .temperature_c = -1.5f,
        .humidity_rh = 50.25f,
        .pressure_pa = 100000.0f,
    };
    memset(slots[0], 0xA5, sizeof(slots[0]));
    assert(ruuvi_history_encode(&source, slots[0], sizeof(slots[0])) == 0);
    assert(memcmp(slots[0], header_and_first_sample, sizeof(header_and_first_sample)) == 0);
    for (size_t i = sizeof(header_and_first_sample); i < sizeof(slots[0]); ++i) {
        assert(slots[0][i] == 0U);
    }

    memset(&decoded, 0xA5, sizeof(decoded));
    assert(ruuvi_history_decode(slots[0], sizeof(slots[0]), &decoded) == 0);
    assert(decoded.start_timestamp_s == source.start_timestamp_s);
    assert(decoded.end_timestamp_s == source.end_timestamp_s);
    assert(decoded.num_samples == 1U);
    assert(decoded.block_configuration.interval_s == 300U);
    assert(decoded.block_configuration.overflow);
    assert(decoded.block_configuration.fields == 0x00050080U);
    assert(decoded.storage[0].timestamp_s == 0x11223344U);
    assert(decoded.storage[0].temperature_c == -1.5f);
    assert(decoded.storage[0].humidity_rh == 50.25f);
    assert(decoded.storage[0].pressure_pa == 100000.0f);
    assert(decoded.storage[249].timestamp_s == 0U);
}

static void test_bounds(void)
{
    uint8_t bytes[RUUVI_HISTORY_RECORD_SIZE];
    memset(&source, 0, sizeof(source));
    source.num_samples = RUUVI_HISTORY_SAMPLES_PER_RECORD;
    source.storage[249] = (ruuvi_history_element_t) {
        .timestamp_s = 0xABCDEF01U, .temperature_c = 1.0f,
        .humidity_rh = 2.0f, .pressure_pa = 3.0f,
    };
    memset(bytes, 0xA5, sizeof(bytes));
    assert(ruuvi_history_encode(&source, bytes, sizeof(bytes)) == 0);
    assert(bytes[4004] == 0x01 && bytes[4005] == 0xEF &&
           bytes[4006] == 0xCD && bytes[4007] == 0xAB);
    assert(bytes[4019] == 0x40); /* last float: 3.0f */
    assert(ruuvi_history_decode(bytes, sizeof(bytes), &decoded) == 0);
    assert(decoded.num_samples == 250U);
    assert(decoded.storage[249].timestamp_s == 0xABCDEF01U);
    assert(decoded.storage[249].pressure_pa == 3.0f);

    assert(ruuvi_history_encode(NULL, bytes, sizeof(bytes)) == -EINVAL);
    assert(ruuvi_history_encode(&source, NULL, sizeof(bytes)) == -EINVAL);
    assert(ruuvi_history_decode(NULL, sizeof(bytes), &decoded) == -EINVAL);
    assert(ruuvi_history_decode(bytes, sizeof(bytes), NULL) == -EINVAL);
    assert(ruuvi_history_encode(&source, bytes, sizeof(bytes) - 1U) == -EMSGSIZE);
    assert(ruuvi_history_encode(&source, bytes, sizeof(bytes) + 1U) == -EMSGSIZE);
    assert(ruuvi_history_decode(bytes, sizeof(bytes) - 1U, &decoded) == -EMSGSIZE);
    assert(ruuvi_history_decode(bytes, sizeof(bytes) + 1U, &decoded) == -EMSGSIZE);
    source.num_samples = 251U;
    uint8_t previous[RUUVI_HISTORY_RECORD_SIZE];
    memcpy(previous, bytes, sizeof(bytes));
    assert(ruuvi_history_encode(&source, bytes, sizeof(bytes)) == -EINVAL);
    assert(memcmp(bytes, previous, sizeof(bytes)) == 0);
    bytes[8] = 251U;
    decoded.num_samples = 42U;
    assert(ruuvi_history_decode(bytes, sizeof(bytes), &decoded) == -EINVAL);
    assert(decoded.num_samples == 42U);
    bytes[8] = 0U;
    bytes[14] = 2U;
    assert(ruuvi_history_decode(bytes, sizeof(bytes), &decoded) == -EINVAL);
    assert(decoded.num_samples == 42U);
    bytes[14] = 0U;
    bytes[15] = 0xA5; /* legacy ABI padding is not a field */
    assert(ruuvi_history_decode(bytes, sizeof(bytes), &decoded) == 0);
    assert(decoded.num_samples == 0U);
}

static void fill_record(unsigned int id)
{
    memset(&source, 0, sizeof(source));
    source.start_timestamp_s = id * 250U;
    source.end_timestamp_s = source.start_timestamp_s + 249U;
    source.num_samples = RUUVI_HISTORY_SAMPLES_PER_RECORD;
    source.block_configuration.interval_s = (uint16_t)(RUUVI_HISTORY_DEFAULT_INTERVAL_S + id);
    source.block_configuration.overflow = RUUVI_HISTORY_DEFAULT_OVERFLOW;
    source.block_configuration.fields = 0xFFAA0080U ^ id;
    for (size_t i = 0; i < RUUVI_HISTORY_SAMPLES_PER_RECORD; ++i) {
        source.storage[i].timestamp_s = source.start_timestamp_s + (uint32_t)i;
        source.storage[i].temperature_c = (float)id + (float)i * 0.5f;
        source.storage[i].humidity_rh = (float)i;
        source.storage[i].pressure_pa = 100000.0f + (float)(id * 250U + i);
    }
}

static void check_slot(unsigned int slot, unsigned int id)
{
    assert(ruuvi_history_decode(slots[slot], RUUVI_HISTORY_RECORD_SIZE, &decoded) == 0);
    assert(decoded.start_timestamp_s == id * 250U);
    assert(decoded.end_timestamp_s == id * 250U + 249U);
    assert(decoded.num_samples == 250U);
    assert(decoded.block_configuration.interval_s == 300U + id);
    assert(decoded.block_configuration.overflow);
    assert(decoded.block_configuration.fields == (0xFFAA0080U ^ id));
    for (size_t i = 0; i < RUUVI_HISTORY_SAMPLES_PER_RECORD; ++i) {
        assert(decoded.storage[i].timestamp_s == id * 250U + i);
        assert(decoded.storage[i].temperature_c == (float)id + (float)i * 0.5f);
        assert(decoded.storage[i].humidity_rh == (float)i);
        assert(decoded.storage[i].pressure_pa == 100000.0f + (float)(id * 250U + i));
    }
}

static void test_full_ring_and_one_overwrite(void)
{
    /* These byte arrays model storage slots; the codec itself does not persist
     * or rotate them. A real backend must implement this policy separately.
     */
    for (unsigned int id = 0; id < RUUVI_HISTORY_RECORD_SLOTS; ++id) {
        fill_record(id);
        assert(ruuvi_history_encode(&source, slots[id], sizeof(slots[id])) == 0);
    }
    for (unsigned int id = 0; id < RUUVI_HISTORY_RECORD_SLOTS; ++id) {
        check_slot(id, id);
    }
    fill_record(RUUVI_HISTORY_RECORD_SLOTS);
    assert(ruuvi_history_encode(&source, slots[0], sizeof(slots[0])) == 0);
    check_slot(0, RUUVI_HISTORY_RECORD_SLOTS);
    for (unsigned int id = 1; id < RUUVI_HISTORY_RECORD_SLOTS; ++id) {
        check_slot(id, id);
    }
}

int main(void)
{
    test_known_vector();
    test_bounds();
    test_full_ring_and_one_overwrite();
    return 0;
}
