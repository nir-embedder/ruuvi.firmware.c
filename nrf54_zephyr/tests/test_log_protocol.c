#include "../src/log_protocol.h"
#include "../../nrf52_oldsdk/src/ruuvi.endpoints.c/src/ruuvi_endpoints.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

static ruuvi_history_element_t records[8];
static size_t record_count;
static unsigned int reads;
static int read_error;

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

static void reset(void)
{
    memset(records, 0, sizeof(records));
    record_count = 0;
    reads = 0;
    read_error = 0;
}

static void add(uint32_t timestamp_s, float temperature_c,
                float humidity_rh, float pressure_pa)
{
    assert(record_count < sizeof(records) / sizeof(records[0]));
    records[record_count++] = (ruuvi_history_element_t) {
        .timestamp_s = timestamp_s,
        .temperature_c = temperature_c,
        .humidity_rh = humidity_rh,
        .pressure_pa = pressure_pa,
    };
}

static void put_u32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)(value >> 24);
    p[1] = (uint8_t)(value >> 16);
    p[2] = (uint8_t)(value >> 8);
    p[3] = (uint8_t)value;
}

static void request(uint8_t *p, uint8_t target, uint32_t current, uint32_t start)
{
    p[0] = target;
    p[1] = 0xA5;
    p[2] = RE_STANDARD_LOG_VALUE_READ;
    put_u32(p + 3, current);
    put_u32(p + 7, start);
}

static void frame_eq(const uint8_t *frame, const uint8_t *expected)
{
    assert(memcmp(frame, expected, RUUVI_LOG_MESSAGE_LENGTH) == 0);
}

static void check_eof(ruuvi_log_iterator_t *it, uint8_t *frame, uint8_t target)
{
    const uint8_t eof[11] = {
        0xA5, target, 0x10, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF,
    };
    assert(ruuvi_log_next(it, frame) == 1);
    frame_eq(frame, eof);
    assert(ruuvi_log_next(it, frame) == 0);
    frame_eq(frame, eof);
}

static void test_environment_and_resume(void)
{
    uint8_t req[11], frame[11];
    ruuvi_log_iterator_t it;
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
    add(190, 4, 5, 6);
    add(240, 23.45f, 56.78f, 101325.0f);
    add(300, 24.0f, 50.0f, 101000.0f);
    request(req, 0x3A, 1000, 900);
    assert(ruuvi_log_start(&it, req, sizeof(req), 300) == 0);
    assert(reads == 0);
    assert(ruuvi_log_next(&it, frame) == 1);
    frame_eq(frame, humidity);
    assert(reads == 1);
    /* A worker may pause here until it can send the previous frame. */
    assert(ruuvi_log_next(&it, frame) == 1);
    frame_eq(frame, pressure);
    assert(reads == 1);
    assert(ruuvi_log_next(&it, frame) == 1);
    frame_eq(frame, temperature);
    assert(reads == 1);
    assert(ruuvi_log_next(&it, frame) == 1);
    assert(frame[1] == 0x31 && frame[6] == 0xE8); /* Next sample at 1000 s. */
    assert(reads == 2);
    assert(ruuvi_log_next(&it, frame) == 1 && frame[1] == 0x32);
    assert(ruuvi_log_next(&it, frame) == 1 && frame[1] == 0x30);
    check_eof(&it, frame, 0x3A);
    assert(reads == 3);
}

static void test_single_destinations(void)
{
    uint8_t req[11], frame[11];
    ruuvi_log_iterator_t it;
    reset();
    add(100, -12.34f, 45.0f, 101325.0f);
    for (uint8_t destination = 0x30; destination <= 0x32; ++destination) {
        request(req, destination, 200, 100);
        assert(ruuvi_log_start(&it, req, sizeof(req), 100) == 0);
        assert(ruuvi_log_next(&it, frame) == 1);
        assert(frame[0] == 0xA5 && frame[1] == destination && frame[2] == 0x10);
        assert(frame[3] == 0 && frame[4] == 0 && frame[5] == 0 && frame[6] == 200);
        if (destination == 0x30) {
            const uint8_t negative[4] = {0xFF, 0xFF, 0xFB, 0x2E};
            assert(memcmp(frame + 7, negative, sizeof(negative)) == 0);
        } else if (destination == 0x31) {
            const uint8_t rh[4] = {0, 0, 0x11, 0x94};
            assert(memcmp(frame + 7, rh, sizeof(rh)) == 0);
        } else {
            const uint8_t pa[4] = {0, 1, 0x8B, 0xCD};
            assert(memcmp(frame + 7, pa, sizeof(pa)) == 0);
        }
        check_eof(&it, frame, destination);
    }
}

static void test_motion_destinations_without_motion_history(void)
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
    uint8_t req[11], frame[11];
    ruuvi_log_iterator_t it;

    reset();
    add(100, 20.0f, 50.0f, 101325.0f);
    for (size_t i = 0; i < sizeof(destinations); ++i) {
        request(req, destinations[i], 200, 100);
        assert(ruuvi_log_start(&it, req, sizeof(req), 100) == 0);
        check_eof(&it, frame, destinations[i]);
    }
    assert(reads == 0); /* Environmental samples must not become motion data. */
}

static void test_invalid_requests(void)
{
    uint8_t req[12], frame[11];
    ruuvi_log_iterator_t it;
    reset();
    request(req, 0x3A, 100, 90);
    assert(ruuvi_log_start(NULL, req, 11, 100) == -EINVAL);
    assert(ruuvi_log_start(&it, NULL, 11, 100) == -EINVAL);
    assert(ruuvi_log_start(&it, req, 10, 100) == -EMSGSIZE);
    assert(ruuvi_log_start(&it, req, 12, 100) == -EMSGSIZE);
    req[2] = RE_STANDARD_LOG_VALUE_WRITE;
    assert(ruuvi_log_start(&it, req, 11, 100) == -ENOTSUP);
    req[2] = RE_STANDARD_VALUE_READ;
    assert(ruuvi_log_start(&it, req, 11, 100) == -ENOTSUP);
    req[2] = RE_STANDARD_LOG_VALUE_READ;
    req[0] = 0x46; /* Not a legacy sensor endpoint. */
    assert(ruuvi_log_start(&it, req, 11, 100) == -ENOTSUP);
    req[0] = 0x3A;
    request(req, 0x3A, 99, 99);
    assert(ruuvi_log_start(&it, req, 11, 100) == -EINVAL);
    request(req, 0x3A, 98, 99);
    assert(ruuvi_log_start(&it, req, 11, 100) == -EINVAL);
    assert(reads == 0);
    assert(ruuvi_log_next(&it, frame) == -EINVAL);
    assert(ruuvi_log_next(NULL, frame) == -EINVAL);
    assert(ruuvi_log_next(&it, NULL) == -EINVAL);
}

static void test_empty_and_failure(void)
{
    uint8_t req[11], frame[11];
    ruuvi_log_iterator_t it;
    reset();
    request(req, 0x30, 500, 400);
    assert(ruuvi_log_start(&it, req, 11, 100) == 0);
    check_eof(&it, frame, 0x30);
    assert(reads == 1);
    add(100, 20.0f, 30.0f, 100000.0f);
    assert(ruuvi_log_start(&it, req, 11, 100) == 0);
    read_error = -EACCES;
    memset(frame, 0x55, sizeof(frame));
    assert(ruuvi_log_next(&it, frame) == -EACCES);
    for (size_t i = 0; i < sizeof(frame); ++i) {
        assert(frame[i] == 0x55);
    }
    read_error = 0;
    assert(ruuvi_log_next(&it, frame) == 1 && frame[1] == 0x30);
    check_eof(&it, frame, 0x30);
}

static void test_invalid_samples_and_wrap(void)
{
    uint8_t req[11], frame[11];
    ruuvi_log_iterator_t it;
    reset();
    add(10, NAN, INFINITY, 1.0e30f);
    add(11, 1.0e30f, NAN, -INFINITY);
    request(req, 0x3A, 100, 90);
    assert(ruuvi_log_start(&it, req, 11, 11) == 0);
    check_eof(&it, frame, 0x3A);
    assert(reads == 3);

    reset();
    add(10, NAN, INFINITY, 101325.0f);
    request(req, 0x3A, 100, 90);
    assert(ruuvi_log_start(&it, req, 11, 10) == 0);
    assert(ruuvi_log_next(&it, frame) == 1 && frame[1] == 0x32);
    check_eof(&it, frame, 0x3A);

    reset();
    add(12, 20.0f, 30.0f, 101000.0f);
    request(req, 0x3A, 100, 90);
    assert(ruuvi_log_start(&it, req, 11, 11) == 0);
    check_eof(&it, frame, 0x3A); /* Do not report future data. */

    reset();
    add(UINT32_MAX, 1.0f, 2.0f, 3.0f);
    request(req, 0x30, UINT32_MAX, UINT32_MAX - 2U);
    assert(ruuvi_log_start(&it, req, 11, UINT32_MAX) == 0);
    assert(ruuvi_log_next(&it, frame) == 1);
    const uint8_t max_ts[4] = {0xFF, 0xFF, 0xFF, 0xFF};
    assert(memcmp(frame + 3, max_ts, 4) == 0);
    check_eof(&it, frame, 0x30);

    /* The synthetic clock does not wrap back to zero when translating time. */
    reset();
    add(UINT32_MAX, 1.0f, 2.0f, 3.0f);
    request(req, 0x30, 2, 1);
    assert(ruuvi_log_start(&it, req, 11, (uint64_t)UINT32_MAX + 1U) == 0);
    assert(ruuvi_log_next(&it, frame) == 1);
    assert(frame[3] == 0 && frame[4] == 0 && frame[5] == 0 && frame[6] == 1);
    check_eof(&it, frame, 0x30);

    reset();
    request(req, 0x30, 2, 1);
    assert(ruuvi_log_start(&it, req, 11, UINT64_MAX) == 0);
    check_eof(&it, frame, 0x30);
    assert(reads == 0);
}

int main(void)
{
    test_environment_and_resume();
    test_single_destinations();
    test_motion_destinations_without_motion_history();
    test_invalid_requests();
    test_empty_and_failure();
    test_invalid_samples_and_wrap();
    return 0;
}
