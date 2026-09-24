#include "gatt.h"
#include "ruuvi_endpoints.h"

#include <errno.h>
#include <stdbool.h>
#include <string.h>

#include <zephyr/bluetooth/att.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>

#define RUUVI_GATT_DF5_LENGTH 18U

static K_SEM_DEFINE(password_reply_sent, 0, 1);
static K_MUTEX_DEFINE(password_lock);
static struct bt_conn *password_conn;
static uint32_t password_ticket;
static bool password_pending;
static int password_result;

static void password_reply_done(struct bt_conn *conn, void *user_data)
{
    k_mutex_lock(&password_lock, K_FOREVER);
    if (password_pending && password_conn == conn &&
        (uintptr_t)user_data == (uintptr_t)password_ticket) {
        password_result = 0;
        password_pending = false;
        password_conn = NULL;
        k_sem_give(&password_reply_sent);
    }
    k_mutex_unlock(&password_lock);
}

static void password_reply_disconnected(struct bt_conn *conn, uint8_t reason)
{
    (void)reason;
    k_mutex_lock(&password_lock, K_FOREVER);
    if (password_pending && password_conn == conn) {
        password_result = -ENOTCONN;
        password_pending = false;
        password_conn = NULL;
        k_sem_give(&password_reply_sent);
    }
    k_mutex_unlock(&password_lock);
}

BT_CONN_CB_DEFINE(ruuvi_password_connections) = {
    .disconnected = password_reply_disconnected,
};

K_MSGQ_DEFINE(ruuvi_gatt_requests, sizeof(ruuvi_gatt_request_t),
              RUUVI_GATT_REQUEST_QUEUE_LEN, 1);

static ssize_t ruuvi_nus_rx_write(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                  const void *buf, uint16_t len, uint16_t offset, uint8_t flags);

#define RUUVI_RX_PROPERTIES (BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP)
/* Zephyr only calls the prepare callback when this bit is set; reject it there. */
#define RUUVI_RX_PERMISSIONS (BT_GATT_PERM_WRITE | BT_GATT_PERM_PREPARE_WRITE)

BT_GATT_SERVICE_DEFINE(ruuvi_nus,
    BT_GATT_PRIMARY_SERVICE(BT_UUID_DECLARE_128(
        BT_UUID_128_ENCODE(0x6e400001, 0xb5a3, 0xf393, 0xe0a9, 0xe50e24dcca9e))),
    BT_GATT_CHARACTERISTIC(BT_UUID_DECLARE_128(
                               BT_UUID_128_ENCODE(0x6e400002, 0xb5a3, 0xf393,
                                                  0xe0a9, 0xe50e24dcca9e)),
                           RUUVI_RX_PROPERTIES, RUUVI_RX_PERMISSIONS, NULL,
                           ruuvi_nus_rx_write, NULL),
    BT_GATT_CHARACTERISTIC(BT_UUID_DECLARE_128(
                               BT_UUID_128_ENCODE(0x6e400003, 0xb5a3, 0xf393,
                                                  0xe0a9, 0xe50e24dcca9e)),
                           BT_GATT_CHRC_NOTIFY, BT_GATT_PERM_NONE, NULL, NULL, NULL),
    BT_GATT_CCC(NULL, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE)
);

static ssize_t ruuvi_nus_rx_write(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                  const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
    ruuvi_gatt_request_t request;

    (void)attr;
    if ((flags & ~BT_GATT_WRITE_FLAG_CMD) != 0U) {
        return BT_GATT_ERR(BT_ATT_ERR_NOT_SUPPORTED);
    }
    if (offset != 0U) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }
    if (len < sizeof(request.data)) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }
    if (conn == NULL || !bt_gatt_is_subscribed(conn, &ruuvi_nus.attrs[4],
                                                BT_GATT_CCC_NOTIFY)) {
        return BT_GATT_ERR(BT_ATT_ERR_AUTHORIZATION);
    }

    request.conn = bt_conn_ref(conn);
    if (request.conn == NULL) {
        return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
    }
    memcpy(request.data, buf, sizeof(request.data));
    if (k_msgq_put(&ruuvi_gatt_requests, &request, K_NO_WAIT) != 0) {
        bt_conn_unref(request.conn);
        return BT_GATT_ERR(BT_ATT_ERR_INSUFFICIENT_RESOURCES);
    }

    return len;
}

int ruuvi_gatt_request_take(ruuvi_gatt_request_t *out, k_timeout_t timeout)
{
    if (out == NULL) {
        return -EINVAL;
    }
    return k_msgq_get(&ruuvi_gatt_requests, out, timeout);
}

struct k_msgq *ruuvi_gatt_request_queue(void)
{
    return &ruuvi_gatt_requests;
}

void ruuvi_gatt_request_release(ruuvi_gatt_request_t *req)
{
    if (req != NULL && req->conn != NULL) {
        bt_conn_unref(req->conn);
        req->conn = NULL;
    }
}

int ruuvi_gatt_command_send(struct bt_conn *conn, const uint8_t data[11])
{
    struct bt_conn_info info;
    int err;

    if (conn == NULL || data == NULL) {
        return -EINVAL;
    }
    if (bt_conn_get_info(conn, &info) != 0 || info.state != BT_CONN_STATE_CONNECTED) {
        return -ENOTCONN;
    }
    if (!bt_gatt_is_subscribed(conn, &ruuvi_nus.attrs[4], BT_GATT_CCC_NOTIFY)) {
        return -EACCES;
    }

    if (data[RE_STANDARD_SOURCE_INDEX] == RE_STANDARD_DESTINATION_PASSWORD &&
        (data[RE_STANDARD_OPERATION_INDEX] == RE_STANDARD_VALUE_WRITE ||
         data[RE_STANDARD_OPERATION_INDEX] == RE_STANDARD_OP_UNAUTHORIZED)) {
        k_mutex_lock(&password_lock, K_FOREVER);
        if (password_pending) {
            k_mutex_unlock(&password_lock);
            return -EAGAIN;
        }
        k_sem_reset(&password_reply_sent);
        ++password_ticket;
        if (password_ticket == 0U) {
            ++password_ticket;
        }
        uint32_t ticket = password_ticket;
        password_conn = conn; /* Caller holds a connection reference. */
        password_result = -ETIMEDOUT;
        password_pending = true;
        k_mutex_unlock(&password_lock);

        struct bt_gatt_notify_params params = {
            .attr = &ruuvi_nus.attrs[4],
            .data = data,
            .len = 11U,
            .func = password_reply_done,
            .user_data = (void *)(uintptr_t)ticket, /* Opaque cookie, never dereferenced. */
        };
        err = bt_gatt_notify_cb(conn, &params);
        int wait_rc = 0;
        if (err == 0) {
            /* Legacy waits for TX completion before dropping the old connection. */
            wait_rc = k_sem_take(&password_reply_sent, K_MSEC(4000));
        }
        k_mutex_lock(&password_lock, K_FOREVER);
        int result = err == 0 ? (wait_rc == 0 ? password_result : -ETIMEDOUT) :
                     (err == -ENOMEM ? -EAGAIN : err);
        if (password_ticket == ticket) {
            password_pending = false;
            password_conn = NULL;
        }
        k_mutex_unlock(&password_lock);
        return result;
    }

    err = bt_gatt_notify(conn, &ruuvi_nus.attrs[4], data, 11U);
    return (err == -ENOMEM) ? -EAGAIN : err;
}

int ruuvi_gatt_notify(const uint8_t *data, size_t length)
{
    if (data == NULL) {
        return -EINVAL;
    }

    if (length > RUUVI_GATT_DF5_LENGTH) {
        length = RUUVI_GATT_DF5_LENGTH;
    }

    /* Passing NULL targets only connected peers with notifications enabled. */
    int err = bt_gatt_notify(NULL, &ruuvi_nus.attrs[4], data, (uint16_t)length);

    return (err == -ENOTCONN) ? 0 : err;
}
