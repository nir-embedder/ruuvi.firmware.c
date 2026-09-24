#ifndef RUUVI_LOG_SERVICE_H
#define RUUVI_LOG_SERVICE_H

#include "log_protocol.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RUUVI_LOG_SERVICE_TIMEOUT_MS INT64_C(300000)
#define RUUVI_LOG_FRAME_TIMEOUT_MS INT64_C(4000)

/* Zero-initialize before the first start. Only one request can be active at a
 * time; the caller must serialize history access for the life of a read.
 * The caller reports history availability when starting a request.
 */
typedef struct {
    ruuvi_log_iterator_t iterator;
    uint8_t pending[RUUVI_LOG_MESSAGE_LENGTH];
    int64_t started_ms;
    int64_t frame_started_ms;
    uint8_t destination;
    uint8_t requester;
    bool active;
    bool read;
    bool pending_valid;
    bool timed_out;
    bool password_match;
} ruuvi_log_service_t;

/* A valid 0x11 starts a stream. Unsupported writes queue an unauthorized
 * response; unsupported odd reads return -ENOTSUP. The password endpoint
 * echoes a matching 8-byte device ID and responds 0x08, otherwise 0xEA.
 * A NULL device ID disables password authorization. When history_available is
 * false, valid log reads return a correctly addressed EOF without a flash read.
 * This is only a wire response; the caller decides when the next connection is enabled.
 */
int ruuvi_log_service_start_with_id(ruuvi_log_service_t *svc,
                                    const uint8_t request[RUUVI_LOG_MESSAGE_LENGTH],
                                    size_t len, uint64_t synthetic_now_s, int64_t now_ms,
                                    const uint8_t device_id[8], bool history_available);
int ruuvi_log_service_start(ruuvi_log_service_t *svc,
                             const uint8_t request[RUUVI_LOG_MESSAGE_LENGTH],
                             size_t len, uint64_t synthetic_now_s, int64_t now_ms);

/* send must accept an entire 11-byte response atomically: nonnegative means
 * accepted, -EAGAIN means busy, and other negative values mean failure.
 * At most one frame is accepted per call. Busy preserves the pending frame;
 * 1 means accepted and 0 means waiting or done. Four seconds of per-frame
 * backpressure aborts like legacy app_comms_blocking_send; a read overdue at
 * five minutes sends one 0xE0 frame with its own four-second retry window.
 */
int ruuvi_log_service_pump(ruuvi_log_service_t *svc, int64_t now_ms,
                           int (*send)(void *, const uint8_t *, size_t), void *ctx);
void ruuvi_log_service_abort(ruuvi_log_service_t *svc);
bool ruuvi_log_service_active(const ruuvi_log_service_t *svc);

#endif
