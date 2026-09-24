#ifndef RUUVI_NFC_CODEC_H_
#define RUUVI_NFC_CODEC_H_

#include <stddef.h>
#include <stdint.h>

#define RUUVI_NFC_COMMAND_LENGTH 11U

/* Encode the legacy id/ad/sw/dt Text records into a writable Type 4 NDEF
 * file (two-byte NLEN followed by the NDEF message). Empty metadata strings
 * are omitted. The data record preserves its binary length, including NULs.
 */
int ruuvi_nfc_ndef_encode(uint8_t *file, size_t capacity,
                           const char *id, const char *mac, const char *sw,
                           const uint8_t *data, size_t data_len, size_t *used);

/* Extract exactly one 11-byte command from a well-formed dt Text record.
 * Validate the NLEN and parser's consumed length before using any descriptor.
 */
int ruuvi_nfc_ndef_decode(const uint8_t *file, size_t capacity,
                           uint8_t command[RUUVI_NFC_COMMAND_LENGTH]);

#endif
