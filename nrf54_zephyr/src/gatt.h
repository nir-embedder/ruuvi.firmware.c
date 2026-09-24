#ifndef RUUVI_GATT_H_
#define RUUVI_GATT_H_

#include <stddef.h>
#include <stdint.h>

#include <zephyr/kernel.h>

struct bt_conn;

/* Legacy RI_SCHEDULER_LENGTH is 10 events, shared across event types. */
#define RUUVI_GATT_REQUEST_QUEUE_LEN 10U

typedef struct {
    uint8_t data[11];
    struct bt_conn *conn;
} ruuvi_gatt_request_t;

/* On success, the caller owns out->conn until ruuvi_gatt_request_release().
 * Log reads need a history partition; password/unsupported-op replies do not.
 */
int ruuvi_gatt_request_take(ruuvi_gatt_request_t *out, k_timeout_t timeout);
/* Expose the queue for a combined GATT/NFC k_poll wait. */
struct k_msgq *ruuvi_gatt_request_queue(void);
void ruuvi_gatt_request_release(ruuvi_gatt_request_t *req);
/* Notify the subscribed peer with an 11-byte reply. Password replies wait
 * for the local TX-completion callback before the caller may change modes.
 */
int ruuvi_gatt_command_send(struct bt_conn *conn, const uint8_t data[11]);

/* Notify subscribed NUS TX peers with at most the first 18 DF5 bytes.
 * Returns 0 if no peer is subscribed, or a negative errno on failure.
 */
int ruuvi_gatt_notify(const uint8_t *data, size_t length);

#endif /* RUUVI_GATT_H_ */
