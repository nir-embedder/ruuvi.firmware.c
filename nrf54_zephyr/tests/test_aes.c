/* Public NIST AES-128 ECB known-answer vector; no production key material. */
#include "crypto.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>

int main(void)
{
    const uint8_t key[16] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
    };
    const uint8_t plaintext[16] = {
        0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
        0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff,
    };
    const uint8_t expected[16] = {
        0x69, 0xc4, 0xe0, 0xd8, 0x6a, 0x7b, 0x04, 0x30,
        0xd8, 0xcd, 0xb7, 0x80, 0x70, 0xb4, 0xc5, 0x5a,
    };
    uint8_t output[16] = {0};

    assert(ruuvi_aes_encrypt(plaintext, output, sizeof(plaintext),
                             key, sizeof(key)) == 0);
    assert(memcmp(output, expected, sizeof(expected)) == 0);
    memset(output, 0xa5, sizeof(output));
    assert(ruuvi_aes_encrypt(NULL, output, sizeof(plaintext),
                             key, sizeof(key)) == EINVAL);
    assert(ruuvi_aes_encrypt(plaintext, output, sizeof(plaintext) - 1,
                             key, sizeof(key)) == EINVAL);
    assert(ruuvi_aes_encrypt(plaintext, output, sizeof(plaintext),
                             key, sizeof(key) - 1) == EINVAL);
    for (size_t i = 0; i < sizeof(output); ++i) {
        assert(output[i] == 0xa5);
    }
    return 0;
}
