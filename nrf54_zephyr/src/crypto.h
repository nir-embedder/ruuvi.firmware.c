#ifndef RUUVI_CRYPTO_H
#define RUUVI_CRYPTO_H

#include "formats.h"

/* One AES-128 ECB block. Returns 0 on success, EINVAL for invalid arguments,
 * or EIO for a PSA failure. Output is unchanged on failure. The caller owns
 * key provisioning; the temporary PSA key is destroyed before success.
 */
uint32_t ruuvi_aes_encrypt(const uint8_t *cleartext, uint8_t *ciphertext,
                           size_t data_size, const uint8_t *key, size_t key_size);

#endif
