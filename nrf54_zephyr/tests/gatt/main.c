#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/bluetooth/att.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/kernel.h>
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
static enum { COMPLETE_REPLY, HOLD_REPLY, SEND_OLD_REPLY } notify_mode;
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
    ruuvi_gatt_request_t queued = {0};

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
#if !RUUVI_HISTORY_ENABLED
    request[2] = RE_STANDARD_LOG_VALUE_READ;
    zassert_equal(rx->write(peer, rx, request, sizeof(request), 0, 0),
                  BT_GATT_ERR(BT_ATT_ERR_NOT_SUPPORTED));
    request[2] = operation;
#endif
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
    zassert_equal(rx->write(peer, rx, request, sizeof(request), 0,
                            BT_GATT_WRITE_FLAG_CMD), sizeof(request));
    zassert_equal(references, 2U);
    zassert_equal(rx->write(peer, rx, request, sizeof(request), 0, 0),
                  BT_GATT_ERR(BT_ATT_ERR_INSUFFICIENT_RESOURCES));
    zassert_equal(references, 3U);
    zassert_equal(releases, 1U);

    request[0] = 0;
    zassert_equal(ruuvi_gatt_request_take(&queued, K_NO_WAIT), 0);
    zassert_equal(queued.conn, peer);
    zassert_equal(queued.data[0], destination);
    zassert_equal(queued.data[2], operation);
    ruuvi_gatt_request_release(&queued);
    zassert_is_null(queued.conn);
    zassert_equal(releases, 2U);
    zassert_not_equal(ruuvi_gatt_request_take(&queued, K_NO_WAIT), 0);
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

ZTEST_SUITE(ruuvi_gatt, NULL, NULL, NULL, NULL, NULL);
