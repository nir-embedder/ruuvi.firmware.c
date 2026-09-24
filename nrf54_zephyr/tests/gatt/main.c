#include <errno.h>
#include <stdint.h>

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

ZTEST_SUITE(ruuvi_gatt, NULL, NULL, NULL, NULL, NULL);
