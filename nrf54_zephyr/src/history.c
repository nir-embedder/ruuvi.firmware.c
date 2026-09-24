#include "history.h"
#include "history_flash.h"

#include <errno.h>
#include <stdbool.h>
#include <string.h>

/* The backend serializes access to its own flash buffer. These two records
 * live in static RAM rather than on the stack; callers serialize history API
 * calls (including reads), just as they do for the legacy logger.
 */
static ruuvi_history_record_t pending;
static ruuvi_history_record_t read_block;
static ruuvi_history_config_t configuration;
static bool present[RUUVI_HISTORY_RECORD_SLOTS];
static uint32_t generations[RUUVI_HISTORY_RECORD_SLOTS];
static uint32_t start_times[RUUVI_HISTORY_RECORD_SLOTS];
static uint8_t order[RUUVI_HISTORY_RECORD_SLOTS];
static uint8_t count;
static uint8_t next_slot;
static uint32_t next_generation;
static uint32_t last_timestamp;
static uint32_t persisted_last_timestamp;
static bool have_last_timestamp;
static bool have_persisted_timestamp;
static bool ready;

/* Serial number arithmetic: unambiguous for active generations separated by
 * less than 2^31 writes, including UINT32_MAX -> 0. Equal generations use
 * timestamp and slot as deterministic tie breakers.
 */
static bool newer(uint32_t a, uint32_t b)
{
    return a != b && (uint32_t)(a - b) < UINT32_C(0x80000000);
}

static bool precedes(uint8_t a, uint8_t b, uint32_t newest)
{
    uint32_t age_a = newest - generations[a];
    uint32_t age_b = newest - generations[b];

    if (age_a != age_b) {
        return age_a > age_b;
    }
    if (start_times[a] != start_times[b]) {
        return start_times[a] < start_times[b];
    }
    return a < b;
}

static void rebuild_order(uint32_t newest)
{
    count = 0;
    for (uint8_t slot = 0; slot < RUUVI_HISTORY_RECORD_SLOTS; ++slot) {
        if (!present[slot]) {
            continue;
        }
        uint8_t position = count++;
        while (position > 0 && precedes(slot, order[position - 1U], newest)) {
            order[position] = order[position - 1U];
            --position;
        }
        order[position] = slot;
    }
}

int ruuvi_history_init(void)
{
    int rc;
    bool found = false;
    uint8_t newest_slot = 0;
    uint32_t newest_generation = 0;
    uint32_t newest_timestamp = 0;

    ready = false;
    memset(&pending, 0, sizeof(pending));
    memset(present, 0, sizeof(present));
    count = 0;
    next_slot = 0;
    next_generation = 0;
    have_last_timestamp = false;
    have_persisted_timestamp = false;
    configuration = (ruuvi_history_config_t) {
        .interval_s = RUUVI_HISTORY_DEFAULT_INTERVAL_S,
        .overflow = RUUVI_HISTORY_DEFAULT_OVERFLOW,
        .fields = RUUVI_HISTORY_DEFAULT_FIELDS,
    };

    rc = ruuvi_history_flash_init();
    if (rc < 0) {
        return rc;
    }
    for (uint8_t slot = 0; slot < RUUVI_HISTORY_RECORD_SLOTS; ++slot) {
        uint32_t generation;
        rc = ruuvi_history_flash_read(slot, &read_block, &generation);
        if (rc == -ENOENT) {
            continue; /* Empty, interrupted, or invalid committed record. */
        }
        if (rc < 0) {
            return rc;
        }
        present[slot] = true;
        generations[slot] = generation;
        start_times[slot] = read_block.start_timestamp_s;
        if (!found || newer(generation, newest_generation) ||
            (generation == newest_generation &&
             (read_block.end_timestamp_s > newest_timestamp ||
              (read_block.end_timestamp_s == newest_timestamp && slot > newest_slot)))) {
            found = true;
            newest_slot = slot;
            newest_generation = generation;
            newest_timestamp = read_block.end_timestamp_s;
        }
    }
    if (found) {
        next_slot = (uint8_t)((newest_slot + 1U) % RUUVI_HISTORY_RECORD_SLOTS);
        next_generation = newest_generation + 1U;
        persisted_last_timestamp = newest_timestamp;
        have_persisted_timestamp = true;
        rebuild_order(newest_generation);
        /* The interval is measured against accepted samples in this session;
         * do not infer a running clock from a stored wall-clock timestamp.
         */
    }
    ready = true;
    return 0;
}

int ruuvi_history_flush(void)
{
    int rc;

    if (!ready) {
        return -EACCES;
    }
    if (pending.num_samples == 0U) {
        return 0;
    }
    if (!configuration.overflow && present[next_slot]) {
        return -ENOSPC;
    }
    rc = ruuvi_history_flash_write(next_slot, &pending, next_generation);
    if (rc < 0) {
        return rc;
    }
    persisted_last_timestamp = pending.end_timestamp_s;
    have_persisted_timestamp = true;
    present[next_slot] = true;
    generations[next_slot] = next_generation;
    start_times[next_slot] = pending.start_timestamp_s;
    rebuild_order(next_generation);
    next_generation++;
    next_slot = (uint8_t)((next_slot + 1U) % RUUVI_HISTORY_RECORD_SLOTS);
    memset(&pending, 0, sizeof(pending));
    return 0;
}

int ruuvi_history_process(const ruuvi_history_element_t *sample)
{
    int rc;
    uint32_t timestamp;

    if (sample == NULL) {
        return -EINVAL;
    }
    if (!ready) {
        return -EACCES;
    }
    timestamp = sample->timestamp_s;
    if ((have_last_timestamp && timestamp < last_timestamp) ||
        (have_persisted_timestamp && timestamp < persisted_last_timestamp)) {
        return -EINVAL;
    }
    if (pending.num_samples == RUUVI_HISTORY_SAMPLES_PER_RECORD) {
        rc = ruuvi_history_flush();
        if (rc < 0) {
            return rc;
        }
    }
    if (have_last_timestamp && timestamp - last_timestamp < configuration.interval_s) {
        return 0;
    }
    if (pending.num_samples == RUUVI_HISTORY_SAMPLES_PER_RECORD - 1U &&
        !configuration.overflow && present[next_slot]) {
        return -ENOSPC;
    }
    if (pending.num_samples == 0U) {
        pending.start_timestamp_s = timestamp;
        pending.block_configuration = configuration;
    }
    pending.storage[pending.num_samples++] = *sample;
    pending.end_timestamp_s = timestamp;
    last_timestamp = timestamp;
    have_last_timestamp = true;
    if (pending.num_samples == RUUVI_HISTORY_SAMPLES_PER_RECORD) {
        rc = ruuvi_history_flush();
        if (rc < 0) {
            return rc; /* Full block retained in RAM for a later retry. */
        }
    }
    return 1;
}

int ruuvi_history_read(uint32_t min_timestamp_s, uint32_t index,
                       ruuvi_history_element_t *sample)
{
    if (sample == NULL) {
        return -EINVAL;
    }
    if (!ready) {
        return -EACCES;
    }
    for (uint8_t i = 0; i <= count; ++i) {
        const ruuvi_history_record_t *block;
        int rc;

        if (i == count) {
            block = &pending;
        } else {
            uint8_t slot = order[i];
            uint32_t generation;
            rc = ruuvi_history_flash_read(slot, &read_block, &generation);
            if (rc == -ENOENT || (rc == 0 && generation != generations[slot])) {
                continue; /* Slot was lost/replaced since the last scan. */
            }
            if (rc < 0) {
                return rc;
            }
            block = &read_block;
        }
        for (uint32_t j = 0; j < block->num_samples; ++j) {
            if (block->storage[j].timestamp_s >= min_timestamp_s) {
                if (index == 0U) {
                    *sample = block->storage[j];
                    return 0;
                }
                --index;
            }
        }
    }
    return -ENOENT;
}

int ruuvi_history_latest_timestamp(uint32_t *timestamp_s)
{
    if (timestamp_s == NULL) {
        return -EINVAL;
    }
    if (!ready) {
        return -EACCES;
    }
    if (pending.num_samples != 0U) {
        *timestamp_s = pending.end_timestamp_s;
        return 0;
    }
    if (count == 0U) {
        return -ENOENT;
    }

    const uint8_t slot = order[count - 1U];
    uint32_t generation;
    int rc = ruuvi_history_flash_read(slot, &read_block, &generation);
    if (rc < 0) {
        return rc;
    }
    if (generation != generations[slot]) {
        return -EIO;
    }
    *timestamp_s = read_block.end_timestamp_s;
    return 0;
}

int ruuvi_history_config_set(const ruuvi_history_config_t *config)
{
    int rc;

    if (config == NULL || config->interval_s == 0U) {
        return -EINVAL;
    }
    if (!ready) {
        return -EACCES;
    }
    if (config->interval_s == configuration.interval_s &&
        config->overflow == configuration.overflow &&
        config->fields == configuration.fields) {
        return 0;
    }
    rc = ruuvi_history_flush();
    if (rc < 0) {
        return rc;
    }
    configuration = *config;
    return 0;
}

int ruuvi_history_config_get(ruuvi_history_config_t *config)
{
    if (config == NULL) {
        return -EINVAL;
    }
    if (!ready) {
        return -EACCES;
    }
    *config = configuration;
    return 0;
}

int ruuvi_history_clear(void)
{
    int rc;

    if (!ready) {
        return -EACCES;
    }
    rc = ruuvi_history_flash_clear();
    if (rc < 0) {
        ready = false;
        return rc;
    }
    memset(present, 0, sizeof(present));
    memset(&pending, 0, sizeof(pending));
    count = 0;
    next_slot = 0;
    next_generation = 0;
    have_last_timestamp = false;
    have_persisted_timestamp = false;
    return 0;
}
