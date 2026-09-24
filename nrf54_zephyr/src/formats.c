#include "formats.h"

#include "ruuvi_endpoint_3.h"
#include "ruuvi_endpoint_5.h"
#include "ruuvi_endpoint_7.h"
#include "ruuvi_endpoint_8.h"
#include "ruuvi_endpoint_c5.h"
#include "ruuvi_endpoint_fa.h"

#include <errno.h>
#include <math.h>
#include <string.h>

ruuvi_format_t ruuvi_format_next(uint32_t enabled_mask, ruuvi_format_t current)
{
    const ruuvi_format_t order[] = {
        RUUVI_FORMAT_3, RUUVI_FORMAT_5, RUUVI_FORMAT_7,
        RUUVI_FORMAT_8, RUUVI_FORMAT_C5, RUUVI_FORMAT_FA,
    };
    size_t index = (sizeof(order) / sizeof(order[0])) - 1U;

    if ((enabled_mask & RUUVI_FORMAT_ALL) == 0U) {
        return RUUVI_FORMAT_INVALID;
    }

    for (size_t i = 0; i < sizeof(order) / sizeof(order[0]); ++i) {
        if (order[i] == current) {
            index = i;
            break;
        }
    }
    for (size_t i = 0; i < sizeof(order) / sizeof(order[0]); ++i) {
        index = (index + 1U) % (sizeof(order) / sizeof(order[0]));
        if ((enabled_mask & order[index]) != 0U) {
            return order[index];
        }
    }
    return RUUVI_FORMAT_INVALID;
}

int ruuvi_format_encode(ruuvi_format_t format, const ruuvi_measurement_t *measurement,
                        const ruuvi_format_crypto_t *crypto, uint8_t *output,
                        size_t *length)
{
    size_t needed;
    uint8_t payload[RUUVI_FORMAT_MAX_LENGTH] = {0};
    re_status_t status;

    if (measurement == NULL || output == NULL || length == NULL) {
        return -EINVAL;
    }

    switch (format) {
    case RUUVI_FORMAT_3: needed = RE_3_DATA_LENGTH; break;
    case RUUVI_FORMAT_5: needed = RE_5_DATA_LENGTH; break;
    case RUUVI_FORMAT_7: needed = RE_7_DATA_LENGTH; break;
    case RUUVI_FORMAT_8: needed = RE_8_DATA_LENGTH; break;
    case RUUVI_FORMAT_C5: needed = RE_C5_DATA_LENGTH; break;
    case RUUVI_FORMAT_FA: needed = RE_FA_DATA_LENGTH; break;
    default: return -EINVAL;
    }

    if (*length < needed) {
        return -ENOSPC;
    }
    if (format == RUUVI_FORMAT_8 || format == RUUVI_FORMAT_FA) {
        if (crypto == NULL || crypto->encrypt == NULL || crypto->key == NULL) {
            return -ENOTSUP;
        }
        if (crypto->key_size != RE_8_CIPHERTEXT_LENGTH) {
            return -EINVAL;
        }
    }

    switch (format) {
    case RUUVI_FORMAT_3: {
        const re_3_data_t data = {
            .humidity_rh = measurement->humidity_rh,
            .pressure_pa = measurement->pressure_pa,
            .temperature_c = measurement->temperature_c,
            .accelerationx_g = measurement->accelerationx_g,
            .accelerationy_g = measurement->accelerationy_g,
            .accelerationz_g = measurement->accelerationz_g,
            .battery_v = measurement->battery_v,
        };
        status = re_3_encode(payload, &data, NAN);
        break;
    }
    case RUUVI_FORMAT_5: {
        const re_5_data_t data = {
            .humidity_rh = measurement->humidity_rh,
            .pressure_pa = measurement->pressure_pa,
            .temperature_c = measurement->temperature_c,
            .accelerationx_g = measurement->accelerationx_g,
            .accelerationy_g = measurement->accelerationy_g,
            .accelerationz_g = measurement->accelerationz_g,
            .battery_v = measurement->battery_v,
            .measurement_count = measurement->measurement_count % (RE_5_SEQCTR_MAX + 1U),
            .movement_count = measurement->movement_count % (RE_5_MVTCTR_MAX + 1U),
            .address = measurement->address,
            .tx_power = measurement->tx_power,
        };
        status = re_5_encode(payload, &data);
        break;
    }
    case RUUVI_FORMAT_7: {
        const re_7_data_t data = {
            .humidity_rh = measurement->humidity_rh,
            .pressure_pa = measurement->pressure_pa,
            .temperature_c = measurement->temperature_c,
            .acceleration_x_g = measurement->accelerationx_g,
            .acceleration_y_g = measurement->accelerationy_g,
            .acceleration_z_g = measurement->accelerationz_g,
            .battery_v = measurement->battery_v,
            .luminosity_lux = measurement->luminosity_lux,
            .color_temp_k = measurement->color_temp_k,
            .sequence_counter = measurement->measurement_count % (RE_7_SEQCTR_MAX + 1U),
            .motion_count = measurement->movement_count % (RE_7_MOTION_CNT_MAX + 1U),
            .motion_intensity = measurement->motion_intensity,
            .motion_detected = measurement->motion_detected,
            .presence_detected = measurement->presence_detected,
            .address = measurement->address,
        };
        status = re_7_encode(payload, &data);
        break;
    }
    case RUUVI_FORMAT_8: {
        uint8_t key[RE_8_CIPHERTEXT_LENGTH];
        const re_8_data_t data = {
            .humidity_rh = measurement->humidity_rh,
            .pressure_pa = measurement->pressure_pa,
            .temperature_c = measurement->temperature_c,
            .battery_v = measurement->battery_v,
            .message_counter = measurement->measurement_count % (RE_8_SEQCTR_MAX + 1U),
            .movement_count = measurement->movement_count % (RE_8_MVTCTR_MAX + 1U),
            .address = measurement->address,
            .tx_power = measurement->tx_power,
        };
        memcpy(key, crypto->key, sizeof(key));
        for (size_t i = 0; i < 8U; ++i) {
            key[i] ^= (uint8_t)(measurement->device_id >> (i * 8U));
        }
        status = re_8_encode(payload, &data, crypto->encrypt, key, sizeof(key));
        memset(key, 0, sizeof(key));
        break;
    }
    case RUUVI_FORMAT_C5: {
        const re_c5_data_t data = {
            .humidity_rh = measurement->humidity_rh,
            .pressure_pa = measurement->pressure_pa,
            .temperature_c = measurement->temperature_c,
            .battery_v = measurement->battery_v,
            .measurement_count = measurement->measurement_count % (RE_C5_SEQCTR_MAX + 1U),
            .movement_count = measurement->movement_count % (RE_C5_MVTCTR_MAX + 1U),
            .address = measurement->address,
            .tx_power = measurement->tx_power,
        };
        status = re_c5_encode(payload, &data);
        break;
    }
    case RUUVI_FORMAT_FA: {
        const re_fa_data_t data = {
            .humidity_rh = measurement->humidity_rh,
            .pressure_pa = measurement->pressure_pa,
            .temperature_c = measurement->temperature_c,
            .accelerationx_g = measurement->accelerationx_g,
            .accelerationy_g = measurement->accelerationy_g,
            .accelerationz_g = measurement->accelerationz_g,
            .battery_v = measurement->battery_v,
            .message_counter = measurement->measurement_count % 0xFFU,
            .address = measurement->address,
        };
        status = re_fa_encode(payload, &data, crypto->encrypt, crypto->key, crypto->key_size);
        break;
    }
    default: return -EINVAL;
    }

    if (status != RE_SUCCESS) {
        return -EIO;
    }
    memcpy(output, payload, needed);
    *length = needed;
    return 0;
}
