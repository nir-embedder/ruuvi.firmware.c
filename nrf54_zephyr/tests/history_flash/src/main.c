/* Destructive flash tests run only on the QEMU simulator, never on target RRAM. */
#include <errno.h>
#include <math.h>
#include <string.h>
#include <zephyr/ztest.h>
#ifdef CONFIG_FLASH_SIMULATOR
#include <zephyr/drivers/flash/flash_simulator.h>
#include <zephyr/storage/flash_map.h>
#endif

#include "history.h"
#include "history_flash.h"
#include "history_settings.h"
#include "log_service.h"
#include "ruuvi_endpoints.h"

ZTEST(ruuvi_history_flash, test_mount_only)
{
    int rc = ruuvi_history_flash_init();
    zassert_equal(rc, 0, "history layout or flash geometry is unsupported (%d)", rc);
}

#ifdef CONFIG_FLASH_SIMULATOR
static ruuvi_history_record_t original;
static ruuvi_history_record_t recovered;

typedef struct {
    uint8_t frames[4][RUUVI_LOG_MESSAGE_LENGTH];
    size_t count;
    bool busy_once;
} log_capture_t;

static int capture_log_frame(void *context, const uint8_t *data, size_t length)
{
    log_capture_t *capture = context;

    if (length != RUUVI_LOG_MESSAGE_LENGTH || capture->count >= ARRAY_SIZE(capture->frames)) {
        return -EINVAL;
    }
    if (capture->busy_once) {
        capture->busy_once = false;
        return -EAGAIN;
    }
    memcpy(capture->frames[capture->count++], data, length);
    return 0;
}

ZTEST(ruuvi_history_flash, test_full_ring_and_torn_overwrite)
{
    zassert_equal(ruuvi_history_flash_init(), 0);
    zassert_equal(ruuvi_history_flash_clear(), 0);

    for (uint8_t slot = 0; slot < RUUVI_HISTORY_RECORD_SLOTS; ++slot) {
        memset(&original, 0, sizeof(original));
        original.num_samples = RUUVI_HISTORY_SAMPLES_PER_RECORD;
        original.start_timestamp_s = (uint32_t)slot * 250U;
        original.end_timestamp_s = original.start_timestamp_s + 249U;
        original.storage[0].timestamp_s = original.start_timestamp_s;
        original.storage[0].temperature_c = (float)slot;
        zassert_equal(ruuvi_history_flash_write(slot, &original, slot + 1U), 0,
                      "write failed for slot %u", slot);
    }
    for (uint8_t slot = 0; slot < RUUVI_HISTORY_RECORD_SLOTS; ++slot) {
        uint32_t generation = 0;
        zassert_equal(ruuvi_history_flash_read(slot, &recovered, &generation), 0);
        zassert_equal(generation, slot + 1U);
        zassert_equal(recovered.num_samples, RUUVI_HISTORY_SAMPLES_PER_RECORD);
        zassert_equal(recovered.storage[0].temperature_c, (float)slot);
    }

    original.num_samples = 1;
    original.storage[0].temperature_c = 99.0f;
    zassert_equal(ruuvi_history_flash_write(0, &original, 15U), 0);
    uint32_t generation = 0;
    zassert_equal(ruuvi_history_flash_read(0, &recovered, &generation), 0);
    zassert_equal(generation, 15U);
    zassert_equal(recovered.storage[0].temperature_c, 99.0f);

    const struct device *flash = PARTITION_DEVICE(ruuvi_history_partition);
    size_t memory_size = 0;
    uint8_t *memory = flash_simulator_get_memory(flash, &memory_size);
    zassert_not_null(memory);
    zassert_true(memory_size >= 15U * 4096U);
    /* Simulate power loss after erasing/partially rewriting slot 0: its
     * commit marker is torn, while the prior record is safe in scratch.
     */
    memory[PARTITION_OFFSET(ruuvi_history_partition) + 4080U + 12U] = 0;
    zassert_equal(ruuvi_history_flash_init(), 0);
    zassert_equal(ruuvi_history_flash_read(0, &recovered, &generation), 0);
    zassert_equal(generation, 1U);
    zassert_equal(recovered.storage[0].temperature_c, 0.0f);

    zassert_equal(ruuvi_history_flash_clear(), 0);
    zassert_equal(ruuvi_history_flash_read(0, &recovered, &generation), -ENOENT);
    zassert_equal(ruuvi_history_flash_read(14, &recovered, &generation), -EINVAL);
}

ZTEST(ruuvi_history_flash, test_invalid_environment_sample_survives_restart)
{
    uint32_t generation = 0;

    zassert_equal(ruuvi_history_flash_init(), 0);
    zassert_equal(ruuvi_history_flash_clear(), 0);
    memset(&original, 0, sizeof(original));
    original.num_samples = 1;
    original.start_timestamp_s = 300;
    original.end_timestamp_s = 300;
    original.storage[0].timestamp_s = 300;
    original.storage[0].temperature_c = NAN;
    original.storage[0].humidity_rh = NAN;
    original.storage[0].pressure_pa = NAN;
    zassert_equal(ruuvi_history_flash_write(0, &original, 1), 0);
    zassert_equal(ruuvi_history_flash_init(), 0);
    zassert_equal(ruuvi_history_flash_read(0, &recovered, &generation), 0);
    zassert_equal(generation, 1);
    zassert_equal(recovered.storage[0].timestamp_s, 300);
    zassert_true(isnan(recovered.storage[0].temperature_c));
    zassert_true(isnan(recovered.storage[0].humidity_rh));
    zassert_true(isnan(recovered.storage[0].pressure_pa));
}

ZTEST(ruuvi_history_flash, test_manager_recovers_committed_samples)
{
    ruuvi_history_element_t first = {
        .timestamp_s = 300, .temperature_c = 20.5f,
        .humidity_rh = 40.0f, .pressure_pa = 101325.0f,
    };
    ruuvi_history_element_t second = {
        .timestamp_s = 600, .temperature_c = 21.25f,
        .humidity_rh = 41.0f, .pressure_pa = 100000.0f,
    };
    ruuvi_history_element_t result = {0};
    uint32_t latest = 0;

    zassert_equal(ruuvi_history_init(), 0);
    zassert_equal(ruuvi_history_clear(), 0);
    zassert_equal(ruuvi_history_process(&first), 1);
    zassert_equal(ruuvi_history_process(&second), 1);
    zassert_equal(ruuvi_history_flush(), 0);

    zassert_equal(ruuvi_history_init(), 0); /* Simulate reboot, not a power cut. */
    zassert_equal(ruuvi_history_latest_timestamp(&latest), 0);
    zassert_equal(latest, second.timestamp_s);
    zassert_equal(ruuvi_history_read(0, 0, &result), 0);
    zassert_equal(result.timestamp_s, first.timestamp_s);
    zassert_equal(result.temperature_c, first.temperature_c);
    zassert_equal(result.humidity_rh, first.humidity_rh);
    zassert_equal(result.pressure_pa, first.pressure_pa);
    zassert_equal(ruuvi_history_read(0, 1, &result), 0);
    zassert_equal(result.timestamp_s, second.timestamp_s);
    zassert_equal(result.temperature_c, second.temperature_c);
    zassert_equal(result.humidity_rh, second.humidity_rh);
    zassert_equal(result.pressure_pa, second.pressure_pa);
    zassert_equal(ruuvi_history_read(0, 2, &result), -ENOENT);

    first.timestamp_s = 900;
    zassert_equal(ruuvi_history_process(&first), 1);
    zassert_equal(ruuvi_history_init(), 0); /* Unflushed RAM sample must be lost. */
    zassert_equal(ruuvi_history_latest_timestamp(&latest), 0);
    zassert_equal(latest, second.timestamp_s);
    zassert_equal(ruuvi_history_read(0, 2, &result), -ENOENT);
    zassert_equal(ruuvi_history_clear(), 0);
}

ZTEST(ruuvi_history_flash, test_flash_backed_log_stream)
{
    const ruuvi_history_element_t sample = {
        .timestamp_s = 240, .temperature_c = 23.45f,
        .humidity_rh = 56.78f, .pressure_pa = 101325.0f,
    };
    const uint8_t request[RUUVI_LOG_MESSAGE_LENGTH] = {
        RE_STANDARD_DESTINATION_ENVIRONMENTAL, 0xA5, RE_STANDARD_LOG_VALUE_READ,
        0x00, 0x00, 0x03, 0xE8, 0x00, 0x00, 0x03, 0x84,
    };
    const uint8_t expected[][RUUVI_LOG_MESSAGE_LENGTH] = {
        {0xA5, 0x31, 0x10, 0x00, 0x00, 0x03, 0xAC, 0x00, 0x00, 0x16, 0x2E},
        {0xA5, 0x32, 0x10, 0x00, 0x00, 0x03, 0xAC, 0x00, 0x01, 0x8B, 0xCD},
        {0xA5, 0x30, 0x10, 0x00, 0x00, 0x03, 0xAC, 0x00, 0x00, 0x09, 0x29},
        {0xA5, 0x3A, 0x10, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF},
    };
    ruuvi_log_service_t service = {0};
    log_capture_t capture = {.busy_once = true};

    zassert_equal(ruuvi_history_init(), 0);
    zassert_equal(ruuvi_history_clear(), 0);
    zassert_equal(ruuvi_history_process(&sample), 1);
    zassert_equal(ruuvi_history_flush(), 0);
    zassert_equal(ruuvi_history_init(), 0);
    zassert_equal(ruuvi_log_service_start(&service, request, sizeof(request), 300, 100), 0);
    zassert_equal(ruuvi_log_service_pump(&service, 100, capture_log_frame, &capture), 0);
    zassert_true(ruuvi_log_service_active(&service));
    for (size_t i = 0; i < ARRAY_SIZE(expected); ++i) {
        zassert_equal(ruuvi_log_service_pump(&service, 101 + i, capture_log_frame, &capture), 1);
    }
    zassert_false(ruuvi_log_service_active(&service));
    zassert_equal(capture.count, ARRAY_SIZE(expected));
    for (size_t i = 0; i < ARRAY_SIZE(expected); ++i) {
        zassert_mem_equal(capture.frames[i], expected[i], RUUVI_LOG_MESSAGE_LENGTH);
    }
    zassert_equal(ruuvi_history_clear(), 0);
}

ZTEST(ruuvi_history_flash, test_config_nvs_survives_history_clear)
{
    ruuvi_history_config_t saved = {
        .interval_s = 300U,
        .overflow = true,
        .fields = RUUVI_HISTORY_DEFAULT_FIELDS,
    };
    ruuvi_history_config_t loaded = {0};

    zassert_equal(ruuvi_history_settings_init(), 0);
    zassert_equal(ruuvi_history_settings_store(&saved), 0);
    zassert_equal(ruuvi_history_settings_load(&loaded), 0);
    zassert_equal(loaded.interval_s, saved.interval_s);
    zassert_equal(loaded.overflow, saved.overflow);
    zassert_equal(loaded.fields, saved.fields);

    zassert_equal(ruuvi_history_flash_init(), 0);
    zassert_equal(ruuvi_history_flash_clear(), 0);
    memset(&loaded, 0, sizeof(loaded));
    zassert_equal(ruuvi_history_settings_init(), 0);
    zassert_equal(ruuvi_history_settings_load(&loaded), 0);
    zassert_equal(loaded.interval_s, saved.interval_s);
    zassert_equal(loaded.fields, saved.fields);
}
#endif

ZTEST_SUITE(ruuvi_history_flash, NULL, NULL, NULL, NULL, NULL);
