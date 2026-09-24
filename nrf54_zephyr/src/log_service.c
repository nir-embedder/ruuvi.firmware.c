#include "log_service.h"

#include "../../nrf52_oldsdk/src/ruuvi.endpoints.c/src/ruuvi_endpoints.h"

#include <errno.h>
#include <string.h>

static void terminal_frame(ruuvi_log_service_t *svc, uint8_t operation)
{
    svc->pending[RE_STANDARD_DESTINATION_INDEX] = svc->requester;
    svc->pending[RE_STANDARD_SOURCE_INDEX] = svc->destination;
    svc->pending[RE_STANDARD_OPERATION_INDEX] = operation;
    memset(svc->pending + RE_STANDARD_PAYLOAD_START_INDEX, 0xFF,
           RE_STANDARD_PAYLOAD_LENGTH);
    svc->pending_valid = true;
}

void ruuvi_log_service_abort(ruuvi_log_service_t *svc)
{
    if (svc != NULL) {
        memset(svc, 0, sizeof(*svc));
    }
}

bool ruuvi_log_service_active(const ruuvi_log_service_t *svc)
{
    return svc != NULL && svc->active;
}

int ruuvi_log_service_start_with_id(ruuvi_log_service_t *svc,
                                    const uint8_t request[RUUVI_LOG_MESSAGE_LENGTH],
                                    size_t len, uint64_t synthetic_now_s, int64_t now_ms,
                                    const uint8_t device_id[8])
{
    int rc;

    if (svc == NULL || request == NULL) {
        return -EINVAL;
    }
    if (svc->active) {
        return -EBUSY;
    }
    if (len != RUUVI_LOG_MESSAGE_LENGTH) {
        return -EMSGSIZE;
    }
    if ((request[RE_STANDARD_OPERATION_INDEX] & RE_STANDARD_OP_READ_BIT) != 0U &&
        request[RE_STANDARD_OPERATION_INDEX] != RE_STANDARD_LOG_VALUE_READ &&
        !(request[RE_STANDARD_DESTINATION_INDEX] == RE_STANDARD_DESTINATION_PASSWORD &&
          request[RE_STANDARD_OPERATION_INDEX] == RE_STANDARD_VALUE_READ)) {
        return -ENOTSUP;
    }

    ruuvi_log_service_abort(svc);
    svc->destination = request[RE_STANDARD_DESTINATION_INDEX];
    svc->requester = request[RE_STANDARD_SOURCE_INDEX];
    svc->started_ms = now_ms;
    svc->frame_started_ms = now_ms;
    if (request[RE_STANDARD_OPERATION_INDEX] == RE_STANDARD_LOG_VALUE_READ) {
        rc = ruuvi_log_start(&svc->iterator, request, len, synthetic_now_s);
        if (rc < 0) {
            ruuvi_log_service_abort(svc);
            return rc;
        }
        svc->read = true;
    } else if (svc->destination == RE_STANDARD_DESTINATION_PASSWORD &&
               request[RE_STANDARD_OPERATION_INDEX] == RE_STANDARD_VALUE_READ) {
        uint8_t difference = 0U;

        if (device_id != NULL) {
            for (size_t i = 0; i < RE_STANDARD_PAYLOAD_LENGTH; ++i) {
                difference |= request[RE_STANDARD_PAYLOAD_START_INDEX + i] ^ device_id[i];
            }
        }
        svc->password_match = device_id != NULL && difference == 0U;
        terminal_frame(svc, svc->password_match ? RE_STANDARD_VALUE_WRITE :
                       RE_STANDARD_OP_UNAUTHORIZED);
        if (svc->password_match) {
            memcpy(svc->pending + RE_STANDARD_PAYLOAD_START_INDEX,
                   request + RE_STANDARD_PAYLOAD_START_INDEX, RE_STANDARD_PAYLOAD_LENGTH);
        }
    } else {
        terminal_frame(svc, RE_STANDARD_OP_UNAUTHORIZED);
    }
    svc->active = true;
    return 0;
}

int ruuvi_log_service_start(ruuvi_log_service_t *svc,
                             const uint8_t request[RUUVI_LOG_MESSAGE_LENGTH],
                             size_t len, uint64_t synthetic_now_s, int64_t now_ms)
{
    return ruuvi_log_service_start_with_id(svc, request, len, synthetic_now_s, now_ms,
                                           NULL);
}

int ruuvi_log_service_pump(ruuvi_log_service_t *svc, int64_t now_ms,
                           int (*send)(void *, const uint8_t *, size_t), void *ctx)
{
    int rc;

    if (svc == NULL || send == NULL) {
        return -EINVAL;
    }
    if (!svc->active) {
        return 0;
    }
    if (now_ms < svc->started_ms) {
        ruuvi_log_service_abort(svc);
        return -EINVAL;
    }
    if (svc->read && !svc->timed_out &&
        (uint64_t)now_ms - (uint64_t)svc->started_ms >= RUUVI_LOG_SERVICE_TIMEOUT_MS) {
        /* Discard even an unsent data/EOF frame once the read has expired. */
        terminal_frame(svc, RE_STANDARD_OP_TIMEOUT);
        svc->frame_started_ms = now_ms;
        svc->timed_out = true;
    }
    if (svc->pending_valid &&
        (uint64_t)now_ms - (uint64_t)svc->frame_started_ms >= RUUVI_LOG_FRAME_TIMEOUT_MS) {
        ruuvi_log_service_abort(svc);
        return -ETIMEDOUT;
    }
    if (!svc->pending_valid) {
        rc = ruuvi_log_next(&svc->iterator, svc->pending);
        if (rc < 0) {
            ruuvi_log_service_abort(svc);
            return rc;
        }
        if (rc == 0) {
            ruuvi_log_service_abort(svc);
            return 0;
        }
        svc->pending_valid = true;
        svc->frame_started_ms = now_ms;
    }
    rc = send(ctx, svc->pending, sizeof(svc->pending));
    if (rc == -EAGAIN) {
        return 0;
    }
    if (rc < 0) {
        ruuvi_log_service_abort(svc);
        return rc;
    }
    if (svc->timed_out || !svc->read || !svc->iterator.active) {
        ruuvi_log_service_abort(svc);
    } else {
        svc->pending_valid = false;
    }
    return 1;
}
