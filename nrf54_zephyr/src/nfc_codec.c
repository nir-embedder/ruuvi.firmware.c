#include "nfc_codec.h"

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <string.h>

#define LEGACY_NFC_TEXT_MAX 48U
#define NDEF_FILE_HEADER 2U
#define TEXT_RECORD_HEADER 10U /* Long record + T + UTF-8 status + two-byte language. */

typedef struct {
    uint8_t language[2];
    const uint8_t *bytes;
    size_t length;
} text_field_t;

static size_t text_length(const char *text)
{
    size_t length = 0;
    while (length < LEGACY_NFC_TEXT_MAX && text[length] != '\0') {
        ++length;
    }
    return length;
}

static int append_text(uint8_t *file, size_t capacity, size_t *offset,
                       const text_field_t *field, bool first, bool last)
{
    if (field->length > capacity - *offset ||
        TEXT_RECORD_HEADER > capacity - *offset - field->length) {
        return -ENOSPC;
    }
    size_t pos = *offset;
    uint32_t payload_length = (uint32_t)(field->length + 3U);
    file[pos++] = 0x01U | (first ? 0x80U : 0U) | (last ? 0x40U : 0U);
    file[pos++] = 1U; /* Well-known NDEF Text type T. */
    file[pos++] = (uint8_t)(payload_length >> 24);
    file[pos++] = (uint8_t)(payload_length >> 16);
    file[pos++] = (uint8_t)(payload_length >> 8);
    file[pos++] = (uint8_t)payload_length;
    file[pos++] = 'T';
    file[pos++] = 0x02U; /* UTF-8 and a two-byte language code. */
    file[pos++] = field->language[0];
    file[pos++] = field->language[1];
    memcpy(file + pos, field->bytes, field->length);
    *offset = pos + field->length;
    return 0;
}

int ruuvi_nfc_ndef_encode(uint8_t *file, size_t capacity,
                           const char *id, const char *mac, const char *sw,
                           const uint8_t *data, size_t data_len, size_t *used)
{
    if (file == NULL || used == NULL || id == NULL || mac == NULL || sw == NULL ||
        data == NULL || data_len == 0U || data_len >= LEGACY_NFC_TEXT_MAX ||
        capacity < NDEF_FILE_HEADER || capacity - NDEF_FILE_HEADER > UINT16_MAX) {
        return -EINVAL;
    }
    size_t id_len = text_length(id);
    size_t mac_len = text_length(mac);
    size_t sw_len = text_length(sw);
    if (id_len == LEGACY_NFC_TEXT_MAX || mac_len == LEGACY_NFC_TEXT_MAX ||
        sw_len == LEGACY_NFC_TEXT_MAX) {
        return -EINVAL;
    }
    text_field_t fields[4];
    size_t count = 0;
    if (id_len != 0U) {
        fields[count++] = (text_field_t) {{'i', 'd'}, (const uint8_t *)id, id_len};
    }
    if (mac_len != 0U) {
        fields[count++] = (text_field_t) {{'a', 'd'}, (const uint8_t *)mac, mac_len};
    }
    if (sw_len != 0U) {
        fields[count++] = (text_field_t) {{'s', 'w'}, (const uint8_t *)sw, sw_len};
    }
    fields[count++] = (text_field_t) {{'d', 't'}, data, data_len};

    memset(file, 0, capacity);
    size_t pos = NDEF_FILE_HEADER;
    for (size_t i = 0; i < count; ++i) {
        int rc = append_text(file, capacity, &pos, &fields[i], i == 0U, i == count - 1U);
        if (rc != 0) {
            return rc;
        }
    }
    size_t ndef_len = pos - NDEF_FILE_HEADER;
    file[0] = (uint8_t)(ndef_len >> 8);
    file[1] = (uint8_t)ndef_len;
    *used = pos;
    return 0;
}

int ruuvi_nfc_ndef_decode(const uint8_t *file, size_t capacity,
                           uint8_t command[RUUVI_NFC_COMMAND_LENGTH])
{
    if (file == NULL || command == NULL || capacity < NDEF_FILE_HEADER) {
        return -EINVAL;
    }
    size_t ndef_len = ((size_t)file[0] << 8) | file[1];
    if (ndef_len == 0U || ndef_len > capacity - NDEF_FILE_HEADER) {
        return -EBADMSG;
    }

    size_t pos = NDEF_FILE_HEADER;
    const size_t end = pos + ndef_len;
    unsigned int count = 0;
    bool found = false;
    while (pos < end) {
        ++count;
        if (count > 4U || end - pos < 3U) {
            return -EBADMSG;
        }
        uint8_t header = file[pos++];
        size_t type_length = file[pos++];
        if ((header & 0x2fU) != 0x01U || type_length != 1U ||
            ((header & 0x80U) != 0U) != (count == 1U)) {
            return -EBADMSG; /* No chunks, ID field or non-Text TNF. */
        }
        size_t payload_length;
        if ((header & 0x10U) != 0U) {
            payload_length = file[pos++];
        } else {
            if (end - pos < 4U) {
                return -EBADMSG;
            }
            payload_length = ((size_t)file[pos] << 24) | ((size_t)file[pos + 1U] << 16) |
                             ((size_t)file[pos + 2U] << 8) | file[pos + 3U];
            pos += 4U;
        }
        if (end - pos < 1U || file[pos++] != 'T' || payload_length > end - pos ||
            payload_length < 3U || file[pos] != 0x02U) {
            return -EBADMSG;
        }
        if (file[pos + 1U] == 'd' && file[pos + 2U] == 't') {
            if (found || payload_length != 3U + RUUVI_NFC_COMMAND_LENGTH) {
                return -EBADMSG;
            }
            memcpy(command, file + pos + 3U, RUUVI_NFC_COMMAND_LENGTH);
            found = true;
        }
        pos += payload_length;
        if ((header & 0x40U) != 0U) {
            if (pos != end) {
                return -EBADMSG;
            }
        } else if (pos == end) {
            return -EBADMSG;
        }
    }
    return found ? 0 : -ENOENT;
}
