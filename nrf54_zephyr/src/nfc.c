#include "nfc.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <nfc_t4t_lib.h>
#include <hal/nrf_nfct.h>
#include <zephyr/devicetree.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(ruuvi_nfc, LOG_LEVEL_INF);

BUILD_ASSERT(IS_ENABLED(CONFIG_NFC_THREAD_CALLBACK) && IS_ENABLED(CONFIG_NFC_OWN_THREAD),
             "NFC field callbacks require a dedicated non-ISR thread");
BUILD_ASSERT(DT_NODE_HAS_STATUS(DT_NODELABEL(nfct), okay), "NFC requires an enabled NFCT");
#if DT_NODE_EXISTS(DT_NODELABEL(uicr))
BUILD_ASSERT(!DT_PROP_OR(DT_NODELABEL(uicr), nfct_pins_as_gpios, 0),
             "NFC cannot share NFCT pins with GPIO; check board UICR before deployment");
#endif

#define NFC_FILE_CAPACITY 512U
#define NFC_DATA_CAPACITY 48U

static uint8_t nfc_files[2][NFC_FILE_CAPACITY] __aligned(4);
static char id_text[33];
static char mac_text[24];
static char sw_text[48];
static uint8_t data_text[NFC_DATA_CAPACITY];
static size_t data_length;
static bool initialized;
static bool reply_waiting;
static atomic_t field_off_events;
static atomic_t field_callback_seen;
static atomic_t write_dirty;
static atomic_t reply_read;
static atomic_t active_file;
static atomic_t dropped_writes;
static K_SEM_DEFINE(nfc_events, 0, 1);
K_MSGQ_DEFINE(nfc_writes, NFC_FILE_CAPACITY, 2, 4);

static void nfc_callback(void *context, nfc_t4t_event_t event,
                         const uint8_t *data, size_t length, uint32_t flags)
{
    ARG_UNUSED(context);
    ARG_UNUSED(data);
    ARG_UNUSED(length);
    ARG_UNUSED(flags);

    switch (event) {
    case NFC_T4T_EVENT_FIELD_ON:
        atomic_set(&field_callback_seen, 1);
        k_sem_give(&nfc_events);
        break;
    case NFC_T4T_EVENT_NDEF_UPDATED:
        /* NLEN may be only partially written. Parse a snapshot after field-off. */
        atomic_set(&write_dirty, 1);
        break;
    case NFC_T4T_EVENT_NDEF_READ:
        atomic_set(&reply_read, 1);
        k_sem_give(&nfc_events);
        break;
    case NFC_T4T_EVENT_FIELD_OFF:
        if (atomic_set(&field_callback_seen, 0) != 0) {
            atomic_inc(&field_off_events);
        }
        if (atomic_set(&write_dirty, 0) != 0 &&
            k_msgq_put(&nfc_writes, nfc_files[atomic_get(&active_file)], K_NO_WAIT) != 0) {
            k_msgq_purge(&nfc_writes); /* Retain the latest reader write. */
            (void)k_msgq_put(&nfc_writes, nfc_files[atomic_get(&active_file)], K_NO_WAIT);
            atomic_inc(&dropped_writes);
        }
        k_sem_give(&nfc_events);
        break;
    default:
        break;
    }
}

int ruuvi_nfc_init(uint64_t address, const uint8_t device_id[8])
{
    static const uint8_t initial_data[] = "Data:"; /* Legacy includes the NUL. */
    int rc;
    size_t used;

    if (initialized) {
        return -EALREADY;
    }
    id_text[0] = '\0';
    if (device_id != NULL) {
        size_t pos = 0;
        pos += (size_t)snprintf(id_text, sizeof(id_text), "ID: ");
        for (size_t i = 0; i < 8U; ++i) {
            pos += (size_t)snprintf(id_text + pos, sizeof(id_text) - pos,
                                     i == 0U ? "%02X" : ":%02X", device_id[i]);
        }
    }
    (void)snprintf(mac_text, sizeof(mac_text), "MAC: %02X:%02X:%02X:%02X:%02X:%02X",
                   (unsigned int)((address >> 40U) & 0xffU),
                   (unsigned int)((address >> 32U) & 0xffU),
                   (unsigned int)((address >> 24U) & 0xffU),
                   (unsigned int)((address >> 16U) & 0xffU),
                   (unsigned int)((address >> 8U) & 0xffU), (unsigned int)(address & 0xffU));
#if defined(CONFIG_BT_DIS_FW_REV_STR) && defined(CONFIG_BT_DIS_MODEL_NUMBER_STR)
    (void)snprintf(sw_text, sizeof(sw_text), "SW: %s %s",
                   CONFIG_BT_DIS_MODEL_NUMBER_STR, CONFIG_BT_DIS_FW_REV_STR);
#else
    (void)snprintf(sw_text, sizeof(sw_text), "SW: Ruuvi nRF54 Zephyr");
#endif
    memcpy(data_text, initial_data, sizeof(initial_data));
    data_length = sizeof(initial_data);
    atomic_clear(&field_off_events);
    atomic_clear(&field_callback_seen);
    atomic_clear(&write_dirty);
    atomic_clear(&reply_read);
    atomic_clear(&active_file);
    atomic_clear(&dropped_writes);

    rc = nfc_t4t_setup(nfc_callback, NULL);
    if (rc != 0) {
        return rc;
    }
    rc = ruuvi_nfc_ndef_encode(nfc_files[0], NFC_FILE_CAPACITY,
                                id_text, mac_text, sw_text, data_text, data_length, &used);
    if (rc == 0) {
        rc = nfc_t4t_ndef_rwpayload_set(nfc_files[0], NFC_FILE_CAPACITY);
    }
    if (rc == 0) {
        rc = nfc_t4t_emulation_start();
    }
    if (rc != 0) {
        (void)nfc_t4t_done();
        return rc;
    }
    initialized = true;
    return 0;
}

struct k_sem *ruuvi_nfc_event_sem(void)
{
    return &nfc_events;
}

bool ruuvi_nfc_field_active(void)
{
    /* Read the live field; a delayed or suppressed callback must not latch it on. */
    return (nrf_nfct_field_status_get(NRF_NFCT) & NRF_NFCT_FIELD_STATE_PRESENT_MASK) != 0U;
}

bool ruuvi_nfc_field_off_take(void)
{
    return atomic_set(&field_off_events, 0) != 0;
}

bool ruuvi_nfc_reply_ready(void)
{
    if (!ruuvi_nfc_field_active() && reply_waiting && atomic_get(&reply_read) != 0) {
        reply_waiting = false;
        atomic_clear(&reply_read);
    }
    return !ruuvi_nfc_field_active() && !reply_waiting;
}

int ruuvi_nfc_request_take(uint8_t command[RUUVI_NFC_COMMAND_LENGTH])
{
    static uint8_t written_file[NFC_FILE_CAPACITY] __aligned(4);

    if (command == NULL) {
        return -EINVAL;
    }
    atomic_val_t dropped = atomic_set(&dropped_writes, 0);
    if (dropped != 0) {
        LOG_WRN("NFC write queue replaced %ld older request(s)", (long)dropped);
    }
    if (k_msgq_get(&nfc_writes, written_file, K_NO_WAIT) != 0) {
        return -ENOMSG;
    }
    /* A new reader write supersedes an unread response. */
    reply_waiting = false;
    atomic_clear(&reply_read);
    int decoded = ruuvi_nfc_ndef_decode(written_file, sizeof(written_file), command);
    int restored = ruuvi_nfc_update(data_text, data_length);
    if (restored != 0 && restored != -EAGAIN) {
        LOG_WRN("Restoring NDEF after reader write failed: %d", restored);
    }
    return decoded;
}

int ruuvi_nfc_update(const uint8_t *data, size_t length)
{
    size_t used;
    int rc;

    if (data == NULL || length == 0U || length >= NFC_DATA_CAPACITY) {
        return -EINVAL;
    }
    if (!initialized) {
        return -EACCES;
    }
    if (!ruuvi_nfc_reply_ready()) {
        return -EAGAIN;
    }
    unsigned int previous = (unsigned int)atomic_get(&active_file);
    unsigned int next = 1U - previous;
    rc = ruuvi_nfc_ndef_encode(nfc_files[next], NFC_FILE_CAPACITY, id_text, mac_text, sw_text,
                                data, length, &used);
    if (rc != 0) {
        return rc;
    }
    rc = nfc_t4t_emulation_stop(); /* Quiesce NFCT before swapping the live file. */
    if (rc != 0) {
        return rc;
    }
    rc = nfc_t4t_ndef_rwpayload_set(nfc_files[next], NFC_FILE_CAPACITY);
    if (rc != 0) {
        int restore = nfc_t4t_emulation_start(); /* Old file is still registered. */
        if (restore != 0) {
            initialized = false;
            LOG_ERR("NFC emulation restart failed: %d", restore);
        }
        return rc;
    }
    rc = nfc_t4t_emulation_start();
    if (rc != 0) {
        int restore = nfc_t4t_ndef_rwpayload_set(nfc_files[previous], NFC_FILE_CAPACITY);
        if (restore == 0) {
            restore = nfc_t4t_emulation_start();
        }
        if (restore != 0) {
            initialized = false;
            LOG_ERR("NFC emulation restart and rollback failed: %d", restore);
        }
        return rc;
    }
    atomic_set(&active_file, (atomic_val_t)next);
    if (data != data_text) {
        memcpy(data_text, data, length);
    }
    data_length = length;
    return 0;
}

int ruuvi_nfc_reply_send(void *unused, const uint8_t *data, size_t length)
{
    ARG_UNUSED(unused);
    if (length != RUUVI_NFC_COMMAND_LENGTH) {
        return -EINVAL;
    }
    int rc = ruuvi_nfc_update(data, length);
    if (rc == 0) {
        atomic_clear(&reply_read);
        reply_waiting = true;
    }
    return rc;
}
