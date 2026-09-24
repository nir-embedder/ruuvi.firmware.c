#include "../src/nfc_codec.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const uint8_t request[RUUVI_NFC_COMMAND_LENGTH] = {
    0x30, 0xa5, 0x11, 0x00, 0x00, 0x03, 0xe8, 0x00, 0x00, 0x03, 0x84,
};

static void test_unaligned_and_boundary(void)
{
    uint8_t storage[513];
    uint8_t decoded[RUUVI_NFC_COMMAND_LENGTH] = {0};
    uint8_t data[48] = {0};
    size_t used = 0;

    /* The vendor v3.4.1 long-record encoder uses unaligned uint32_t stores;
     * this byte-oriented codec supports any buffer alignment instead.
     */
    assert(ruuvi_nfc_ndef_encode(storage + 1, 512, "", "", "",
                                  request, sizeof(request), &used) == 0);
    assert(used == 23U);
    assert(ruuvi_nfc_ndef_decode(storage + 1, used, decoded) == 0);
    assert(memcmp(decoded, request, sizeof(request)) == 0);
    assert(ruuvi_nfc_ndef_encode(storage + 1, used - 1U, "", "", "",
                                  request, sizeof(request), &used) == -ENOSPC);
    assert(ruuvi_nfc_ndef_encode(storage + 1, 512, "", "", "",
                                  data, 0, &used) == -EINVAL);
    assert(ruuvi_nfc_ndef_encode(storage + 1, 512, "", "", "",
                                  data, 47, &used) == 0);
    assert(ruuvi_nfc_ndef_encode(storage + 1, 512, "", "", "",
                                  data, 48, &used) == -EINVAL);
}

static void test_short_records_and_bad_flags(void)
{
    uint8_t short_record[] = {
        0x00, 0x12, 0xd1, 0x01, 0x0e, 'T', 0x02, 'd', 't',
        0x30, 0xa5, 0x11, 0x00, 0x00, 0x03, 0xe8, 0x00, 0x00, 0x03, 0x84,
    };
    uint8_t decoded[RUUVI_NFC_COMMAND_LENGTH];

    assert(ruuvi_nfc_ndef_decode(short_record, sizeof(short_record), decoded) == 0);
    assert(memcmp(decoded, request, sizeof(request)) == 0);
    short_record[2] |= 0x20U; /* Chunked NDEF is not supported. */
    assert(ruuvi_nfc_ndef_decode(short_record, sizeof(short_record), decoded) == -EBADMSG);
    short_record[2] = 0xd9; /* IL flag is unsupported. */
    assert(ruuvi_nfc_ndef_decode(short_record, sizeof(short_record), decoded) == -EBADMSG);
    short_record[2] = 0xd7; /* Reserved TNF. */
    assert(ruuvi_nfc_ndef_decode(short_record, sizeof(short_record), decoded) == -EBADMSG);
    short_record[2] = 0x51; /* Missing message-begin flag. */
    assert(ruuvi_nfc_ndef_decode(short_record, sizeof(short_record), decoded) == -EBADMSG);
    short_record[2] = 0xd1;
    short_record[7] = 'x'; /* A valid Text record, but not a dt command. */
    assert(ruuvi_nfc_ndef_decode(short_record, sizeof(short_record), decoded) == -ENOENT);
    short_record[7] = 'd';
    short_record[1] = 0xff; /* NLEN extends beyond the provided buffer. */
    assert(ruuvi_nfc_ndef_decode(short_record, sizeof(short_record), decoded) == -EBADMSG);
}

static void test_legacy_startup_file(void)
{
    static const uint8_t expected[] = {
        0x00, 0x7a,
        0x81, 0x01, 0x00, 0x00, 0x00, 0x1e, 'T', 0x02, 'i', 'd',
        'I', 'D', ':', ' ', '0', '0', ':', '1', '1', ':', '2', '2', ':',
        '3', '3', ':', '4', '4', ':', '5', '5', ':', '6', '6', ':', '7', '7',
        0x01, 0x01, 0x00, 0x00, 0x00, 0x19, 'T', 0x02, 'a', 'd',
        'M', 'A', 'C', ':', ' ', 'A', 'A', ':', 'B', 'B', ':', 'C', 'C', ':',
        'D', 'D', ':', 'E', 'E', ':', 'F', 'F',
        0x01, 0x01, 0x00, 0x00, 0x00, 0x1e, 'T', 0x02, 's', 'w',
        'S', 'W', ':', ' ', 'R', 'u', 'u', 'v', 'i', ' ', 'F', 'W', ' ',
        'v', '0', '.', '0', '.', '1', '+', 'd', 'e', 'f', 'a', 'u', 'l', 't',
        0x41, 0x01, 0x00, 0x00, 0x00, 0x09, 'T', 0x02, 'd', 't',
        'D', 'a', 't', 'a', ':', 0x00,
    };
    uint8_t file[256];
    size_t used = 0;
    static const uint8_t data[] = "Data:";

    assert(ruuvi_nfc_ndef_encode(file, sizeof(file),
                                  "ID: 00:11:22:33:44:55:66:77",
                                  "MAC: AA:BB:CC:DD:EE:FF",
                                  "SW: Ruuvi FW v0.0.1+default",
                                  data, sizeof(data), &used) == 0);
    assert(used == sizeof(expected));
    for (size_t i = 0; i < sizeof(expected); ++i) {
        if (file[i] != expected[i]) {
            fprintf(stderr, "NDEF byte %zu: %02x != %02x\n", i, file[i], expected[i]);
            assert(file[i] == expected[i]);
        }
    }
}

static void test_duplicate_command_rejected(void)
{
    uint8_t file[64];
    uint8_t duplicate[64] = {0};
    uint8_t decoded[RUUVI_NFC_COMMAND_LENGTH];
    size_t used = 0;

    assert(ruuvi_nfc_ndef_encode(file, sizeof(file), "", "", "",
                                  request, sizeof(request), &used) == 0);
    assert(used == 23U);
    duplicate[0] = 0;
    duplicate[1] = 42; /* Two 21-byte records. */
    memcpy(duplicate + 2, file + 2, 21);
    memcpy(duplicate + 23, file + 2, 21);
    duplicate[2] = 0x81;  /* First, not last. */
    duplicate[23] = 0x41; /* Last, not first. */
    assert(ruuvi_nfc_ndef_decode(duplicate, 44, decoded) == -EBADMSG);
}

int main(void)
{
    test_unaligned_and_boundary();
    test_short_records_and_bad_flags();
    test_legacy_startup_file();
    test_duplicate_command_rejected();
    return 0;
}
