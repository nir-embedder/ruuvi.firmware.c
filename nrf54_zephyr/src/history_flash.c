#include "history_flash.h"

#include <errno.h>
#include <string.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/kernel.h>
#include <zephyr/storage/flash_map.h>

#if PARTITION_EXISTS(ruuvi_history_partition) && defined(CONFIG_FLASH_MAP) && \
    defined(CONFIG_FLASH_PAGE_LAYOUT)

enum {
    PAGE_SIZE = 4096,
    SCRATCH_SLOT = RUUVI_HISTORY_RECORD_SLOTS,
    PAGE_COUNT = RUUVI_HISTORY_RECORD_SLOTS + 1,
    DATA_SIZE = 4032, /* 4020 bytes of legacy payload, padded to a 16-byte boundary. */
    TRAILER_OFFSET = PAGE_SIZE - 16,
    TRAILER_SIZE = 16,
    COMMIT_MARKER = 0x48535452,
};

_Static_assert(RUUVI_HISTORY_RECORD_SIZE == 4020 && RUUVI_HISTORY_RECORD_SLOTS == 14,
               "history flash geometry must match the legacy codec");

static K_MUTEX_DEFINE(history_lock);
static const struct flash_area *history_area;
static uint8_t page_buffer[PAGE_SIZE];

static void put_u32(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8);
    out[2] = (uint8_t)(value >> 16);
    out[3] = (uint8_t)(value >> 24);
}

static uint32_t get_u32(const uint8_t *in)
{
    return (uint32_t)in[0] | ((uint32_t)in[1] << 8) |
           ((uint32_t)in[2] << 16) | ((uint32_t)in[3] << 24);
}

static off_t slot_offset(uint8_t slot)
{
    return (off_t)slot * PAGE_SIZE;
}

static uint32_t payload_crc32(const uint8_t *data)
{
    uint32_t crc = UINT32_MAX;

    for (size_t i = 0; i < RUUVI_HISTORY_RECORD_SIZE; ++i) {
        crc ^= data[i];
        for (unsigned int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
        }
    }
    return ~crc;
}

/* Success leaves the verified payload in page_buffer. The scratch page's
 * trailer names the record slot it protects, not the physical scratch slot.
 */
static int load_image(off_t offset, uint8_t slot, uint32_t *generation)
{
    uint8_t trailer[TRAILER_SIZE];
    int rc = flash_area_read(history_area, offset + TRAILER_OFFSET,
                             trailer, sizeof(trailer));

    if (rc < 0) {
        return rc;
    }
    if (get_u32(trailer) != slot || get_u32(trailer + 12) != COMMIT_MARKER) {
        return -ENOENT;
    }

    rc = flash_area_read(history_area, offset, page_buffer, RUUVI_HISTORY_RECORD_SIZE);
    if (rc < 0) {
        return rc;
    }
    if (payload_crc32(page_buffer) != get_u32(trailer + 8) ||
        get_u32(page_buffer + 8) > RUUVI_HISTORY_SAMPLES_PER_RECORD ||
        page_buffer[14] > 1U) {
        return -ENOENT;
    }
    if (generation != NULL) {
        *generation = get_u32(trailer + 4);
    }
    return 0;
}

/* Return 1 for a completely erased page, 0 for any data, or an I/O error. */
static int page_is_erased(off_t offset)
{
    int rc = flash_area_read(history_area, offset, page_buffer, sizeof(page_buffer));

    if (rc < 0) {
        return rc;
    }
    for (size_t i = 0; i < sizeof(page_buffer); ++i) {
        if (page_buffer[i] != 0xffU) {
            return 0;
        }
    }
    return 1;
}

static int erase_page(off_t offset)
{
    int rc = flash_area_erase(history_area, offset, PAGE_SIZE);

    if (rc < 0) {
        return rc;
    }
    rc = page_is_erased(offset);
    return rc == 1 ? 0 : (rc == 0 ? -EIO : rc);
}

/* page_buffer holds the 4020-byte payload; the target must already be erased.
 * Publish the trailer only after the payload has been read back and checked.
 */
static int write_image(off_t offset, uint8_t slot, uint32_t generation)
{
    uint8_t trailer[TRAILER_SIZE];
    uint32_t crc = payload_crc32(page_buffer);
    int rc;

    memset(page_buffer + RUUVI_HISTORY_RECORD_SIZE, 0xff,
           DATA_SIZE - RUUVI_HISTORY_RECORD_SIZE);
    rc = flash_area_write(history_area, offset, page_buffer, DATA_SIZE);
    if (rc < 0) {
        return rc;
    }
    rc = flash_area_read(history_area, offset, page_buffer, RUUVI_HISTORY_RECORD_SIZE);
    if (rc < 0) {
        return rc;
    }
    if (payload_crc32(page_buffer) != crc) {
        return -EIO;
    }

    put_u32(trailer, slot);
    put_u32(trailer + 4, generation);
    put_u32(trailer + 8, crc);
    put_u32(trailer + 12, COMMIT_MARKER);
    rc = flash_area_write(history_area, offset + TRAILER_OFFSET,
                          trailer, sizeof(trailer));
    if (rc < 0) {
        return rc;
    }
    rc = load_image(offset, slot, NULL);
    return rc == -ENOENT ? -EIO : rc;
}

/* A valid destination supersedes the journal (including after a completed
 * new write). A torn destination is restored from the committed old image.
 * Do not erase the journal during replay: another power cut must be retryable.
 */
static int replay_journal(void)
{
    uint8_t trailer[TRAILER_SIZE];
    uint8_t slot;
    uint32_t generation;
    int rc = flash_area_read(history_area, slot_offset(SCRATCH_SLOT) + TRAILER_OFFSET,
                             trailer, sizeof(trailer));

    if (rc < 0) {
        return rc;
    }
    if (get_u32(trailer + 12) != COMMIT_MARKER ||
        get_u32(trailer) >= RUUVI_HISTORY_RECORD_SLOTS) {
        return 0;
    }
    slot = (uint8_t)get_u32(trailer);
    rc = load_image(slot_offset(SCRATCH_SLOT), slot, &generation);
    if (rc == -ENOENT) {
        return 0;
    }
    if (rc < 0) {
        return rc;
    }
    rc = load_image(slot_offset(slot), slot, NULL);
    if (rc == 0) {
        return 0;
    }
    if (rc != -ENOENT) {
        return rc;
    }
    rc = erase_page(slot_offset(slot));
    if (rc < 0) {
        return rc;
    }
    rc = load_image(slot_offset(SCRATCH_SLOT), slot, NULL);
    if (rc != 0) {
        return rc == -ENOENT ? -EIO : rc;
    }
    return write_image(slot_offset(slot), slot, generation);
}

static int validate_partition(const struct flash_area *area)
{
    const struct flash_parameters *params;
    struct flash_pages_info page;
    size_t align;

    if (!flash_area_device_is_ready(area)) {
        return -ENODEV;
    }
    if (area->fa_off < 0 || area->fa_size < PAGE_COUNT * PAGE_SIZE ||
        area->fa_size % PAGE_SIZE != 0) {
        return -EINVAL;
    }
    params = flash_get_parameters(area->fa_dev);
    if (params == NULL || params->erase_value != 0xffU ||
        flash_area_erased_val(area) != 0xffU) {
        return -ENOTSUP;
    }
    align = flash_area_align(area);
    if (align == 0 || TRAILER_SIZE % align != 0 ||
        DATA_SIZE % align != 0 || (size_t)area->fa_off % align != 0) {
        return -ENOTSUP;
    }
    for (unsigned int i = 0; i < PAGE_COUNT; ++i) {
        off_t start = area->fa_off + (off_t)i * PAGE_SIZE;
        int rc = flash_get_page_info_by_offs(area->fa_dev, start, &page);

        if (rc < 0 || page.start_offset != start || page.size != PAGE_SIZE) {
            return -EINVAL;
        }
    }
    return 0;
}

int ruuvi_history_flash_init(void)
{
    int rc;

    k_mutex_lock(&history_lock, K_FOREVER);
    if (history_area == NULL) {
        const struct flash_area *area;

        rc = flash_area_open(PARTITION_ID(ruuvi_history_partition), &area);
        if (rc < 0) {
            goto out;
        }
        rc = validate_partition(area);
        if (rc < 0) {
            flash_area_close(area);
            goto out;
        }
        history_area = area;
    }
    rc = flash_area_device_is_ready(history_area) ? replay_journal() : -ENODEV;
out:
    k_mutex_unlock(&history_lock);
    return rc;
}

int ruuvi_history_flash_read(uint8_t slot, ruuvi_history_record_t *record,
                             uint32_t *generation)
{
    uint32_t found_generation;
    int rc;

    if (slot >= RUUVI_HISTORY_RECORD_SLOTS || record == NULL) {
        return -EINVAL;
    }
    k_mutex_lock(&history_lock, K_FOREVER);
    if (history_area == NULL || !flash_area_device_is_ready(history_area)) {
        rc = history_area == NULL ? -EACCES : -ENODEV;
        goto out;
    }
    rc = load_image(slot_offset(slot), slot, &found_generation);
    if (rc == -ENOENT) {
        rc = load_image(slot_offset(SCRATCH_SLOT), slot, &found_generation);
    }
    if (rc == 0) {
        rc = ruuvi_history_decode(page_buffer, RUUVI_HISTORY_RECORD_SIZE, record);
        if (rc == 0 && generation != NULL) {
            *generation = found_generation;
        }
        if (rc == -EINVAL) {
            rc = -ENOENT;
        }
    }
out:
    k_mutex_unlock(&history_lock);
    return rc;
}

int ruuvi_history_flash_write(uint8_t slot, const ruuvi_history_record_t *record,
                              uint32_t generation)
{
    uint32_t old_generation;
    bool had_old;
    int rc;

    if (slot >= RUUVI_HISTORY_RECORD_SLOTS || record == NULL) {
        return -EINVAL;
    }
    k_mutex_lock(&history_lock, K_FOREVER);
    if (history_area == NULL || !flash_area_device_is_ready(history_area)) {
        rc = history_area == NULL ? -EACCES : -ENODEV;
        goto out;
    }
    rc = ruuvi_history_encode(record, page_buffer, RUUVI_HISTORY_RECORD_SIZE);
    if (rc < 0) {
        goto out;
    }
    rc = replay_journal();
    if (rc < 0) {
        goto out;
    }

    rc = load_image(slot_offset(slot), slot, &old_generation);
    if (rc == -ENOENT) {
        /* A torn, unjournaled page is not free space. Only clear() may
         * discard it; do not erase a possible older record implicitly.
         */
        rc = page_is_erased(slot_offset(slot));
        if (rc == 0) {
            rc = -EIO;
            goto out;
        }
        if (rc < 0) {
            goto out;
        }
        had_old = false;
    } else if (rc < 0) {
        goto out;
    } else {
        had_old = true;
    }

    rc = erase_page(slot_offset(SCRATCH_SLOT));
    if (rc < 0) {
        goto out;
    }
    if (had_old) {
        rc = load_image(slot_offset(slot), slot, NULL);
        if (rc != 0) {
            goto out;
        }
        rc = write_image(slot_offset(SCRATCH_SLOT), slot, old_generation);
        if (rc < 0) {
            goto out;
        }
    }
    rc = erase_page(slot_offset(slot));
    if (rc < 0) {
        goto out;
    }
    rc = ruuvi_history_encode(record, page_buffer, RUUVI_HISTORY_RECORD_SIZE);
    if (rc == 0) {
        rc = write_image(slot_offset(slot), slot, generation);
    }
out:
    k_mutex_unlock(&history_lock);
    return rc;
}

int ruuvi_history_flash_clear(void)
{
    int rc;

    k_mutex_lock(&history_lock, K_FOREVER);
    if (history_area == NULL || !flash_area_device_is_ready(history_area)) {
        rc = history_area == NULL ? -EACCES : -ENODEV;
        goto out;
    }
    /* Invalidate the journal first so an interrupted clear cannot resurrect
     * a deleted record. Clearing all 14 slots is not an atomic operation.
     */
    rc = erase_page(slot_offset(SCRATCH_SLOT));
    if (rc < 0) {
        goto out;
    }
    for (uint8_t slot = 0; slot < RUUVI_HISTORY_RECORD_SLOTS; ++slot) {
        rc = erase_page(slot_offset(slot));
        if (rc < 0) {
            goto out;
        }
    }
out:
    k_mutex_unlock(&history_lock);
    return rc;
}

#else

int ruuvi_history_flash_init(void)
{
#if PARTITION_EXISTS(ruuvi_history_partition)
    return -ENOTSUP; /* Flash map or page-layout support is unavailable. */
#else
    return -ENODEV;
#endif
}

int ruuvi_history_flash_read(uint8_t slot, ruuvi_history_record_t *record,
                             uint32_t *generation)
{
    (void)slot;
    (void)record;
    (void)generation;
    return ruuvi_history_flash_init();
}

int ruuvi_history_flash_write(uint8_t slot, const ruuvi_history_record_t *record,
                              uint32_t generation)
{
    (void)slot;
    (void)record;
    (void)generation;
    return ruuvi_history_flash_init();
}

int ruuvi_history_flash_clear(void)
{
    return ruuvi_history_flash_init();
}

#endif
