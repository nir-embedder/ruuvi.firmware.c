#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/bluetooth/att.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/iterable_sections.h>
#include <zephyr/ztest.h>

#include "gatt.h"
#include "ruuvi_endpoints.h"

extern const struct bt_gatt_attr attr_ruuvi_nus[];

static bool subscribed;
static bool reference_available = true;
static unsigned int references;
static unsigned int releases;

bool __wrap_bt_gatt_is_subscribed(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                  uint16_t ccc_type)
{
    return conn != NULL && attr == &attr_ruuvi_nus[4] &&
           ccc_type == BT_GATT_CCC_NOTIFY && subscribed;
}

struct bt_conn *__wrap_bt_conn_ref(struct bt_conn *conn)
{
    ++references;
    return reference_available ? conn : NULL;
}

void __wrap_bt_conn_unref(struct bt_conn *conn)
{
    if (conn != NULL) {
        ++releases;
    }
}

static bool link_connected = true;
static int notify_error;
static uint16_t expected_notification_length;
static enum { COMPLETE_REPLY, HOLD_REPLY, SEND_OLD_REPLY, DISCONNECT_REPLY } notify_mode;
static bt_gatt_complete_func_t old_completion;
static void *old_cookie;
static struct bt_conn *old_conn;

int __wrap_bt_conn_get_info(const struct bt_conn *conn, struct bt_conn_info *info)
{
    if (conn == NULL || !link_connected) {
        return -ENOTCONN;
    }
    memset(info, 0, sizeof(*info));
    info->state = BT_CONN_STATE_CONNECTED;
    return 0;
}

int __wrap_bt_gatt_notify_cb(struct bt_conn *conn, struct bt_gatt_notify_params *params)
{
    zassert_equal(params->attr, &attr_ruuvi_nus[4]);
    zassert_equal(params->len, expected_notification_length);
    if (notify_error != 0) {
        return notify_error;
    }
    if (params->func != NULL) {
        if (notify_mode == HOLD_REPLY) {
            old_completion = params->func;
            old_cookie = params->user_data;
            old_conn = conn;
        } else if (notify_mode == SEND_OLD_REPLY) {
            zassert_not_null(old_completion);
            old_completion(old_conn, old_cookie);
        } else if (notify_mode == DISCONNECT_REPLY) {
            size_t count = 0;
            STRUCT_SECTION_COUNT(bt_conn_cb, &count);
            zassert_equal(count, 1U); /* Never call an unrelated callback with a fake conn. */
            STRUCT_SECTION_FOREACH(bt_conn_cb, cb) {
                zassert_not_null(cb->disconnected);
                cb->disconnected(conn, 0);
            }
        } else {
            params->func(conn, params->user_data);
        }
    }
    return 0;
}

ZTEST(ruuvi_gatt, test_rx_validation_and_queue_ownership)
{
    const struct bt_gatt_attr *rx = &attr_ruuvi_nus[2];
    struct bt_conn *peer = (struct bt_conn *)(uintptr_t)1U;
#if RUUVI_HISTORY_ENABLED
    const uint8_t destination = RE_STANDARD_DESTINATION_TEMPERATURE;
    const uint8_t operation = RE_STANDARD_LOG_VALUE_READ;
#else
    const uint8_t destination = RE_STANDARD_DESTINATION_PASSWORD;
    const uint8_t operation = RE_STANDARD_VALUE_READ;
#endif
    uint8_t request[11] = {destination, 0xA5, operation};
    uint8_t extended[20] = {0};
    ruuvi_gatt_request_t queued = {0};

    memcpy(extended, request, sizeof(request));
    memset(extended + sizeof(request), 0x5A, sizeof(extended) - sizeof(request));

    subscribed = false;
    reference_available = true;
    references = 0;
    releases = 0;
    zassert_not_null(rx->write);
    zassert_equal(ruuvi_gatt_request_take(NULL, K_NO_WAIT), -EINVAL);
    zassert_equal(rx->write(peer, rx, request, sizeof(request), 0,
                            BT_GATT_WRITE_FLAG_PREPARE),
                  BT_GATT_ERR(BT_ATT_ERR_NOT_SUPPORTED));
    zassert_equal(rx->write(peer, rx, request, sizeof(request), 1, 0),
                  BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET));
    zassert_equal(rx->write(peer, rx, request, sizeof(request) - 1U, 0, 0),
                  BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN));
    zassert_equal(rx->write(NULL, rx, request, sizeof(request), 0, 0),
                  BT_GATT_ERR(BT_ATT_ERR_AUTHORIZATION));
    zassert_equal(rx->write(peer, rx, request, sizeof(request), 0, 0),
                  BT_GATT_ERR(BT_ATT_ERR_AUTHORIZATION));
    zassert_equal(references, 0U);

    subscribed = true;
    reference_available = false;
    zassert_equal(rx->write(peer, rx, request, sizeof(request), 0, 0),
                  BT_GATT_ERR(BT_ATT_ERR_UNLIKELY));
    zassert_equal(references, 1U);
    reference_available = true;
    zassert_equal(rx->write(peer, rx, extended, sizeof(extended), 0,
                            BT_GATT_WRITE_FLAG_CMD), sizeof(extended));
    zassert_equal(references, 2U);
    for (uint8_t i = 1U; i < RUUVI_GATT_REQUEST_QUEUE_LEN; ++i) {
        request[3] = i;
        zassert_equal(rx->write(peer, rx, request, sizeof(request), 0, 0),
                      sizeof(request));
    }
    request[3] = 0xFE;
    zassert_equal(rx->write(peer, rx, request, sizeof(request), 0, 0),
                  BT_GATT_ERR(BT_ATT_ERR_INSUFFICIENT_RESOURCES));
    zassert_equal(references, RUUVI_GATT_REQUEST_QUEUE_LEN + 2U);
    zassert_equal(releases, 1U);

    request[0] = 0;
    zassert_equal(ruuvi_gatt_request_take(&queued, K_NO_WAIT), 0);
    zassert_equal(queued.conn, peer);
    zassert_equal(queued.data[0], destination);
    zassert_equal(queued.data[2], operation);
    zassert_mem_equal(queued.data, extended, sizeof(queued.data));
    ruuvi_gatt_request_release(&queued);
    zassert_is_null(queued.conn);
    for (uint8_t i = 1U; i < RUUVI_GATT_REQUEST_QUEUE_LEN; ++i) {
        zassert_equal(ruuvi_gatt_request_take(&queued, K_NO_WAIT), 0);
        zassert_equal(queued.conn, peer);
        zassert_equal(queued.data[0], destination);
        zassert_equal(queued.data[3], i);
        ruuvi_gatt_request_release(&queued);
    }
    zassert_equal(releases, RUUVI_GATT_REQUEST_QUEUE_LEN + 1U);
    request[0] = destination;
    request[3] = 0;
    zassert_equal(rx->write(peer, rx, request, sizeof(request), 0, 0), sizeof(request));
    zassert_equal(ruuvi_gatt_request_take(&queued, K_NO_WAIT), 0);
    zassert_mem_equal(queued.data, request, sizeof(request));
    ruuvi_gatt_request_release(&queued);
    zassert_equal(references, RUUVI_GATT_REQUEST_QUEUE_LEN + 3U);
    zassert_equal(releases, RUUVI_GATT_REQUEST_QUEUE_LEN + 2U);
    zassert_not_equal(ruuvi_gatt_request_take(&queued, K_NO_WAIT), 0);
#if !RUUVI_HISTORY_ENABLED
    request[0] = RE_STANDARD_DESTINATION_TEMPERATURE;
    request[2] = RE_STANDARD_LOG_VALUE_READ;
    zassert_equal(rx->write(peer, rx, request, sizeof(request), 0, 0), sizeof(request));
    zassert_equal(ruuvi_gatt_request_take(&queued, K_NO_WAIT), 0);
    zassert_mem_equal(queued.data, request, sizeof(request));
    ruuvi_gatt_request_release(&queued);
    zassert_equal(references, RUUVI_GATT_REQUEST_QUEUE_LEN + 4U);
    zassert_equal(releases, RUUVI_GATT_REQUEST_QUEUE_LEN + 3U);
#endif
}

ZTEST(ruuvi_gatt, test_password_completion_and_timeout_tickets)
{
    struct bt_conn *peer = (struct bt_conn *)(uintptr_t)1U;
    const uint8_t password_reply[11] = {
        0xA5, RE_STANDARD_DESTINATION_PASSWORD, RE_STANDARD_VALUE_WRITE,
    };
    const uint8_t log_reply[11] = {
        0xA5, RE_STANDARD_DESTINATION_TEMPERATURE, RE_STANDARD_LOG_VALUE_WRITE,
    };
    uint8_t broadcast[24] = {0};

    subscribed = true;
    link_connected = true;
    notify_error = 0;
    notify_mode = COMPLETE_REPLY;
    expected_notification_length = sizeof(password_reply);
    old_completion = NULL;
    zassert_equal(ruuvi_gatt_command_send(NULL, password_reply), -EINVAL);
    zassert_equal(ruuvi_gatt_command_send(peer, NULL), -EINVAL);
    link_connected = false;
    zassert_equal(ruuvi_gatt_command_send(peer, password_reply), -ENOTCONN);
    link_connected = true;
    subscribed = false;
    zassert_equal(ruuvi_gatt_command_send(peer, password_reply), -EACCES);
    subscribed = true;

    notify_error = -ENOMEM;
    zassert_equal(ruuvi_gatt_command_send(peer, password_reply), -EAGAIN);
    notify_error = 0;
    zassert_equal(ruuvi_gatt_command_send(peer, password_reply), 0);

    notify_mode = HOLD_REPLY;
    zassert_equal(ruuvi_gatt_command_send(peer, password_reply), -ETIMEDOUT);
    zassert_not_null(old_completion);
    notify_mode = SEND_OLD_REPLY;
    zassert_equal(ruuvi_gatt_command_send(peer, password_reply), -ETIMEDOUT);
    notify_mode = COMPLETE_REPLY;
    zassert_equal(ruuvi_gatt_command_send(peer, password_reply), 0);
    notify_mode = DISCONNECT_REPLY;
    zassert_equal(ruuvi_gatt_command_send(peer, password_reply), -ENOTCONN);
    notify_mode = COMPLETE_REPLY;
    zassert_equal(ruuvi_gatt_command_send(peer, password_reply), 0);

    notify_error = -ENOMEM;
    zassert_equal(ruuvi_gatt_command_send(peer, log_reply), -EAGAIN);
    notify_error = 0;
    zassert_equal(ruuvi_gatt_command_send(peer, log_reply), 0);
    zassert_equal(ruuvi_gatt_notify(NULL, sizeof(broadcast)), -EINVAL);
    expected_notification_length = 18U;
    zassert_equal(ruuvi_gatt_notify(broadcast, sizeof(broadcast)), 0);
    notify_error = -ENOTCONN;
    zassert_equal(ruuvi_gatt_notify(broadcast, sizeof(broadcast)), 0);
}

ZTEST(ruuvi_gatt, test_legacy_ad_serialization_order)
{
    uint8_t manufacturer[26] = {0x99, 0x04, 0x05};
    struct bt_data no_uuid[] = {
        BT_DATA_BYTES(BT_DATA_FLAGS, BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR),
        BT_DATA(BT_DATA_MANUFACTURER_DATA, manufacturer, sizeof(manufacturer)),
    };
    struct bt_data with_uuid[] = {
        BT_DATA_BYTES(BT_DATA_FLAGS, BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR),
        BT_DATA_BYTES(BT_DATA_UUID16_SOME, 0x98, 0xFC),
        BT_DATA(BT_DATA_MANUFACTURER_DATA, manufacturer, 22U),
    };
    uint8_t primary[31];
    size_t length = 0;

    zassert_equal(bt_data_get_len(no_uuid, ARRAY_SIZE(no_uuid)), sizeof(primary));
    for (size_t i = 0; i < ARRAY_SIZE(no_uuid); ++i) {
        length += bt_data_serialize(&no_uuid[i], primary + length);
    }
    const uint8_t df5_header[] = {0x02, 0x01, 0x06, 0x1B, 0xFF, 0x99, 0x04, 0x05};
    zassert_equal(length, sizeof(primary));
    zassert_mem_equal(primary, df5_header, sizeof(df5_header));
    zassert_mem_equal(primary + 5U, manufacturer, sizeof(manufacturer));

    manufacturer[2] = 0x07;
    length = 0;
    zassert_equal(bt_data_get_len(with_uuid, ARRAY_SIZE(with_uuid)), sizeof(primary));
    for (size_t i = 0; i < ARRAY_SIZE(with_uuid); ++i) {
        length += bt_data_serialize(&with_uuid[i], primary + length);
    }
    const uint8_t df7_header[] = {
        0x02, 0x01, 0x06, 0x03, 0x02, 0x98, 0xFC, 0x17, 0xFF, 0x99, 0x04, 0x07,
    };
    zassert_equal(length, sizeof(primary));
    zassert_mem_equal(primary, df7_header, sizeof(df7_header));
    zassert_mem_equal(primary + 9U, manufacturer, 22U);

    manufacturer[2] = 0xC5;
    with_uuid[2].data_len = 20U;
    length = 0;
    zassert_equal(bt_data_get_len(with_uuid, ARRAY_SIZE(with_uuid)), 29U);
    for (size_t i = 0; i < ARRAY_SIZE(with_uuid); ++i) {
        length += bt_data_serialize(&with_uuid[i], primary + length);
    }
    zassert_equal(length, 29U);
    zassert_equal(primary[7], 0x15);
    zassert_mem_equal(primary + 9U, manufacturer, 20U);

    const uint8_t nus_uuid[] = {
        BT_UUID_128_ENCODE(0x6e400001, 0xb5a3, 0xf393, 0xe0a9, 0xe50e24dcca9e),
    };
    const char device_name[] = "Ruuvi 884F";
    const struct bt_data scan_data[] = {
        BT_DATA(BT_DATA_UUID128_ALL, nus_uuid, sizeof(nus_uuid)),
        BT_DATA(BT_DATA_NAME_COMPLETE, device_name, sizeof(device_name) - 1U),
    };
    uint8_t scan[30];
    length = 0;
    zassert_equal(bt_data_get_len(scan_data, ARRAY_SIZE(scan_data)), sizeof(scan));
    for (size_t i = 0; i < ARRAY_SIZE(scan_data); ++i) {
        length += bt_data_serialize(&scan_data[i], scan + length);
    }
    const uint8_t expected_scan[] = {
        0x11, 0x07, 0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,
        0x93, 0xF3, 0xA3, 0xB5, 0x01, 0x00, 0x40, 0x6E,
        0x0B, 0x09, 'R', 'u', 'u', 'v', 'i', ' ', '8', '8', '4', 'F',
    };
    zassert_equal(length, sizeof(scan));
    zassert_mem_equal(scan, expected_scan, sizeof(expected_scan));
}

ZTEST_SUITE(ruuvi_gatt, NULL, NULL, NULL, NULL, NULL);
