#include "history_settings.h"

#include <errno.h>
#include <stdint.h>
#include <zephyr/storage/flash_map.h>

#if defined(CONFIG_NVS) && defined(CONFIG_FLASH_MAP) && \
    defined(CONFIG_FLASH_PAGE_LAYOUT) && PARTITION_EXISTS(storage_partition)

#include <zephyr/device.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/kernel.h>
#include <zephyr/kvss/nvs.h>

/* Below Zephyr settings NVS's reserved range (0x8000 and above). */
#define HISTORY_CONFIG_ID UINT16_C(0x4843)
#define HISTORY_CONFIG_VERSION 1U
#define HISTORY_CONFIG_SIZE 8U

static K_MUTEX_DEFINE(settings_lock);
static struct nvs_fs settings_fs;
static bool settings_ready;

int ruuvi_history_settings_init(void)
{
    struct flash_pages_info page;
    const size_t partition_size = PARTITION_SIZE(storage_partition);
    size_t count;
    int rc;

    k_mutex_lock(&settings_lock, K_FOREVER);
    if (settings_ready) {
        rc = 0;
        goto out;
    }

    settings_fs.flash_device = PARTITION_DEVICE(storage_partition);
    settings_fs.offset = PARTITION_OFFSET(storage_partition);
    if (!device_is_ready(settings_fs.flash_device)) {
        rc = -ENODEV;
        goto out;
    }
    rc = flash_get_page_info_by_offs(settings_fs.flash_device, settings_fs.offset, &page);
    if (rc < 0) {
        goto out;
    }
    if (page.start_offset != settings_fs.offset || page.size == 0U ||
        page.size > UINT16_MAX || (page.size & (page.size - 1U)) != 0U ||
        partition_size % page.size != 0U) {
        rc = -EINVAL;
        goto out;
    }
    count = partition_size / page.size;
    if (count < 2U || count > UINT16_MAX) {
        rc = -EINVAL;
        goto out;
    }
    settings_fs.sector_size = (uint32_t)page.size;
    settings_fs.sector_count = (uint16_t)count;
    for (size_t i = 0; i < count; ++i) {
        off_t offset = settings_fs.offset + (off_t)(i * settings_fs.sector_size);

        rc = flash_get_page_info_by_offs(settings_fs.flash_device, offset, &page);
        if (rc < 0) {
            goto out;
        }
        if (page.start_offset != offset || page.size != settings_fs.sector_size) {
            rc = -EINVAL;
            goto out;
        }
    }
    rc = nvs_mount(&settings_fs);
    if (rc == 0) {
        settings_ready = true;
    }
out:
    k_mutex_unlock(&settings_lock);
    return rc;
}

int ruuvi_history_settings_load(ruuvi_history_config_t *config)
{
    uint8_t data[HISTORY_CONFIG_SIZE];
    ruuvi_history_config_t decoded;
    ssize_t rc;

    if (config == NULL) {
        return -EINVAL;
    }
    k_mutex_lock(&settings_lock, K_FOREVER);
    if (!settings_ready || !device_is_ready(settings_fs.flash_device)) {
        rc = settings_ready ? -ENODEV : -EACCES;
        goto out;
    }
    rc = nvs_read(&settings_fs, HISTORY_CONFIG_ID, data, sizeof(data));
    if (rc < 0) {
        goto out;
    }
    if (rc != sizeof(data) || data[0] != HISTORY_CONFIG_VERSION ||
        data[1] > 1U || (data[2] == 0U && data[3] == 0U)) {
        rc = -EBADMSG;
        goto out;
    }
    decoded.interval_s = (uint16_t)data[2] | ((uint16_t)data[3] << 8);
    decoded.overflow = (data[1] == 1U);
    decoded.fields = (uint32_t)data[4] | ((uint32_t)data[5] << 8) |
                     ((uint32_t)data[6] << 16) | ((uint32_t)data[7] << 24);
    *config = decoded;
    rc = 0;
out:
    k_mutex_unlock(&settings_lock);
    return (int)rc;
}

int ruuvi_history_settings_store(const ruuvi_history_config_t *config)
{
    uint8_t data[HISTORY_CONFIG_SIZE];
    ssize_t rc;

    if (config == NULL || config->interval_s == 0U) {
        return -EINVAL;
    }
    data[0] = HISTORY_CONFIG_VERSION;
    data[1] = config->overflow ? 1U : 0U;
    data[2] = (uint8_t)config->interval_s;
    data[3] = (uint8_t)(config->interval_s >> 8);
    data[4] = (uint8_t)config->fields;
    data[5] = (uint8_t)(config->fields >> 8);
    data[6] = (uint8_t)(config->fields >> 16);
    data[7] = (uint8_t)(config->fields >> 24);

    k_mutex_lock(&settings_lock, K_FOREVER);
    if (!settings_ready || !device_is_ready(settings_fs.flash_device)) {
        rc = settings_ready ? -ENODEV : -EACCES;
        goto out;
    }
    rc = nvs_write(&settings_fs, HISTORY_CONFIG_ID, data, sizeof(data));
    if (rc >= 0) {
        /* Zephyr NVS returns zero when an identical entry is already saved. */
        rc = (rc == 0 || rc == sizeof(data)) ? 0 : -EIO;
    }
out:
    k_mutex_unlock(&settings_lock);
    return (int)rc;
}

#else

int ruuvi_history_settings_init(void)
{
    return -ENOTSUP;
}

int ruuvi_history_settings_load(ruuvi_history_config_t *config)
{
    return config == NULL ? -EINVAL : -ENOTSUP;
}

int ruuvi_history_settings_store(const ruuvi_history_config_t *config)
{
    return config == NULL || config->interval_s == 0U ? -EINVAL : -ENOTSUP;
}

#endif
