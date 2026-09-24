#ifndef RUUVI_NFC_H_
#define RUUVI_NFC_H_

#include "nfc_codec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <zephyr/kernel.h>

/* NCS Type 4 read-write tag only; the caller owns BLE field arbitration. */
int ruuvi_nfc_init(uint64_t address, const uint8_t device_id[8]);
struct k_sem *ruuvi_nfc_event_sem(void);
bool ruuvi_nfc_field_active(void);
/* Latches a completed field transition even when a tap ends before main runs. */
bool ruuvi_nfc_field_off_take(void);
/* A reply remains on the tag until a reader reads it and leaves the field. */
bool ruuvi_nfc_reply_ready(void);
/* Retrieve one complete NFC-written file's 11-byte dt command after field-off. */
int ruuvi_nfc_request_take(uint8_t command[RUUVI_NFC_COMMAND_LENGTH]);
/* Only updates while the field is absent and no unread reply is pending. */
int ruuvi_nfc_update(const uint8_t *data, size_t length);
int ruuvi_nfc_reply_send(void *unused, const uint8_t *data, size_t length);

#endif
