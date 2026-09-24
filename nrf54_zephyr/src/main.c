#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/watchdog.h>
#if RUUVI_GATT_ENABLED || defined(CONFIG_NFC_T4T_NRFXLIB)
#include <zephyr/drivers/hwinfo.h>
#endif
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#include "formats.h"
#include "gatt.h"
#include "history.h"
#include "history_settings.h"
#include "log_service.h"
#if defined(CONFIG_NFC_T4T_NRFXLIB)
#include "nfc.h"
#endif
#include "ruuvi_endpoint_5.h"
#include "ruuvi_endpoint_7.h"
#include "sensors.h"
#include "ui.h"

LOG_MODULE_REGISTER(ruuvi_beacon, LOG_LEVEL_INF);

#define APP_HEARTBEAT_MS (RUUVI_BLE_INTERVAL_MS * RUUVI_NUM_REPEATS)
#define APP_FAST_ADV_TIME_MS 5000
#define APP_FAST_ADV_UNITS 160 /* 100 ms at 0.625 ms per unit */
#define APP_NORMAL_ADV_UNITS ((RUUVI_BLE_INTERVAL_MS * 8U + 2U) / 5U)
#define APP_WDT_TIMEOUT_MS RUUVI_WDT_TIMEOUT_MS
#define APP_GATT_TURBO_DELAY_MS 30000
#define APP_CONFIG_WINDOW_MS 60000U
#define APP_FAST_ADV_REPEATS MAX(APP_HEARTBEAT_MS / 100U, 1U)

BUILD_ASSERT(RUUVI_NUM_REPEATS > 0U && RUUVI_NUM_REPEATS < 255U &&
             APP_FAST_ADV_REPEATS < 255U, "Advertising event count must fit the controller");

typedef struct {
    uint8_t manufacturer[2U + RUUVI_FORMAT_MAX_LENGTH];
    uint8_t length;
    uint8_t ad_count;
    uint8_t repeats;
    bool fast;
} ruuvi_adv_frame_t;

/* Match the SDK5 queue's three pending frames; an active set is separate. */
K_MSGQ_DEFINE(adv_queue, sizeof(ruuvi_adv_frame_t), 3, 4);
K_SEM_DEFINE(adv_sent, 0, 1);

#if DT_NODE_EXISTS(DT_NODELABEL(ruuvi_history_partition)) && RUUVI_HISTORY_ENABLED
static const struct bt_le_conn_param gatt_turbo = BT_LE_CONN_PARAM_INIT(12, 24, 0, 600);
#endif
#if RUUVI_GATT_ENABLED
static const struct bt_le_conn_param gatt_low_power = BT_LE_CONN_PARAM_INIT(1560, 1584, 0, 600);
#endif

#if DT_NODE_EXISTS(DT_NODELABEL(ruuvi_history_partition)) && RUUVI_HISTORY_ENABLED
BUILD_ASSERT(IS_ENABLED(CONFIG_FLASH_MAP) && IS_ENABLED(CONFIG_FLASH_PAGE_LAYOUT) &&
             IS_ENABLED(CONFIG_NVS) && IS_ENABLED(CONFIG_USE_DT_CODE_PARTITION),
             "History layout requires flash map, NVS and code partition settings");
BUILD_ASSERT(DT_REG_SIZE(DT_NODELABEL(ruuvi_history_partition)) >=
             15U * 4096U, "History needs 14 pages and an overwrite journal");
#if DT_NODE_EXISTS(DT_NODELABEL(slot0_partition)) && \
    DT_NODE_EXISTS(DT_NODELABEL(slot1_partition)) && \
    DT_NODE_EXISTS(DT_NODELABEL(storage_partition))
BUILD_ASSERT(DT_REG_SIZE(DT_NODELABEL(slot0_partition)) ==
             DT_REG_SIZE(DT_NODELABEL(slot1_partition)), "DFU image slots must match");
BUILD_ASSERT(DT_REG_ADDR(DT_NODELABEL(slot1_partition)) >=
             DT_REG_ADDR(DT_NODELABEL(slot0_partition)) +
             DT_REG_SIZE(DT_NODELABEL(slot0_partition)), "Image slots overlap");
BUILD_ASSERT(DT_REG_ADDR(DT_NODELABEL(ruuvi_history_partition)) >=
             DT_REG_ADDR(DT_NODELABEL(slot1_partition)) +
             DT_REG_SIZE(DT_NODELABEL(slot1_partition)), "History overlaps image slot");
BUILD_ASSERT(DT_REG_ADDR(DT_NODELABEL(storage_partition)) >=
             DT_REG_ADDR(DT_NODELABEL(ruuvi_history_partition)) +
             DT_REG_SIZE(DT_NODELABEL(ruuvi_history_partition)),
             "Settings overlap history");
#else
#error "History needs explicit primary/secondary image and settings partitions"
#endif
#endif

static atomic_t connected;
static atomic_t advertising;

static void on_adv_sent(struct bt_le_ext_adv *adv, struct bt_le_ext_adv_sent_info *info)
{
    ARG_UNUSED(adv);
    ARG_UNUSED(info);
    atomic_clear(&advertising);
    k_sem_give(&adv_sent);
}

static const struct bt_le_ext_adv_cb adv_callbacks = {
    .sent = on_adv_sent,
};

#if RUUVI_GATT_ENABLED
static atomic_t config_next;
static atomic_t config_current;
static atomic_t config_window_start_ms;

static void config_window_open(void)
{
    atomic_set(&config_window_start_ms, (atomic_val_t)k_uptime_get_32());
    atomic_set(&config_next, 1);
}

static void disconnect_for_config(struct bt_conn *conn, void *user_data)
{
    struct bt_conn_info info;

    ARG_UNUSED(user_data);
    if (bt_conn_get_info(conn, &info) == 0 && info.state == BT_CONN_STATE_CONNECTED) {
        int rc = bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
        if (rc != 0) {
            LOG_WRN("Disconnect for configuration failed: %d", rc);
        }
    }
}

static void on_connected(struct bt_conn *conn, uint8_t status)
{
    ARG_UNUSED(conn);
    atomic_clear(&advertising); /* Connectable legacy advertising stops on connection. */
    if (status == 0) {
        k_msgq_purge(&adv_queue);
        bool in_window = atomic_get(&config_next) != 0 &&
            (uint32_t)(k_uptime_get_32() -
                       (uint32_t)atomic_get(&config_window_start_ms)) < APP_CONFIG_WINDOW_MS;
        atomic_set(&config_current, in_window ? 1 : 0);
        atomic_clear(&config_next);
        atomic_set(&connected, 1);
        if (in_window) {
            LOG_INF("Configuration window connection active; writes remain unsupported");
        }
    }
}

static void on_disconnected(struct bt_conn *conn, uint8_t reason)
{
    ARG_UNUSED(conn);
    ARG_UNUSED(reason);
    atomic_clear(&config_current);
    atomic_clear(&connected);
    atomic_clear(&advertising);
    k_msgq_purge(&adv_queue);
}

BT_CONN_CB_DEFINE(ruuvi_connections) = {
    .connected = on_connected,
    .disconnected = on_disconnected,
};
#endif

#if RUUVI_GATT_ENABLED
static int send_log_reply(void *context, const uint8_t *data, size_t length)
{
    if (length != RUUVI_LOG_MESSAGE_LENGTH) {
        return -EINVAL;
    }
    return ruuvi_gatt_command_send((struct bt_conn *)context, data);
}
#endif

int main(void)
{
    const uint32_t adv_options = BT_LE_ADV_OPT_USE_IDENTITY |
        (RUUVI_GATT_ENABLED ? (BT_LE_ADV_OPT_CONN | BT_LE_ADV_OPT_SCANNABLE) : 0U);
    const struct bt_le_adv_param fast_adv =
        BT_LE_ADV_PARAM_INIT(adv_options, APP_FAST_ADV_UNITS, APP_FAST_ADV_UNITS, NULL);
    const struct bt_le_adv_param normal_adv =
        BT_LE_ADV_PARAM_INIT(adv_options, APP_NORMAL_ADV_UNITS, APP_NORMAL_ADV_UNITS, NULL);
    struct bt_le_ext_adv *advertiser = NULL;
    ruuvi_adv_frame_t on_air = {0};
    uint8_t manufacturer[2 + RUUVI_FORMAT_MAX_LENGTH] = { 0x99, 0x04 };
    struct bt_data ad[] = {
        BT_DATA_BYTES(BT_DATA_FLAGS, BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR),
        BT_DATA(BT_DATA_MANUFACTURER_DATA, manufacturer, sizeof(manufacturer)),
        BT_DATA_BYTES(BT_DATA_UUID16_SOME, 0x98, 0xFC),
    };
    char device_name[sizeof("Ruuvi FFFF")] = "Ruuvi 0000";
    const struct bt_data sd[] = {
        BT_DATA(BT_DATA_NAME_COMPLETE, device_name, sizeof(device_name) - 1U),
        BT_DATA_BYTES(BT_DATA_UUID128_ALL,
                      BT_UUID_128_ENCODE(0x6e400001, 0xb5a3, 0xf393,
                                         0xe0a9, 0xe50e24dcca9e)),
    };
#if RUUVI_GATT_ENABLED && defined(CONFIG_MCUMGR_TRANSPORT_BT)
    const struct bt_data sd_dfu[] = {
        BT_DATA(BT_DATA_NAME_COMPLETE, device_name, sizeof(device_name) - 1U),
        BT_DATA_BYTES(BT_DATA_UUID128_ALL,
                      BT_UUID_128_ENCODE(0x8d53dc1d, 0x1db7, 0x4cd3,
                                         0x868b, 0x8a527460aa84)),
    };
#endif
    const struct bt_data *scan_response = sd;
    size_t adv_count = 2U;
    const size_t scan_rsp_count = RUUVI_GATT_ENABLED ? ARRAY_SIZE(sd) : 0U;
    bt_addr_le_t identity;
    size_t identity_count = 1;
    uint64_t address = 0;
#if RUUVI_GATT_ENABLED || defined(CONFIG_NFC_T4T_NRFXLIB)
    uint8_t device_id[8] = {0};
    bool have_device_id = false;
#endif
#if RUUVI_GATT_ENABLED
    bool config_pending_seen = false;
#endif
    uint16_t sequence[6] = {0}; /* Separate legacy counters for each format. */
    ruuvi_format_t format = RUUVI_FORMAT_INVALID;
    bool recovery_reported = false;
    int err = ruuvi_ui_init();

    if (err) {
        LOG_ERR("GPIO UI initialization failed: %d", err);
        return err;
    }
    ruuvi_ui_error(true);

#if DT_NODE_EXISTS(DT_NODELABEL(ruuvi_history_partition)) && RUUVI_HISTORY_ENABLED
    uint32_t history_base_s = 0;
    ruuvi_history_config_t log_config;

    err = ruuvi_history_init();
    if (err) {
        LOG_ERR("History initialization failed: %d", err);
        return err;
    }
    err = ruuvi_history_settings_init();
    if (err) {
        LOG_ERR("History settings mount failed: %d", err);
        return err;
    }
    err = ruuvi_history_settings_load(&log_config);
    if (err == -ENOENT) {
        err = ruuvi_history_config_get(&log_config);
        if (!err) {
            log_config.interval_s = RUUVI_LOG_INTERVAL_S;
            err = ruuvi_history_config_set(&log_config);
        }
        if (!err) {
            err = ruuvi_history_settings_store(&log_config);
        }
    } else if (!err) {
        err = ruuvi_history_config_set(&log_config);
    }
    if (err) {
        LOG_ERR("History configuration failed: %d", err);
        return err;
    }
    err = ruuvi_history_latest_timestamp(&history_base_s);
    if (err == 0 && history_base_s != UINT32_MAX) {
        ++history_base_s;
    } else if (err == -ENOENT) {
        history_base_s = 0;
    } else {
        LOG_ERR("History clock recovery failed: %d", err);
        return err ? err : -EOVERFLOW;
    }
#endif

#if DT_NODE_HAS_STATUS(DT_ALIAS(watchdog0), okay)
    const struct device *wdt = DEVICE_DT_GET(DT_ALIAS(watchdog0));
    const struct wdt_timeout_cfg wdt_config = {
        .flags = WDT_FLAG_RESET_SOC,
        .window = { .min = 0, .max = APP_WDT_TIMEOUT_MS },
    };

    if (!device_is_ready(wdt)) {
        LOG_ERR("Watchdog device is not ready");
        return -1;
    }
    int wdt_channel = wdt_install_timeout(wdt, &wdt_config);
    if (wdt_channel < 0) {
        LOG_ERR("Watchdog timeout installation failed: %d", wdt_channel);
        return wdt_channel;
    }
    err = wdt_setup(wdt, 0);
    if (err) {
        LOG_ERR("Watchdog setup failed: %d", err);
        return err;
    }
#else
    LOG_WRN("No watchdog0 alias: heartbeat is not protected by a watchdog");
#endif

    err = bt_enable(NULL);
    if (err) {
        LOG_ERR("Bluetooth initialization failed: %d", err);
        return err;
    }

    bt_id_get(&identity, &identity_count);
    if (identity_count != 1) {
        LOG_ERR("No Bluetooth identity address available");
        return -1;
    }

    /* Zephyr stores BLE addresses least-significant octet first. DF5 encodes MSB first. */
    for (size_t i = 0; i < sizeof(identity.a.val); ++i) {
        address |= (uint64_t)identity.a.val[i] << (i * 8U);
    }
#if RUUVI_GATT_ENABLED
    (void)snprintf(device_name, sizeof(device_name), "Ruuvi %04X",
                   (unsigned int)(address & 0xFFFFU));
    err = bt_set_name(device_name);
    if (err) {
        LOG_ERR("Setting BLE device name failed: %d", err);
        return err;
    }
#endif
#if RUUVI_GATT_ENABLED || defined(CONFIG_NFC_T4T_NRFXLIB)
    uint8_t raw_id[8];
    ssize_t id_size = hwinfo_get_device_id(raw_id, sizeof(raw_id));
    if (id_size == sizeof(raw_id)) {
        /* Zephyr's Nordic hwinfo emits DEVICEID[1], then DEVICEID[0] in BE;
         * legacy ri_comm_id_get compares DEVICEID[0], then DEVICEID[1].
         */
        memcpy(device_id, raw_id + 4, 4);
        memcpy(device_id + 4, raw_id, 4);
        have_device_id = true;
    } else {
        LOG_WRN("Device ID unavailable (%d); password unlock disabled", (int)id_size);
    }
#endif
#if defined(CONFIG_NFC_T4T_NRFXLIB)
    err = ruuvi_nfc_init(address, have_device_id ? device_id : NULL);
    if (err != 0) {
        LOG_ERR("NFC Type 4 Tag setup failed: %d", err);
        return err;
    }
#endif
    err = bt_le_ext_adv_create(&fast_adv, &adv_callbacks, &advertiser);
    if (err != 0) {
        LOG_ERR("Creating BLE advertising set failed: %d", err);
        return err;
    }
    ruuvi_ui_error(false);
    bool normal_mode = false;
    bool configured_fast = true;
#if defined(CONFIG_NFC_T4T_NRFXLIB) || (RUUVI_GATT_ENABLED && defined(CONFIG_MCUMGR_TRANSPORT_BT))
    bool have_payload = false;
#endif
#if defined(CONFIG_NFC_T4T_NRFXLIB)
    size_t latest_payload_len = 0;
#endif
    bool no_sensors_reported = false;
#if RUUVI_GATT_ENABLED
    ruuvi_log_service_t log_service = {0};
    ruuvi_gatt_request_t active_request = {0};
#if DT_NODE_EXISTS(DT_NODELABEL(ruuvi_history_partition)) && RUUVI_HISTORY_ENABLED
    bool turbo_requested = false;
    int64_t stream_started_ms = 0;
#endif
#endif
#if defined(CONFIG_NFC_T4T_NRFXLIB)
    ruuvi_log_service_t nfc_service = {0};
    bool nfc_was_present = false;
#endif
    int64_t next_heartbeat_ms = k_uptime_get();
    const int64_t fast_deadline_ms = next_heartbeat_ms + APP_FAST_ADV_TIME_MS;

    while (true) {
#if defined(CONFIG_NFC_T4T_NRFXLIB)
        bool nfc_present = ruuvi_nfc_field_active();
        if (nfc_present) {
            if (atomic_get(&advertising)) {
                err = bt_le_ext_adv_stop(advertiser);
                if (err == 0) {
                    atomic_clear(&advertising);
                } else {
                    LOG_WRN("Stopping BLE for NFC field failed: %d", err);
                }
            }
            k_msgq_purge(&adv_queue);
#if RUUVI_GATT_ENABLED
            if (!nfc_was_present) {
                bt_conn_foreach(BT_CONN_TYPE_LE, disconnect_for_config, NULL);
                if (ruuvi_log_service_active(&log_service)) {
                    ruuvi_log_service_abort(&log_service);
                    ruuvi_gatt_request_release(&active_request);
#if DT_NODE_EXISTS(DT_NODELABEL(ruuvi_history_partition)) && RUUVI_HISTORY_ENABLED
                    turbo_requested = false;
#endif
                }
            }
#endif
        }
        nfc_was_present = nfc_present;
        bool nfc_field_ended = ruuvi_nfc_field_off_take();
#if RUUVI_GATT_ENABLED
        if (nfc_field_ended && !ruuvi_ui_recovery_requested()) {
            config_window_open(); /* As in legacy, NFC field-off opens the next BLE link. */
        }
#else
        ARG_UNUSED(nfc_field_ended);
#endif
#endif
#if RUUVI_GATT_ENABLED
        bool config_pending = ruuvi_ui_config_pending();
        if (config_pending && !config_pending_seen) {
            config_window_open();
            bt_conn_foreach(BT_CONN_TYPE_LE, disconnect_for_config, NULL);
        }
        config_pending_seen = config_pending;
        if (atomic_get(&config_next) &&
            (uint32_t)(k_uptime_get_32() -
                       (uint32_t)atomic_get(&config_window_start_ms)) >= APP_CONFIG_WINDOW_MS) {
            atomic_clear(&config_next);
        }
        ruuvi_ui_configuration(atomic_get(&config_next) || atomic_get(&config_current));
#endif
        if (ruuvi_ui_recovery_requested() && !recovery_reported) {
#if RUUVI_GATT_ENABLED
            atomic_clear(&config_next);
            atomic_clear(&config_current);
#endif
            LOG_ERR("Recovery requested: flash purge/bootloader not yet implemented");
            ruuvi_ui_error(true);
            recovery_reported = true;
        }
#if RUUVI_GATT_ENABLED && defined(CONFIG_MCUMGR_TRANSPORT_BT)
        /* Keep legacy NUS discovery normally; expose SMP for the next-link window. */
        const struct bt_data *wanted_response = atomic_get(&config_next) ? sd_dfu : sd;
        if (scan_response != wanted_response) {
            scan_response = wanted_response;
            if (have_payload && atomic_get(&advertising) && !atomic_get(&connected)) {
                int adv_rc = bt_le_ext_adv_set_data(advertiser, ad, on_air.ad_count,
                                                      scan_response, scan_rsp_count);
                if (adv_rc != 0) {
                    LOG_WRN("Updating DFU scan response failed: %d", adv_rc);
                }
            }
        }
#endif
#if defined(CONFIG_NFC_T4T_NRFXLIB)
        bool radio_allowed = !ruuvi_nfc_field_active();
#else
        bool radio_allowed = true;
#endif
        int64_t now = k_uptime_get();
        if (!normal_mode && now >= fast_deadline_ms) {
            /* Existing fast frames retain their settings; only new samples switch. */
            normal_mode = true;
        }
#if defined(CONFIG_NFC_T4T_NRFXLIB)
        if (nfc_field_ended && radio_allowed && have_payload && !atomic_get(&connected)) {
            ruuvi_adv_frame_t resume = {
                .length = (uint8_t)(2U + latest_payload_len),
                .ad_count = (uint8_t)adv_count,
                .repeats = normal_mode ? RUUVI_NUM_REPEATS : APP_FAST_ADV_REPEATS,
                .fast = !normal_mode,
            };
            memcpy(resume.manufacturer, manufacturer, sizeof(resume.manufacturer));
            if (k_msgq_put(&adv_queue, &resume, K_NO_WAIT) != 0) {
                LOG_WRN("BLE advertisement queue full after NFC field");
            }
        }
#endif
        if (radio_allowed && !atomic_get(&connected) && !atomic_get(&advertising) &&
            k_msgq_get(&adv_queue, &on_air, K_NO_WAIT) == 0) {
            int adv_rc = 0;
            if (on_air.fast != configured_fast) {
                adv_rc = bt_le_ext_adv_update_param(advertiser,
                                on_air.fast ? &fast_adv : &normal_adv);
                if (adv_rc == 0) {
                    configured_fast = on_air.fast;
                }
            }
            if (adv_rc == 0) {
                ad[1].data = on_air.manufacturer;
                ad[1].data_len = on_air.length;
                adv_rc = bt_le_ext_adv_set_data(advertiser, ad, on_air.ad_count,
                                                 scan_response, scan_rsp_count);
            }
            if (adv_rc == 0) {
                const struct bt_le_ext_adv_start_param limits =
                    BT_LE_EXT_ADV_START_PARAM_INIT(0, on_air.repeats);
                atomic_set(&advertising, 1);
                adv_rc = bt_le_ext_adv_start(advertiser, &limits);
                if (adv_rc != 0) {
                    atomic_clear(&advertising);
                }
            }
            if (adv_rc != 0) {
                LOG_ERR("Starting queued advertisement failed: %d", adv_rc);
            }
        }
#if defined(CONFIG_NFC_T4T_NRFXLIB)
        if (!ruuvi_nfc_field_active()) {
            uint8_t nfc_command[RUUVI_NFC_COMMAND_LENGTH];
            int nfc_rc = ruuvi_nfc_request_take(nfc_command);
            if (nfc_rc == 0) {
                ruuvi_log_service_abort(&nfc_service); /* New NFC write replaces an old stream. */
#if !DT_NODE_EXISTS(DT_NODELABEL(ruuvi_history_partition)) || !RUUVI_HISTORY_ENABLED
                if (nfc_command[2] == RE_STANDARD_LOG_VALUE_READ) {
                    LOG_WRN("NFC log read unavailable without history partition");
                } else
#endif
                {
                    uint64_t history_now_s = 0;
#if DT_NODE_EXISTS(DT_NODELABEL(ruuvi_history_partition)) && RUUVI_HISTORY_ENABLED
                    history_now_s = (uint64_t)history_base_s +
                                    (uint64_t)k_uptime_get() / 1000U;
#endif
                    int start_rc = ruuvi_log_service_start_with_id(&nfc_service, nfc_command,
                                       sizeof(nfc_command), history_now_s, k_uptime_get(),
                                       have_device_id ? device_id : NULL);
                    if (start_rc != 0) {
                        LOG_WRN("NFC command rejected: %d", start_rc);
                    }
#if RUUVI_GATT_ENABLED
                    else if (nfc_command[0] == RE_STANDARD_DESTINATION_PASSWORD &&
                             nfc_command[2] == RE_STANDARD_VALUE_READ &&
                             !nfc_service.password_match) {
                        atomic_clear(&config_next);
                    }
#endif
                }
            } else if (nfc_rc != -ENOMSG) {
                LOG_WRN("Malformed NFC NDEF command: %d", nfc_rc);
            }
            if (ruuvi_log_service_active(&nfc_service) && ruuvi_nfc_reply_ready()) {
                int reply_rc = ruuvi_log_service_pump(&nfc_service, k_uptime_get(),
                                                       ruuvi_nfc_reply_send, NULL);
                if (reply_rc < 0) {
                    LOG_WRN("NFC reply stopped: %d", reply_rc);
                    ruuvi_log_service_abort(&nfc_service);
                }
            }
        }
#endif
#if RUUVI_GATT_ENABLED
        if (ruuvi_log_service_active(&log_service)) {
            struct bt_conn_info info;
            if (!radio_allowed || !atomic_get(&connected) ||
                bt_conn_get_info(active_request.conn, &info) != 0 ||
                info.state != BT_CONN_STATE_CONNECTED) {
                ruuvi_log_service_abort(&log_service);
                ruuvi_gatt_request_release(&active_request);
#if DT_NODE_EXISTS(DT_NODELABEL(ruuvi_history_partition)) && RUUVI_HISTORY_ENABLED
                turbo_requested = false;
#endif
                next_heartbeat_ms = k_uptime_get();
                continue;
            }
#if DT_NODE_EXISTS(DT_NODELABEL(ruuvi_history_partition)) && RUUVI_HISTORY_ENABLED
            if (!turbo_requested &&
                active_request.data[2] == RE_STANDARD_LOG_VALUE_READ &&
                now - stream_started_ms >= APP_GATT_TURBO_DELAY_MS) {
                int params_rc = bt_conn_le_param_update(active_request.conn, &gatt_turbo);
                if (params_rc < 0) {
                    LOG_WRN("Turbo connection request failed: %d", params_rc);
                }
                turbo_requested = true;
            }
#endif
            bool password_match = log_service.password_match;
            int stream_rc = ruuvi_log_service_pump(&log_service, k_uptime_get(),
                                                     send_log_reply, active_request.conn);
            if (stream_rc < 0) {
                LOG_ERR("NUS stream stopped: %d", stream_rc);
                ruuvi_log_service_abort(&log_service);
            }
            if (!ruuvi_log_service_active(&log_service)) {
                bool password_request = active_request.data[0] == RE_STANDARD_DESTINATION_PASSWORD &&
                    active_request.data[2] == RE_STANDARD_VALUE_READ;
                if (stream_rc == 1 && password_request) {
                    if (password_match) {
                        config_window_open();
                    } else {
                        atomic_clear(&config_next);
                    }
                    /* The legacy unlock applies only to the next connection. */
                    int disconnect_rc = bt_conn_disconnect(active_request.conn,
                                          BT_HCI_ERR_REMOTE_USER_TERM_CONN);
                    if (disconnect_rc != 0) {
                        LOG_WRN("Password disconnect failed: %d", disconnect_rc);
                    }
                } else if (atomic_get(&connected) &&
                           active_request.data[2] == RE_STANDARD_LOG_VALUE_READ) {
                    int params_rc = bt_conn_le_param_update(active_request.conn,
                                                              &gatt_low_power);
                    if (params_rc < 0) {
                        LOG_WRN("Low-power connection request failed: %d", params_rc);
                    }
                }
#if DT_NODE_EXISTS(DT_NODELABEL(ruuvi_history_partition)) && RUUVI_HISTORY_ENABLED
                turbo_requested = false;
#endif
                ruuvi_gatt_request_release(&active_request);
                next_heartbeat_ms = k_uptime_get(); /* Legacy restarts heartbeat now. */
            } else {
                k_sleep(K_MSEC(stream_rc == 0 ? 10 : 5));
            }
            continue;
        }
#endif
        now = k_uptime_get();
        int64_t next_event_ms = next_heartbeat_ms;
        if (!normal_mode && fast_deadline_ms < next_event_ms) {
            next_event_ms = fast_deadline_ms;
        }
        if (now < next_event_ms) {
            struct k_poll_event events[1 + RUUVI_GATT_ENABLED +
                                       IS_ENABLED(CONFIG_NFC_T4T_NRFXLIB)];
            size_t event_count = 0;
            k_poll_event_init(&events[event_count++], K_POLL_TYPE_SEM_AVAILABLE,
                              K_POLL_MODE_NOTIFY_ONLY, &adv_sent);
#if RUUVI_GATT_ENABLED
            k_poll_event_init(&events[event_count++], K_POLL_TYPE_MSGQ_DATA_AVAILABLE,
                              K_POLL_MODE_NOTIFY_ONLY, ruuvi_gatt_request_queue());
#endif
#if defined(CONFIG_NFC_T4T_NRFXLIB)
            k_poll_event_init(&events[event_count++], K_POLL_TYPE_SEM_AVAILABLE,
                              K_POLL_MODE_NOTIFY_ONLY, ruuvi_nfc_event_sem());
#endif
            int poll_rc = k_poll(events, event_count, K_MSEC(next_event_ms - now));
            if (poll_rc != 0 && poll_rc != -EAGAIN) {
                LOG_WRN("BLE/NFC event wait failed: %d", poll_rc);
            }
            (void)k_sem_take(&adv_sent, K_NO_WAIT);
#if defined(CONFIG_NFC_T4T_NRFXLIB)
            (void)k_sem_take(ruuvi_nfc_event_sem(), K_NO_WAIT);
#endif
#if RUUVI_GATT_ENABLED
            ruuvi_gatt_request_t queued = {0};
            int queue_rc = ruuvi_gatt_request_take(&queued, K_NO_WAIT);
            if (queue_rc == 0) {
                struct bt_conn_info info;
                if (!radio_allowed || !atomic_get(&connected) ||
                    bt_conn_get_info(queued.conn, &info) != 0 ||
                    info.state != BT_CONN_STATE_CONNECTED) {
                    ruuvi_gatt_request_release(&queued);
                } else {
                    uint64_t history_now_s = 0;
#if DT_NODE_EXISTS(DT_NODELABEL(ruuvi_history_partition)) && RUUVI_HISTORY_ENABLED
                    history_now_s = (uint64_t)history_base_s +
                                    (uint64_t)k_uptime_get() / 1000U;
#else
                    if (queued.data[2] == RE_STANDARD_LOG_VALUE_READ) {
                        LOG_WRN("Log read unavailable without history partition");
                        ruuvi_gatt_request_release(&queued);
                        continue;
                    }
#endif
                    int start_rc = ruuvi_log_service_start_with_id(&log_service, queued.data,
                                     sizeof(queued.data), history_now_s, k_uptime_get(),
                                     have_device_id ? device_id : NULL);
                    if (start_rc == 0) {
                        active_request = queued;
#if DT_NODE_EXISTS(DT_NODELABEL(ruuvi_history_partition)) && RUUVI_HISTORY_ENABLED
                        stream_started_ms = k_uptime_get();
                        turbo_requested = false;
#endif
                    } else {
                        LOG_WRN("NUS command rejected: %d", start_rc);
                        ruuvi_gatt_request_release(&queued);
                    }
                }
            }
#endif
            continue;
        }
        next_heartbeat_ms = now + APP_HEARTBEAT_MS;
        re_5_data_t sample = { 0 };
        int channels = ruuvi_sensor_read(&sample);

        if (channels < 0) {
            LOG_ERR("Sensor read failed: %d; advertising invalid fields", channels);
            ruuvi_ui_error(true);
            channels = 0;
            no_sensors_reported = true;
        }
        if (channels == 0 && !no_sensors_reported) {
            LOG_WRN("No sensor channels available; physical fields invalid");
            no_sensors_reported = true;
        } else if (channels > 0) {
            no_sensors_reported = false;
        }
        ruuvi_ui_activity(true);

        format = ruuvi_format_next(RUUVI_ENABLED_FORMATS, format);
        if (format == RUUVI_FORMAT_INVALID) {
            LOG_ERR("No data format enabled");
            return -1;
        }
        adv_count = (format == RUUVI_FORMAT_7 || format == RUUVI_FORMAT_8 ||
                     format == RUUVI_FORMAT_C5) ? ARRAY_SIZE(ad) : 2U;
        size_t slot = 0;
        while (format != (ruuvi_format_t)(1U << slot)) {
            ++slot;
        }
        uint16_t max_count = (format == RUUVI_FORMAT_7 || format == RUUVI_FORMAT_FA)
                             ? RE_7_SEQCTR_MAX : RE_5_SEQCTR_MAX;
        sequence[slot] = (sequence[slot] >= max_count) ? 0 : sequence[slot] + 1;

        const ruuvi_measurement_t reading = {
            .humidity_rh = sample.humidity_rh,
            .pressure_pa = sample.pressure_pa,
            .temperature_c = sample.temperature_c,
            .accelerationx_g = sample.accelerationx_g,
            .accelerationy_g = sample.accelerationy_g,
            .accelerationz_g = sample.accelerationz_g,
            .battery_v = sample.battery_v,
            .luminosity_lux = NAN,
            .color_temp_k = NAN,
            .measurement_count = sequence[slot],
            .movement_count = 0, /* Motion interrupts have not been connected. */
            .address = address,
            .tx_power = RE_5_INVALID_POWER, /* Controller TX power is unmeasured. */
        };
        size_t payload_length = RUUVI_FORMAT_MAX_LENGTH;
        memset(&manufacturer[2], 0, payload_length);
        err = ruuvi_format_encode(format, &reading, NULL, &manufacturer[2],
                                  &payload_length);
        if (err) {
            LOG_ERR("Data format %u encoding failed: %d", (unsigned int)format, err);
        } else {
#if defined(CONFIG_NFC_T4T_NRFXLIB) || (RUUVI_GATT_ENABLED && defined(CONFIG_MCUMGR_TRANSPORT_BT))
            have_payload = true;
#endif
#if defined(CONFIG_NFC_T4T_NRFXLIB)
            latest_payload_len = payload_length;
#endif
            bool heartbeat_ok = false;

            if (radio_allowed && !atomic_get(&connected)) {
                ruuvi_adv_frame_t frame = {
                    .length = (uint8_t)(2U + payload_length),
                    .ad_count = (uint8_t)adv_count,
                    .repeats = normal_mode ? RUUVI_NUM_REPEATS : APP_FAST_ADV_REPEATS,
                    .fast = !normal_mode,
                };
                memcpy(frame.manufacturer, manufacturer, sizeof(frame.manufacturer));
                err = k_msgq_put(&adv_queue, &frame, K_NO_WAIT);
                if (err != 0) {
                    LOG_ERR("BLE advertisement queue full: %d", err);
                } else {
                    heartbeat_ok = true;
                }
            }

#if RUUVI_GATT_ENABLED
            if (radio_allowed) {
                err = ruuvi_gatt_notify(&manufacturer[2], 18U);
                if (err) {
                    LOG_ERR("GATT notification failed: %d", err);
                } else if (atomic_get(&connected)) {
                    heartbeat_ok = true;
                }
            }
#endif
#if defined(CONFIG_NFC_T4T_NRFXLIB)
            int nfc_rc = ruuvi_nfc_update(&manufacturer[2], payload_length);
            if (nfc_rc == 0) {
                heartbeat_ok = true;
            } else if (nfc_rc != -EAGAIN) {
                LOG_WRN("NFC payload update failed: %d", nfc_rc);
            }
#endif
#if DT_NODE_HAS_STATUS(DT_ALIAS(watchdog0), okay)
            if (heartbeat_ok) {
                err = wdt_feed(wdt, wdt_channel);
                if (err) {
                    LOG_ERR("Watchdog feed failed: %d", err);
                }
            }
#else
            ARG_UNUSED(heartbeat_ok);
#endif
        }

        ruuvi_ui_activity(false);
#if DT_NODE_EXISTS(DT_NODELABEL(ruuvi_history_partition)) && RUUVI_HISTORY_ENABLED
        /* Legacy app_log_process records timestamps even when every field is invalid. */
        uint64_t seconds = (uint64_t)history_base_s +
                           (uint64_t)k_uptime_get() / 1000U;
        if (seconds > UINT32_MAX) {
            LOG_ERR("History timestamp overflow");
            ruuvi_ui_error(true);
        } else {
            ruuvi_history_element_t entry = {
                .timestamp_s = (uint32_t)seconds,
                .temperature_c = sample.temperature_c,
                .humidity_rh = sample.humidity_rh,
                .pressure_pa = sample.pressure_pa,
            };
            int log_rc = ruuvi_history_process(&entry);
            if (log_rc < 0) {
                LOG_ERR("History sample failed: %d", log_rc);
                ruuvi_ui_error(true);
            }
        }
#endif
    }
}
