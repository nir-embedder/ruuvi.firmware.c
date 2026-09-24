#include "../src/log_service.h"
#include "../../nrf52_oldsdk/src/ruuvi.endpoints.c/src/ruuvi_endpoints.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>

static ruuvi_history_element_t records[2];
static size_t record_count;
static unsigned int reads;
static int read_error;

typedef struct {
    uint8_t attempts[32][RUUVI_LOG_MESSAGE_LENGTH];
    uint8_t accepted[32][RUUVI_LOG_MESSAGE_LENGTH];
    size_t attempt_count;
    size_t accepted_count;
    unsigned int busy;
    int failure;
} fake_tx_t;

int ruuvi_history_read(uint32_t min_timestamp_s, uint32_t index,
                       ruuvi_history_element_t *sample)
{
    ++reads;
    if (read_error != 0) {
        return read_error;
    }
    for (size_t i = 0; i < record_count; ++i) {
        if (records[i].timestamp_s >= min_timestamp_s) {
            if (index == 0U) {
                *sample = records[i];
                return 0;
            }
            --index;
        }
    }
    return -ENOENT;
}

static int send_frame(void *ctx, const uint8_t *frame, size_t length)
{
    fake_tx_t *tx = ctx;
    assert(length == RUUVI_LOG_MESSAGE_LENGTH);
    assert(tx->attempt_count < 32 && tx->accepted_count < 32);
    memcpy(tx->attempts[tx->attempt_count++], frame, length);
    if (tx->busy != 0U) {
        --tx->busy;
        return -EAGAIN;
    }
    if (tx->failure != 0) {
        return tx->failure;
    }
    memcpy(tx->accepted[tx->accepted_count++], frame, length);
    return 0;
}

static void put_u32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)(value >> 24);
    p[1] = (uint8_t)(value >> 16);
    p[2] = (uint8_t)(value >> 8);
    p[3] = (uint8_t)value;
}

static void request(uint8_t *req, uint8_t target, uint8_t op,
                    uint32_t current, uint32_t start)
{
    req[0] = target;
    req[1] = 0xA5;
    req[2] = op;
    put_u32(req + 3, current);
    put_u32(req + 7, start);
}

static void reset(void)
{
    record_count = 0;
    reads = 0;
    read_error = 0;
}

static void check_terminal(const uint8_t *frame, uint8_t target, uint8_t op)
{
    uint8_t expected[RUUVI_LOG_MESSAGE_LENGTH] = {0xA5, target, op};
    memset(expected + 3, 0xFF, 8);
    assert(memcmp(frame, expected, sizeof(expected)) == 0);
}

static void test_retry_and_legacy_vector(void)
{
    ruuvi_log_service_t svc = {0};
    fake_tx_t tx = {0};
    uint8_t req[11];
    const uint8_t humidity[11] = {
        0xA5, 0x31, 0x10, 0x00, 0x00, 0x03, 0xAC,
        0x00, 0x00, 0x16, 0x2E,
    };
    const uint8_t pressure[11] = {
        0xA5, 0x32, 0x10, 0x00, 0x00, 0x03, 0xAC,
        0x00, 0x01, 0x8B, 0xCD,
    };
    const uint8_t temperature[11] = {
        0xA5, 0x30, 0x10, 0x00, 0x00, 0x03, 0xAC,
        0x00, 0x00, 0x09, 0x29,
    };

    reset();
    records[record_count++] = (ruuvi_history_element_t) {
        .timestamp_s = 240, .temperature_c = 23.45f,
        .humidity_rh = 56.78f, .pressure_pa = 101325.0f,
    };
    request(req, 0x3A, RE_STANDARD_LOG_VALUE_READ, 1000, 900);
    assert(ruuvi_log_service_start(&svc, req, sizeof(req), 300, 100) == 0);
    assert(ruuvi_log_service_active(&svc) && reads == 0);
    assert(ruuvi_log_service_start(&svc, req, sizeof(req), 300, 100) == -EBUSY);
    tx.busy = 2;
    assert(ruuvi_log_service_pump(&svc, 100, send_frame, &tx) == 0);
    assert(ruuvi_log_service_pump(&svc, 101, send_frame, &tx) == 0);
    assert(reads == 1 && tx.accepted_count == 0 && tx.attempt_count == 2);
    assert(memcmp(tx.attempts[0], tx.attempts[1], 11) == 0);
    assert(ruuvi_log_service_pump(&svc, 102, send_frame, &tx) == 1);
    assert(reads == 1 && tx.accepted_count == 1);
    for (size_t i = 0; i < 3; ++i) {
        assert(ruuvi_log_service_pump(&svc, 103 + (int64_t)i, send_frame, &tx) == 1);
    }
    assert(tx.accepted_count == 4);
    assert(memcmp(tx.accepted[0], humidity, 11) == 0);
    assert(memcmp(tx.accepted[1], pressure, 11) == 0);
    assert(memcmp(tx.accepted[2], temperature, 11) == 0);
    check_terminal(tx.accepted[3], 0x3A, RE_STANDARD_LOG_VALUE_WRITE);
    assert(!ruuvi_log_service_active(&svc) && reads == 2);
    assert(ruuvi_log_service_pump(&svc, 106, send_frame, &tx) == 0);
    assert(tx.accepted_count == 4);
    /* Compare to frames produced by the existing endpoint encoder. */
    uint8_t encoded[11];
    assert(re_log_write_header(encoded, 0x31) == RE_SUCCESS);
    assert(re_log_write_timestamp(encoded, 940000) == RE_SUCCESS);
    assert(re_log_write_data(encoded, 56.78f, 0x31) == RE_SUCCESS);
    encoded[0] = 0xA5;
    assert(memcmp(encoded, humidity, 11) == 0);
}

static void test_empty_read_and_unauthorized(void)
{
    ruuvi_log_service_t svc = {0};
    fake_tx_t tx = {0};
    uint8_t req[11];
    reset();
    request(req, 0x30, RE_STANDARD_LOG_VALUE_READ, 500, 400);
    assert(ruuvi_log_service_start(&svc, req, 11, 100, 0) == 0);
    assert(ruuvi_log_service_pump(&svc, 0, send_frame, &tx) == 1);
    check_terminal(tx.accepted[0], 0x30, 0x10);
    assert(!ruuvi_log_service_active(&svc) && reads == 1);

    const uint8_t writes[] = {0x02, 0x04, 0x06, 0x08, 0x10, 0x20, 0xF2, 0xEA};
    for (size_t i = 0; i < sizeof(writes); ++i) {
        request(req, 0x3A, writes[i], 500, 400);
        tx = (fake_tx_t) {0};
        tx.busy = 1;
        assert(ruuvi_log_service_start(&svc, req, 11, 100, 0) == 0);
        assert(ruuvi_log_service_pump(&svc, 0, send_frame, &tx) == 0);
        assert(ruuvi_log_service_active(&svc));
        assert(ruuvi_log_service_pump(&svc, 1, send_frame, &tx) == 1);
        check_terminal(tx.accepted[0], 0x3A, 0xEA);
        assert(!ruuvi_log_service_active(&svc) && reads == 1);
    }
    request(req, RE_STANDARD_DESTINATION_PASSWORD, RE_STANDARD_VALUE_READ, 500, 400);
    assert(ruuvi_log_service_start(&svc, req, 11, 100, 0) == 0);
    assert(ruuvi_log_service_pump(&svc, 0, send_frame, &tx) == 1);
    check_terminal(tx.accepted[1], RE_STANDARD_DESTINATION_PASSWORD, 0xEA);
    assert(!ruuvi_log_service_active(&svc) && reads == 1);
}

static void test_log_read_without_history_partition(void)
{
    ruuvi_log_service_t svc = {0};
    fake_tx_t tx = {0};
    uint8_t req[11];

    reset();
    records[record_count++] = (ruuvi_history_element_t) {
        .timestamp_s = 100, .temperature_c = 20.0f,
        .humidity_rh = 50.0f, .pressure_pa = 101325.0f,
    };
    read_error = -EIO; /* No history backend must be called. */
    request(req, RE_STANDARD_DESTINATION_ENVIRONMENTAL,
            RE_STANDARD_LOG_VALUE_READ, 200, 100);
    assert(ruuvi_log_service_start_with_id(&svc, req, sizeof(req), 100, 0,
                                           NULL, false) == 0);
    tx.busy = 1;
    assert(ruuvi_log_service_pump(&svc, 0, send_frame, &tx) == 0);
    check_terminal(tx.attempts[0], req[0], RE_STANDARD_LOG_VALUE_WRITE);
    assert(ruuvi_log_service_pump(&svc, 1, send_frame, &tx) == 1);
    check_terminal(tx.accepted[0], req[0], RE_STANDARD_LOG_VALUE_WRITE);
    assert(!ruuvi_log_service_active(&svc) && reads == 0);

    tx = (fake_tx_t) {0};
    request(req, RE_STANDARD_DESTINATION_TEMPERATURE,
            RE_STANDARD_LOG_VALUE_READ, 200, 100);
    assert(ruuvi_log_service_start_with_id(&svc, req, sizeof(req), 100, 2,
                                           NULL, false) == 0);
    assert(ruuvi_log_service_pump(&svc, 2, send_frame, &tx) == 1);
    check_terminal(tx.accepted[0], req[0], RE_STANDARD_LOG_VALUE_WRITE);
    assert(reads == 0);

    request(req, RE_STANDARD_DESTINATION_TEMPERATURE,
            RE_STANDARD_LOG_VALUE_READ, 100, 100);
    assert(ruuvi_log_service_start_with_id(&svc, req, sizeof(req), 100, 3,
                                           NULL, false) == -EINVAL);
    assert(!ruuvi_log_service_active(&svc) && reads == 0);
}

static void test_legacy_password_reply(void)
{
    const uint8_t device_id[8] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
    const uint8_t expected[11] = {
        0xA5, RE_STANDARD_DESTINATION_PASSWORD, RE_STANDARD_VALUE_WRITE,
        0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
    };
    ruuvi_log_service_t svc = {0};
    fake_tx_t tx = {0};
    uint8_t req[11] = {
        RE_STANDARD_DESTINATION_PASSWORD, 0xA5, RE_STANDARD_VALUE_READ,
        0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
    };

    reset();
    assert(ruuvi_log_service_start_with_id(&svc, req, sizeof(req), 0, 100,
                                           device_id, true) == 0);
    assert(svc.password_match && ruuvi_log_service_active(&svc));
    tx.busy = 1;
    assert(ruuvi_log_service_pump(&svc, 100, send_frame, &tx) == 0);
    assert(memcmp(tx.attempts[0], expected, sizeof(expected)) == 0);
    assert(ruuvi_log_service_pump(&svc, 101, send_frame, &tx) == 1);
    assert(memcmp(tx.accepted[0], expected, sizeof(expected)) == 0);
    assert(!ruuvi_log_service_active(&svc) && !svc.password_match && reads == 0);

    req[10] ^= 1U;
    assert(ruuvi_log_service_start_with_id(&svc, req, sizeof(req), 0, 102,
                                           device_id, true) == 0);
    assert(!svc.password_match);
    assert(ruuvi_log_service_pump(&svc, 102, send_frame, &tx) == 1);
    check_terminal(tx.accepted[1], RE_STANDARD_DESTINATION_PASSWORD,
                   RE_STANDARD_OP_UNAUTHORIZED);
    assert(!ruuvi_log_service_active(&svc));

    req[10] ^= 1U;
    assert(ruuvi_log_service_start_with_id(&svc, req, sizeof(req), 0, 103,
                                            NULL, true) == 0);
    assert(!svc.password_match);
    assert(ruuvi_log_service_pump(&svc, 103, send_frame, &tx) == 1);
    check_terminal(tx.accepted[2], RE_STANDARD_DESTINATION_PASSWORD,
                   RE_STANDARD_OP_UNAUTHORIZED);
    req[2] = RE_STANDARD_VALUE_WRITE;
    assert(ruuvi_log_service_start_with_id(&svc, req, sizeof(req), 0, 104,
                                           device_id, true) == 0);
    assert(!svc.password_match);
    assert(ruuvi_log_service_pump(&svc, 104, send_frame, &tx) == 1);
    check_terminal(tx.accepted[3], RE_STANDARD_DESTINATION_PASSWORD,
                   RE_STANDARD_OP_UNAUTHORIZED);
}

static void test_motion_reads_without_motion_history(void)
{
    static const uint8_t destinations[] = {
        RE_STANDARD_DESTINATION_ACCELERATION,
        RE_STANDARD_DESTINATION_ACCELERATION_X,
        RE_STANDARD_DESTINATION_ACCELERATION_Y,
        RE_STANDARD_DESTINATION_ACCELERATION_Z,
        RE_STANDARD_DESTINATION_GYRATION,
        RE_STANDARD_DESTINATION_GYRATION_X,
        RE_STANDARD_DESTINATION_GYRATION_Y,
        RE_STANDARD_DESTINATION_GYRATION_Z,
    };
    ruuvi_log_service_t svc = {0};
    uint8_t req[11];

    reset();
    records[record_count++] = (ruuvi_history_element_t) {
        .timestamp_s = 100, .temperature_c = 20.0f,
        .humidity_rh = 50.0f, .pressure_pa = 101325.0f,
    };
    for (size_t i = 0; i < sizeof(destinations); ++i) {
        fake_tx_t tx = {0};
        request(req, destinations[i], RE_STANDARD_LOG_VALUE_READ, 200, 100);
        assert(ruuvi_log_service_start(&svc, req, sizeof(req), 100, 0) == 0);
        assert(ruuvi_log_service_pump(&svc, 0, send_frame, &tx) == 1);
        assert(tx.accepted_count == 1);
        check_terminal(tx.accepted[0], destinations[i], RE_STANDARD_LOG_VALUE_WRITE);
        assert(!ruuvi_log_service_active(&svc));
    }
    assert(reads == 0);
}

static void test_bad_requests_and_failures(void)
{
    ruuvi_log_service_t svc = {0};
    fake_tx_t tx = {0};
    uint8_t req[12];
    reset();
    request(req, 0x30, 0x11, 500, 400);
    assert(ruuvi_log_service_start(NULL, req, 11, 100, 0) == -EINVAL);
    assert(ruuvi_log_service_start(&svc, NULL, 11, 100, 0) == -EINVAL);
    assert(ruuvi_log_service_start(&svc, req, 10, 100, 0) == -EMSGSIZE);
    assert(ruuvi_log_service_start(&svc, req, 12, 100, 0) == -EMSGSIZE);
    request(req, 0x30, 0x11, 400, 400);
    assert(ruuvi_log_service_start(&svc, req, 11, 100, 0) == -EINVAL);
    request(req, 0x30, 0x11, 399, 400);
    assert(ruuvi_log_service_start(&svc, req, 11, 100, 0) == -EINVAL);
    request(req, 0x46, 0x11, 500, 400); /* Not a legacy sensor endpoint. */
    assert(ruuvi_log_service_start(&svc, req, 11, 100, 0) == -ENOTSUP);
    request(req, 0x30, 0x13, 500, 400);
    assert(ruuvi_log_service_start(&svc, req, 11, 100, 0) == -ENOTSUP);
    request(req, 0x30, RE_STANDARD_VALUE_READ, 500, 400);
    assert(ruuvi_log_service_start(&svc, req, 11, 100, 0) == -ENOTSUP);
    assert(!ruuvi_log_service_active(&svc) && tx.attempt_count == 0 && reads == 0);

    request(req, 0x30, 0x11, 500, 400);
    assert(ruuvi_log_service_start(&svc, req, 11, 100, 0) == 0);
    read_error = -EIO;
    assert(ruuvi_log_service_pump(&svc, 1, send_frame, &tx) == -EIO);
    assert(!ruuvi_log_service_active(&svc) && tx.attempt_count == 0);
    read_error = 0;
    assert(ruuvi_log_service_start(&svc, req, 11, 100, 2) == 0);
    tx.failure = -EIO;
    assert(ruuvi_log_service_pump(&svc, 2, send_frame, &tx) == -EIO);
    assert(!ruuvi_log_service_active(&svc));
    assert(ruuvi_log_service_pump(&svc, 3, send_frame, &tx) == 0);
    assert(ruuvi_log_service_pump(NULL, 3, send_frame, &tx) == -EINVAL);
    assert(ruuvi_log_service_pump(&svc, 3, NULL, &tx) == -EINVAL);
}

static void test_timeout_and_abort(void)
{
    ruuvi_log_service_t svc = {0};
    fake_tx_t tx = {0};
    uint8_t req[11];
    reset();
    records[record_count++] = (ruuvi_history_element_t) {
        .timestamp_s = 100, .temperature_c = 20.0f,
        .humidity_rh = 50.0f, .pressure_pa = 101325.0f,
    };
    request(req, 0x30, 0x11, 500, 400);
    assert(ruuvi_log_service_start(&svc, req, 11, 100, 10) == 0);
    tx.busy = 1;
    assert(ruuvi_log_service_pump(&svc, 11, send_frame, &tx) == 0);
    assert(reads == 1);
    assert(ruuvi_log_service_pump(&svc, 12, send_frame, &tx) == 1);
    assert(tx.accepted_count == 1 && tx.accepted[0][2] == 0x10);
    tx.busy = 2;
    assert(ruuvi_log_service_pump(&svc, 300010, send_frame, &tx) == 0);
    assert(ruuvi_log_service_pump(&svc, 300011, send_frame, &tx) == 0);
    assert(ruuvi_log_service_active(&svc) && reads == 1);
    assert(memcmp(tx.attempts[2], tx.attempts[3], 11) == 0);
    check_terminal(tx.attempts[2], 0x30, 0xE0);
    assert(ruuvi_log_service_pump(&svc, 300012, send_frame, &tx) == 1);
    check_terminal(tx.accepted[1], 0x30, 0xE0);
    assert(!ruuvi_log_service_active(&svc));
    assert(ruuvi_log_service_pump(&svc, 300013, send_frame, &tx) == 0);
    assert(tx.accepted_count == 2);

    tx = (fake_tx_t) {0};
    assert(ruuvi_log_service_start(&svc, req, 11, 100, INT64_MIN) == 0);
    assert(ruuvi_log_service_pump(&svc, INT64_MAX, send_frame, &tx) == 1);
    check_terminal(tx.accepted[0], 0x30, 0xE0);
    assert(!ruuvi_log_service_active(&svc) && reads == 1);
    assert(ruuvi_log_service_start(&svc, req, 11, 100, 100) == 0);
    assert(ruuvi_log_service_pump(&svc, 99, send_frame, &tx) == -EINVAL);
    assert(!ruuvi_log_service_active(&svc));
    assert(ruuvi_log_service_start(&svc, req, 11, 100, 0) == 0);
    tx.busy = 1;
    assert(ruuvi_log_service_pump(&svc, 0, send_frame, &tx) == 0);
    assert(ruuvi_log_service_active(&svc));
    ruuvi_log_service_abort(&svc);
    assert(!ruuvi_log_service_active(&svc));
    assert(ruuvi_log_service_pump(&svc, 300000, send_frame, &tx) == 0);
    assert(tx.accepted_count == 1);

    tx = (fake_tx_t) {0};
    tx.busy = 3;
    assert(ruuvi_log_service_start(&svc, req, 11, 100, 1000) == 0);
    assert(ruuvi_log_service_pump(&svc, 1000, send_frame, &tx) == 0);
    assert(ruuvi_log_service_pump(&svc, 4999, send_frame, &tx) == 0);
    assert(ruuvi_log_service_pump(&svc, 5000, send_frame, &tx) == -ETIMEDOUT);
    assert(!ruuvi_log_service_active(&svc) && tx.accepted_count == 0);
    ruuvi_log_service_abort(NULL);
    assert(!ruuvi_log_service_active(NULL));
}

int main(void)
{
    test_retry_and_legacy_vector();
    test_empty_read_and_unauthorized();
    test_log_read_without_history_partition();
    test_legacy_password_reply();
    test_motion_reads_without_motion_history();
    test_bad_requests_and_failures();
    test_timeout_and_abort();
    return 0;
}
