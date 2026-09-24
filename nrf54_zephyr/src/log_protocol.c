#include "log_protocol.h"

#include "../../nrf52_oldsdk/src/ruuvi.endpoints.c/src/ruuvi_endpoints.h"

#include <errno.h>
#include <math.h>
#include <limits.h>
#include <string.h>

/* The legacy decoders left-shift promoted signed int; their top-bit case is
 * undefined in C. Preserve their API for the ordinary range and decode the
 * high half with unsigned arithmetic instead.
 */
static uint32_t request_time(const uint8_t *request, size_t offset,
                             uint32_t (*legacy_decoder)(const uint8_t *))
{
    if (request[offset] < 0x80U) {
        return legacy_decoder(request);
    }
    return ((uint32_t)request[offset] << 24U) |
           ((uint32_t)request[offset + 1U] << 16U) |
           ((uint32_t)request[offset + 2U] << 8U) |
           (uint32_t)request[offset + 3U];
}

int ruuvi_log_start(ruuvi_log_iterator_t *iterator, const uint8_t *request,
                    size_t request_length, uint64_t history_now_s)
{
    uint64_t window_s;
    uint64_t oldest_s;

    if (iterator == NULL || request == NULL) {
        return -EINVAL;
    }
    memset(iterator, 0, sizeof(*iterator));
    if (request_length != RE_STANDARD_MESSAGE_LENGTH) {
        return -EMSGSIZE;
    }
    if (request[RE_STANDARD_OPERATION_INDEX] != RE_STANDARD_LOG_VALUE_READ) {
        return -ENOTSUP;
    }
    switch (request[RE_STANDARD_DESTINATION_INDEX]) {
    case RE_STANDARD_DESTINATION_TEMPERATURE:
    case RE_STANDARD_DESTINATION_HUMIDITY:
    case RE_STANDARD_DESTINATION_PRESSURE:
    case RE_STANDARD_DESTINATION_ENVIRONMENTAL:
        break;
    case RE_STANDARD_DESTINATION_ACCELERATION:
    case RE_STANDARD_DESTINATION_ACCELERATION_X:
    case RE_STANDARD_DESTINATION_ACCELERATION_Y:
    case RE_STANDARD_DESTINATION_ACCELERATION_Z:
    case RE_STANDARD_DESTINATION_GYRATION:
    case RE_STANDARD_DESTINATION_GYRATION_X:
    case RE_STANDARD_DESTINATION_GYRATION_Y:
    case RE_STANDARD_DESTINATION_GYRATION_Z:
        /* Current history stores environmental samples only; reply with EOF. */
        iterator->exhausted = true;
        break;
    default:
        return -ENOTSUP;
    }
    iterator->current_time_s = request_time(request, RE_LOG_READ_CURRENT_MSB_IDX,
                                            re_std_log_current_time);
    iterator->start_time_s = request_time(request, RE_LOG_READ_START_MSB_IDX,
                                          re_std_log_start_time);
    if (iterator->current_time_s <= iterator->start_time_s) {
        return -EINVAL;
    }

    window_s = (uint64_t)iterator->current_time_s - iterator->start_time_s;
    oldest_s = history_now_s > window_s ? history_now_s - window_s : 0U;
    iterator->exhausted = iterator->exhausted || oldest_s > UINT32_MAX;
    iterator->min_timestamp_s = iterator->exhausted ? UINT32_MAX : (uint32_t)oldest_s;
    iterator->history_now_s = history_now_s;
    iterator->destination = request[RE_STANDARD_DESTINATION_INDEX];
    iterator->requester = request[RE_STANDARD_SOURCE_INDEX];
    iterator->active = true;
    return 0;
}

/* Legacy field bit order is humidity, pressure, temperature. */
static uint8_t source_for_field(uint8_t field)
{
    static const uint8_t sources[] = {
        RE_STANDARD_DESTINATION_HUMIDITY,
        RE_STANDARD_DESTINATION_PRESSURE,
        RE_STANDARD_DESTINATION_TEMPERATURE,
    };
    return sources[field];
}

static float value_for_field(const ruuvi_history_element_t *sample, uint8_t field)
{
    switch (field) {
    case 0:
        return sample->humidity_rh;
    case 1:
        return sample->pressure_pa;
    default:
        return sample->temperature_c;
    }
}

int ruuvi_log_next(ruuvi_log_iterator_t *iterator,
                   uint8_t response[RUUVI_LOG_MESSAGE_LENGTH])
{
    uint8_t frame[RE_STANDARD_MESSAGE_LENGTH] = {0};

    if (iterator == NULL || response == NULL) {
        return -EINVAL;
    }
    if (!iterator->active) {
        return iterator->exhausted ? 0 : -EINVAL;
    }
    for (;;) {
        if (!iterator->have_sample && !iterator->exhausted) {
            int rc = ruuvi_history_read(iterator->min_timestamp_s, iterator->index,
                                        &iterator->sample);
            if (rc == -ENOENT) {
                iterator->exhausted = true;
            } else if (rc < 0) {
                return rc;
            } else if ((uint64_t)iterator->sample.timestamp_s > iterator->history_now_s) {
                iterator->exhausted = true;
            } else {
                iterator->have_sample = true;
                iterator->field = 0;
            }
        }
        if (iterator->exhausted) {
            memset(frame + RE_STANDARD_HEADER_LENGTH, 0xFF,
                   RE_STANDARD_PAYLOAD_LENGTH);
            frame[RE_STANDARD_SOURCE_INDEX] = iterator->destination;
            frame[RE_STANDARD_DESTINATION_INDEX] = iterator->requester;
            frame[RE_STANDARD_OPERATION_INDEX] = RE_STANDARD_LOG_VALUE_WRITE;
            memcpy(response, frame, sizeof(frame));
            iterator->active = false;
            return 1;
        }

        while (iterator->field < 3U) {
            uint8_t source = source_for_field(iterator->field++);
            float value;
            double scaled;
            uint64_t mapped_s;
            float scale;

            if (iterator->destination != RE_STANDARD_DESTINATION_ENVIRONMENTAL &&
                iterator->destination != source) {
                continue;
            }
            value = value_for_field(&iterator->sample, iterator->field - 1U);
            scale = source == RE_STANDARD_DESTINATION_PRESSURE ?
                    RE_STANDARD_PRESSURE_SF : RE_STANDARD_HUMIDITY_SF;
            if (!isfinite(value)) {
                continue;
            }
            /* The legacy encoder rounds into signed 32 bits. Refuse values
             * outside its safe range instead of relying on float-to-int UB.
             */
            scaled = (double)value * (double)scale;
            if (scaled < (double)INT32_MIN + 1.0 || scaled > (double)INT32_MAX - 1.0) {
                continue;
            }
            mapped_s = (uint64_t)iterator->sample.timestamp_s + iterator->current_time_s;
            if (mapped_s < iterator->history_now_s) {
                continue;
            }
            mapped_s -= iterator->history_now_s;
            if (mapped_s < iterator->start_time_s) {
                continue;
            }
            if (mapped_s > UINT32_MAX) {
                --iterator->field;
                return -EOVERFLOW;
            }
            if (re_log_write_header(frame, source) != RE_SUCCESS ||
                re_log_write_timestamp(frame, mapped_s * UINT64_C(1000)) != RE_SUCCESS ||
                re_log_write_data(frame, value, source) != RE_SUCCESS) {
                --iterator->field;
                return -ERANGE;
            }
            frame[RE_STANDARD_DESTINATION_INDEX] = iterator->requester;
            memcpy(response, frame, sizeof(frame));
            return 1;
        }
        iterator->have_sample = false;
        if (iterator->index == UINT32_MAX) {
            return -EOVERFLOW;
        }
        ++iterator->index;
    }
}
