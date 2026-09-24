#include "history.h"
#include "history_flash.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* The host fake stores real codec payloads, but needs no Zephyr flash driver. */
static uint8_t bytes[RUUVI_HISTORY_RECORD_SLOTS][RUUVI_HISTORY_RECORD_SIZE];
static uint32_t generations[RUUVI_HISTORY_RECORD_SLOTS];
static bool occupied[RUUVI_HISTORY_RECORD_SLOTS];
static bool torn[RUUVI_HISTORY_RECORD_SLOTS];
static bool fail_read;
static bool fail_write;
static bool fail_clear;
static unsigned int mounts;
static unsigned int writes;
static ruuvi_history_record_t seeded;
static ruuvi_history_element_t result;

int ruuvi_history_flash_init(void)
{
    ++mounts;
    return 0;
}

int ruuvi_history_flash_read(uint8_t slot, ruuvi_history_record_t *record,
                             uint32_t *generation)
{
    if (fail_read) {
        return -EIO;
    }
    if (!occupied[slot] || torn[slot]) {
        return -ENOENT;
    }
    int rc = ruuvi_history_decode(bytes[slot], sizeof(bytes[slot]), record);
    if (rc < 0) {
        return -ENOENT;
    }
    if (generation != NULL) {
        *generation = generations[slot];
    }
    return 0;
}

int ruuvi_history_flash_write(uint8_t slot, const ruuvi_history_record_t *record,
                              uint32_t generation)
{
    if (fail_write || torn[slot]) {
        return -EIO;
    }
    int rc = ruuvi_history_encode(record, bytes[slot], sizeof(bytes[slot]));
    if (rc == 0) {
        generations[slot] = generation;
        occupied[slot] = true;
        torn[slot] = false;
        ++writes;
    }
    return rc;
}

int ruuvi_history_flash_clear(void)
{
    if (fail_clear) {
        return -EIO;
    }
    memset(occupied, 0, sizeof(occupied));
    memset(torn, 0, sizeof(torn));
    return 0;
}

static void reset_flash(void)
{
    memset(occupied, 0, sizeof(occupied));
    memset(torn, 0, sizeof(torn));
    fail_read = false;
    fail_write = false;
    fail_clear = false;
    mounts = 0;
    writes = 0;
    assert(ruuvi_history_init() == 0);
}

static ruuvi_history_element_t element(uint32_t timestamp)
{
    return (ruuvi_history_element_t) {
        .timestamp_s = timestamp,
        .temperature_c = (float)timestamp,
        .humidity_rh = 44.0f,
        .pressure_pa = 100000.0f,
    };
}

static int add(uint32_t timestamp)
{
    ruuvi_history_element_t sample = element(timestamp);
    return ruuvi_history_process(&sample);
}

static void set_config(uint16_t interval, bool overflow)
{
    ruuvi_history_config_t config = {
        .interval_s = interval,
        .overflow = overflow,
        .fields = RUUVI_HISTORY_DEFAULT_FIELDS,
    };
    assert(ruuvi_history_config_set(&config) == 0);
}

static void seed(uint8_t slot, uint32_t generation, uint32_t timestamp)
{
    memset(&seeded, 0, sizeof(seeded));
    seeded.start_timestamp_s = timestamp;
    seeded.end_timestamp_s = timestamp;
    seeded.num_samples = 1;
    seeded.block_configuration.interval_s = 1;
    seeded.block_configuration.overflow = true;
    seeded.block_configuration.fields = RUUVI_HISTORY_DEFAULT_FIELDS;
    seeded.storage[0] = element(timestamp);
    assert(ruuvi_history_flash_write(slot, &seeded, generation) == 0);
}

static void check(uint32_t after, uint32_t index, uint32_t timestamp)
{
    assert(ruuvi_history_read(after, index, &result) == 0);
    assert(result.timestamp_s == timestamp);
    assert(result.temperature_c == (float)timestamp);
    assert(result.humidity_rh == 44.0f);
    assert(result.pressure_pa == 100000.0f);
}

static void test_defaults_interval_and_config(void)
{
    ruuvi_history_config_t config;
    reset_flash();
    assert(mounts == 1);
    uint32_t latest = 0;
    assert(ruuvi_history_latest_timestamp(NULL) == -EINVAL);
    assert(ruuvi_history_latest_timestamp(&latest) == -ENOENT);
    assert(ruuvi_history_config_get(&config) == 0);
    assert(config.interval_s == 300 && config.overflow &&
           config.fields == RUUVI_HISTORY_DEFAULT_FIELDS);
    assert(ruuvi_history_process(NULL) == -EINVAL);
    assert(ruuvi_history_read(0, 0, NULL) == -EINVAL);
    assert(add(0) == 1);
    assert(add(299) == 0);
    assert(add(300) == 1);
    assert(ruuvi_history_latest_timestamp(&latest) == 0 && latest == 300U);
    assert(add(299) == -EINVAL);
    check(0, 0, 0);
    check(1, 0, 300);
    assert(ruuvi_history_read(301, 0, &result) == -ENOENT);
    assert(writes == 0); /* RAM data has not been persisted. */
    assert(ruuvi_history_flush() == 0);
    assert(writes == 1 && occupied[0] && generations[0] == 0);
    assert(ruuvi_history_init() == 0);
    assert(ruuvi_history_latest_timestamp(&latest) == 0 && latest == 300U);
    check(0, 1, 300);
    assert(add(299) == -EINVAL); /* Maintain timestamp order across reboot. */
    assert(ruuvi_history_config_get(&config) == 0 && config.interval_s == 300);
    config.interval_s = 0;
    assert(ruuvi_history_config_set(&config) == -EINVAL);
    set_config(1, true);
    assert(add(301) == 1);
    config.interval_s = 2;
    assert(ruuvi_history_config_set(&config) == 0); /* Flush old config first. */
    assert(ruuvi_history_flash_read(1, &seeded, NULL) == 0);
    assert(seeded.num_samples == 1 && seeded.block_configuration.interval_s == 1);
    assert(add(302) == 0);
    assert(add(303) == 1);
    assert(ruuvi_history_config_get(&config) == 0 && config.interval_s == 2);
    assert(ruuvi_history_init() == 0); /* Drop only unflushed sample. */
    assert(ruuvi_history_config_get(&config) == 0 && config.interval_s == 300);
    assert(ruuvi_history_read(303, 0, &result) == -ENOENT);
}

static void test_full_ring_wrap_and_overflow(void)
{
    reset_flash();
    set_config(1, true);
    for (uint32_t i = 0; i < 14U * 250U; ++i) {
        assert(add(i) == 1);
    }
    assert(writes == 14 && generations[13] == 13);
    check(0, 0, 0);
    check(0, 249, 249);
    check(0, 250, 250);
    check(0, 3499, 3499);
    assert(ruuvi_history_read(0, 3500, &result) == -ENOENT);
    assert(add(3500) == 1);
    assert(ruuvi_history_flush() == 0);
    assert(writes == 15 && generations[0] == 14);
    check(0, 0, 250);
    check(0, 3249, 3499);
    check(0, 3250, 3500);
    check(3450, 50, 3500);
    assert(ruuvi_history_init() == 0);
    check(0, 0, 250);
    check(0, 3250, 3500);
    assert(add(4000) == 1);
    assert(ruuvi_history_flush() == 0);
    assert(generations[1] == 15);
    check(0, 0, 500);
    check(0, 3001, 4000);
}

static void test_no_overflow_and_failed_write(void)
{
    ruuvi_history_config_t config;
    reset_flash();
    set_config(1, false);
    for (uint32_t i = 0; i < 14U * 250U; ++i) {
        assert(add(i) == 1);
    }
    assert(writes == 14);
    for (uint32_t i = 3500; i < 3500U + 249U; ++i) {
        assert(add(i) == 1);
    }
    assert(add(3749) == -ENOSPC);
    assert(ruuvi_history_flush() == -ENOSPC);
    assert(writes == 14);
    check(0, 3748, 3748); /* Full flash plus 249 pending samples. */
    config = (ruuvi_history_config_t) { .interval_s = 1, .overflow = true,
                                        .fields = RUUVI_HISTORY_DEFAULT_FIELDS };
    assert(ruuvi_history_config_set(&config) == -ENOSPC);
    assert(ruuvi_history_config_get(&config) == 0 && !config.overflow);
    assert(ruuvi_history_init() == 0);
    check(0, 3499, 3499);
    assert(ruuvi_history_read(0, 3500, &result) == -ENOENT);

    reset_flash();
    set_config(1, true);
    for (uint32_t i = 0; i < 249U; ++i) {
        assert(add(i) == 1);
    }
    fail_write = true;
    assert(add(249) == -EIO);
    assert(writes == 0);
    check(0, 249, 249); /* The failed block is still in RAM. */
    assert(ruuvi_history_flush() == -EIO);
    fail_write = false;
    assert(ruuvi_history_flush() == 0 && writes == 1);
    assert(ruuvi_history_init() == 0);
    check(0, 249, 249);
}

static void test_sparse_torn_and_generation_wrap(void)
{
    reset_flash();
    seed(10, UINT32_MAX - 1U, 100);
    seed(11, UINT32_MAX, 200);
    seed(12, 0, 300);
    seed(13, 1, 400);
    seed(8, UINT32_MAX - 2U, 50);
    torn[9] = true;
    occupied[9] = true;
    assert(ruuvi_history_init() == 0);
    check(0, 0, 50);
    check(0, 1, 100);
    check(0, 2, 200);
    check(0, 3, 300);
    check(0, 4, 400);
    check(201, 0, 300);
    assert(ruuvi_history_read(0, 5, &result) == -ENOENT);
    assert(add(500) == 1);
    assert(ruuvi_history_flush() == 0);
    assert(generations[0] == 2);
    check(0, 5, 500);
    assert(ruuvi_history_init() == 0);
    check(0, 5, 500);
    torn[11] = true; /* Skip a slot that disappeared after the scan. */
    check(0, 2, 300);
    torn[11] = false;
    fail_read = true;
    assert(ruuvi_history_read(0, 0, &result) == -EIO);
    assert(ruuvi_history_init() == -EIO);
    fail_read = false;
    assert(ruuvi_history_init() == 0);

    reset_flash();
    seed(13, UINT32_MAX, 100);
    assert(ruuvi_history_init() == 0);
    assert(add(101) == 1);
    assert(ruuvi_history_flush() == 0);
    assert(generations[0] == 0);
    check(0, 0, 100);
    check(0, 1, 101);
    assert(ruuvi_history_init() == 0);
    check(0, 1, 101);

    reset_flash();
    seed(13, 7, 100);
    occupied[0] = true;
    torn[0] = true;
    assert(ruuvi_history_init() == 0);
    assert(add(101) == 1);
    assert(ruuvi_history_flush() == -EIO); /* Never erase an unjournaled tear. */
    assert(ruuvi_history_init() == 0);
    check(0, 0, 100);
    assert(ruuvi_history_read(0, 1, &result) == -ENOENT);
}

static void test_clear_and_recovery(void)
{
    reset_flash();
    assert(add(1) == 1);
    assert(ruuvi_history_flush() == 0);
    assert(add(301) == 1);
    fail_clear = true;
    assert(ruuvi_history_clear() == -EIO);
    assert(ruuvi_history_read(0, 0, &result) == -EACCES);
    fail_clear = false;
    assert(ruuvi_history_init() == 0);
    check(0, 0, 1);
    assert(ruuvi_history_read(0, 1, &result) == -ENOENT);
    assert(ruuvi_history_clear() == 0);
    assert(ruuvi_history_read(0, 0, &result) == -ENOENT);
    assert(ruuvi_history_init() == 0);
    assert(ruuvi_history_read(0, 0, &result) == -ENOENT);
    assert(add(0) == 1);
    assert(ruuvi_history_flush() == 0);
    assert(generations[0] == 0);
}

static void test_missing_environment_keeps_log_timestamps(void)
{
    ruuvi_history_element_t missing = {
        .timestamp_s = 0, .temperature_c = NAN,
        .humidity_rh = NAN, .pressure_pa = NAN,
    };
    uint32_t latest;

    reset_flash();
    assert(ruuvi_history_process(&missing) == 1);
    missing.timestamp_s = 299;
    assert(ruuvi_history_process(&missing) == 0);
    missing.timestamp_s = 300;
    assert(ruuvi_history_process(&missing) == 1);
    assert(ruuvi_history_read(0, 1, &result) == 0);
    assert(result.timestamp_s == 300 && isnan(result.temperature_c) &&
           isnan(result.humidity_rh) && isnan(result.pressure_pa));
    assert(ruuvi_history_flush() == 0);
    assert(ruuvi_history_init() == 0);
    assert(ruuvi_history_latest_timestamp(&latest) == 0 && latest == 300);
    assert(ruuvi_history_read(0, 0, &result) == 0);
    assert(result.timestamp_s == 0 && isnan(result.temperature_c));
    assert(ruuvi_history_read(0, 1, &result) == 0);
    assert(result.timestamp_s == 300 && isnan(result.humidity_rh));
}

int main(void)
{
    test_missing_environment_keeps_log_timestamps();
    test_defaults_interval_and_config();
    test_full_ring_wrap_and_overflow();
    test_no_overflow_and_failed_write();
    test_sparse_torn_and_generation_wrap();
    test_clear_and_recovery();
    return 0;
}
