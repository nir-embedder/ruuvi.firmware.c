#include "history_codec.h"

#include <errno.h>
#include <float.h>
#include <limits.h>
#include <string.h>

_Static_assert(CHAR_BIT == 8 && sizeof(uint32_t) == 4 && sizeof(float) == 4 &&
               FLT_RADIX == 2 && FLT_MANT_DIG == 24 && FLT_MAX_EXP == 128,
               "history codec requires IEEE 754 binary32 and eight-bit bytes");
_Static_assert(RUUVI_HISTORY_RECORD_SIZE == 20U + 16U * RUUVI_HISTORY_SAMPLES_PER_RECORD,
               "history record size must include every legacy sample");

enum {
    START_OFFSET = 0,
    END_OFFSET = 4,
    COUNT_OFFSET = 8,
    INTERVAL_OFFSET = 12,
    OVERFLOW_OFFSET = 14,
    PADDING_OFFSET = 15,
    FIELDS_OFFSET = 16,
    ELEMENTS_OFFSET = 20,
    ELEMENT_SIZE = 16,
};

static void put_u16(uint8_t *dest, uint16_t value)
{
    dest[0] = (uint8_t)value;
    dest[1] = (uint8_t)(value >> 8);
}

static void put_u32(uint8_t *dest, uint32_t value)
{
    dest[0] = (uint8_t)value;
    dest[1] = (uint8_t)(value >> 8);
    dest[2] = (uint8_t)(value >> 16);
    dest[3] = (uint8_t)(value >> 24);
}

static uint16_t get_u16(const uint8_t *src)
{
    return (uint16_t)((uint16_t)src[0] | ((uint16_t)src[1] << 8));
}

static uint32_t get_u32(const uint8_t *src)
{
    return (uint32_t)src[0] | ((uint32_t)src[1] << 8) |
           ((uint32_t)src[2] << 16) | ((uint32_t)src[3] << 24);
}

static void put_float(uint8_t *dest, float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    put_u32(dest, bits);
}

static float get_float(const uint8_t *src)
{
    uint32_t bits = get_u32(src);
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

int ruuvi_history_encode(const ruuvi_history_record_t *record, uint8_t *output,
                         size_t length)
{
    if (record == NULL || output == NULL || record->num_samples > RUUVI_HISTORY_SAMPLES_PER_RECORD) {
        return -EINVAL;
    }
    if (length != RUUVI_HISTORY_RECORD_SIZE) {
        return -EMSGSIZE;
    }

    put_u32(output + START_OFFSET, record->start_timestamp_s);
    put_u32(output + END_OFFSET, record->end_timestamp_s);
    put_u32(output + COUNT_OFFSET, record->num_samples);
    put_u16(output + INTERVAL_OFFSET, record->block_configuration.interval_s);
    output[OVERFLOW_OFFSET] = record->block_configuration.overflow ? 1U : 0U;
    output[PADDING_OFFSET] = 0U;
    put_u32(output + FIELDS_OFFSET, record->block_configuration.fields);

    for (size_t i = 0; i < RUUVI_HISTORY_SAMPLES_PER_RECORD; ++i) {
        uint8_t *element = output + ELEMENTS_OFFSET + i * ELEMENT_SIZE;
        put_u32(element, record->storage[i].timestamp_s);
        put_float(element + 4, record->storage[i].temperature_c);
        put_float(element + 8, record->storage[i].humidity_rh);
        put_float(element + 12, record->storage[i].pressure_pa);
    }
    return 0;
}

int ruuvi_history_decode(const uint8_t *input, size_t length,
                         ruuvi_history_record_t *record)
{
    if (input == NULL || record == NULL) {
        return -EINVAL;
    }
    if (length != RUUVI_HISTORY_RECORD_SIZE) {
        return -EMSGSIZE;
    }
    if (get_u32(input + COUNT_OFFSET) > RUUVI_HISTORY_SAMPLES_PER_RECORD ||
        input[OVERFLOW_OFFSET] > 1U) {
        return -EINVAL;
    }

    record->start_timestamp_s = get_u32(input + START_OFFSET);
    record->end_timestamp_s = get_u32(input + END_OFFSET);
    record->num_samples = get_u32(input + COUNT_OFFSET);
    record->block_configuration.interval_s = get_u16(input + INTERVAL_OFFSET);
    record->block_configuration.overflow = input[OVERFLOW_OFFSET] != 0U;
    record->block_configuration.fields = get_u32(input + FIELDS_OFFSET);

    for (size_t i = 0; i < RUUVI_HISTORY_SAMPLES_PER_RECORD; ++i) {
        const uint8_t *element = input + ELEMENTS_OFFSET + i * ELEMENT_SIZE;
        record->storage[i].timestamp_s = get_u32(element);
        record->storage[i].temperature_c = get_float(element + 4);
        record->storage[i].humidity_rh = get_float(element + 8);
        record->storage[i].pressure_pa = get_float(element + 12);
    }
    return 0;
}
