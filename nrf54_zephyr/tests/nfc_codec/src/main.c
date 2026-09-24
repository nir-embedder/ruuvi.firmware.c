#include "nfc_codec.h"

#include <zephyr/ztest.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>

static uint8_t file[512];
static const uint8_t command[RUUVI_NFC_COMMAND_LENGTH] = {
    0x30, 0xa5, 0x11, 0x00, 0x00, 0x03, 0xe8, 0x00, 0x00, 0x03, 0x84,
};

ZTEST(ruuvi_nfc_codec, test_legacy_four_text_records_and_binary_request)
{
    static const uint8_t first_record[] = {
        0x81, 0x01, 0x00, 0x00, 0x00, 0x1e, 'T', 0x02, 'i', 'd',
        'I', 'D', ':', ' ', '0', '0', ':', '1', '1', ':', '2', '2', ':',
        '3', '3', ':', '4', '4', ':', '5', '5', ':', '6', '6', ':', '7', '7',
    };
    static const uint8_t last_header[] = {
        0x41, 0x01, 0x00, 0x00, 0x00, 0x0e, 'T', 0x02, 'd', 't',
    };
    uint8_t parsed[RUUVI_NFC_COMMAND_LENGTH] = {0};
    size_t used = 0;

    zassert_ok(ruuvi_nfc_ndef_encode(file, sizeof(file),
                                    "ID: 00:11:22:33:44:55:66:77",
                                    "MAC: AA:BB:CC:DD:EE:FF",
                                    "SW: Ruuvi FW v0.0.1+default",
                                    command, sizeof(command), &used));
    zassert_equal(file[0], (used - 2U) >> 8U);
    zassert_equal(file[1], (used - 2U) & 0xffU);
    zassert_mem_equal(file + 2U, first_record, sizeof(first_record));
    zassert_mem_equal(file + used - sizeof(command) - sizeof(last_header),
                      last_header, sizeof(last_header));
    zassert_mem_equal(file + used - sizeof(command), command, sizeof(command));
    zassert_ok(ruuvi_nfc_ndef_decode(file, sizeof(file), parsed));
    zassert_mem_equal(parsed, command, sizeof(command));
}

ZTEST(ruuvi_nfc_codec, test_single_dt_record_and_embedded_zero)
{
    static const uint8_t expected[] = {
        0x00, 0x15, 0xc1, 0x01, 0x00, 0x00, 0x00, 0x0e,
        'T', 0x02, 'd', 't',
        0x30, 0xa5, 0x11, 0x00, 0x00, 0x03, 0xe8, 0x00, 0x00, 0x03, 0x84,
    };
    uint8_t parsed[RUUVI_NFC_COMMAND_LENGTH] = {0};
    size_t used = 0;

    zassert_ok(ruuvi_nfc_ndef_encode(file, sizeof(file), "", "", "",
                                    command, sizeof(command), &used));
    zassert_equal(used, sizeof(expected));
    zassert_mem_equal(file, expected, sizeof(expected));
    zassert_ok(ruuvi_nfc_ndef_decode(file, used, parsed));
    zassert_mem_equal(parsed, command, sizeof(command));
}

ZTEST(ruuvi_nfc_codec, test_reject_malformed_or_non_command_records)
{
    uint8_t parsed[RUUVI_NFC_COMMAND_LENGTH];
    size_t used = 0;

    zassert_ok(ruuvi_nfc_ndef_encode(file, sizeof(file), "", "", "",
                                    command, sizeof(command), &used));
    file[0] = 0;
    file[1] = 0;
    zassert_equal(ruuvi_nfc_ndef_decode(file, sizeof(file), parsed), -EBADMSG);
    file[0] = 2;
    file[1] = 0;
    zassert_equal(ruuvi_nfc_ndef_decode(file, used, parsed), -EBADMSG);
    file[0] = 0;
    file[1] = used - 2U;
    zassert_equal(ruuvi_nfc_ndef_decode(file, used - 1U, parsed), -EBADMSG);
    file[10] = 'x'; /* Text-record language is no longer dt. */
    zassert_equal(ruuvi_nfc_ndef_decode(file, used, parsed), -ENOENT);
    file[10] = 'd';
    file[9] = 0x82; /* Status/language length is not UTF-8 / 2. */
    zassert_equal(ruuvi_nfc_ndef_decode(file, used, parsed), -EBADMSG);
    file[9] = 0x02;
    file[4] = 0;
    file[5] = 0;
    file[6] = 0;
    file[7] = 0x0d; /* Too short to contain the 11-byte command. */
    zassert_equal(ruuvi_nfc_ndef_decode(file, used, parsed), -EBADMSG);
}

ZTEST_SUITE(ruuvi_nfc_codec, NULL, NULL, NULL, NULL, NULL);
