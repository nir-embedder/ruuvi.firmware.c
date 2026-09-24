#ifndef RUUVI_LOG_PROTOCOL_H
#define RUUVI_LOG_PROTOCOL_H

#include "history.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RUUVI_LOG_MESSAGE_LENGTH 11U

/* One iterator per request. The caller serializes history access and must
 * restart after a concurrent history write, clear, or reinitialization.
 * history_now_s is the current time in the same synthetic clock as the
 * stored samples (including the post-reboot history base), not raw uptime.
 * The request's current-time field supplies the remote wall-clock mapping.
 * start returns 0 or negative errno; it does not access the history backend.
 */
typedef struct {
    ruuvi_history_element_t sample;
    uint64_t history_now_s;
    uint32_t current_time_s;
    uint32_t start_time_s;
    uint32_t min_timestamp_s;
    uint32_t index;
    uint8_t destination;
    uint8_t requester;
    uint8_t field;
    bool have_sample;
    bool exhausted;
    bool active;
} ruuvi_log_iterator_t;

int ruuvi_log_start(ruuvi_log_iterator_t *iterator, const uint8_t *request,
                    size_t request_length, uint64_t history_now_s);

/* Returns 1 for one 11-byte frame (including a single final EOF), 0 when
 * already finished, or negative errno for a history/encoding failure.
 * On failure no frame is produced. Transient history errors may be retried;
 * encoding or index overflow errors require aborting the request.
 * The call advances the iterator when it returns a frame. A worker must
 * retain that frame until its transport accepts it before calling again.
 */
int ruuvi_log_next(ruuvi_log_iterator_t *iterator,
                   uint8_t response[RUUVI_LOG_MESSAGE_LENGTH]);

#endif
