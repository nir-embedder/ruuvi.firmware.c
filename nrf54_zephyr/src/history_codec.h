#ifndef RUUVI_HISTORY_CODEC_H
#define RUUVI_HISTORY_CODEC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Legacy app_config.h: 16 flash pages, two reserved for swap/settings. */
#define RUUVI_HISTORY_RECORD_SLOTS 14U
/* Legacy app_log.h: (4096 - 96) / sizeof(app_log_element_t), 16 bytes each. */
#define RUUVI_HISTORY_SAMPLES_PER_RECORD 250U
#define RUUVI_HISTORY_RECORD_SIZE 4020U
#define RUUVI_HISTORY_DEFAULT_INTERVAL_S 300U
#define RUUVI_HISTORY_DEFAULT_OVERFLOW true
/* rd_sensor_data_fields_t is a uint32_t union, not a byte-sized enum.
 * On the legacy little-endian ARM build: humidity bit 7, pressure bit 16,
 * temperature bit 18. Keep all other bits when encoding/decoding.
 */
#define RUUVI_HISTORY_DEFAULT_FIELDS ((1U << 7) | (1U << 16) | (1U << 18))

/* Fixed-width equivalent of app_log_config_t and app_log_record_t. The wire
 * layout matches the 32-bit ARM legacy record's member offsets, not host ABI:
 * start 0, end 4, num_samples 8, interval 12, overflow 14, pad 15,
 * fields 16, storage 20; each element is timestamp + three binary32 floats.
 * All 250 elements are serialized even when num_samples is smaller.
 */
typedef struct {
    uint16_t interval_s;
    bool overflow;
    uint32_t fields;
} ruuvi_history_config_t;

typedef struct {
    uint32_t timestamp_s;
    float temperature_c;
    float humidity_rh;
    float pressure_pa;
} ruuvi_history_element_t;

typedef struct {
    uint32_t start_timestamp_s;
    uint32_t end_timestamp_s;
    uint32_t num_samples;
    ruuvi_history_config_t block_configuration;
    ruuvi_history_element_t storage[RUUVI_HISTORY_SAMPLES_PER_RECORD];
} ruuvi_history_record_t;

/* Exactly RUUVI_HISTORY_RECORD_SIZE bytes, little-endian. Return 0 on
 * success, -EINVAL for null/invalid count/overflow byte, -EMSGSIZE if
 * length differs from the fixed size. No output is changed on error.
 * This codec has no storage, flash, or rotation side effects.
 */
int ruuvi_history_encode(const ruuvi_history_record_t *record, uint8_t *output,
                         size_t length);
int ruuvi_history_decode(const uint8_t *input, size_t length,
                         ruuvi_history_record_t *record);

#endif
