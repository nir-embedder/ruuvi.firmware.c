#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include "crypto.h"

ZTEST(ruuvi_aes, test_nist_aes128_ecb_block)
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
    uint8_t ciphertext[16] = {0};

    zassert_equal(ruuvi_aes_encrypt(plaintext, ciphertext,
                                    sizeof(plaintext), key, sizeof(key)), 0);
    if (memcmp(ciphertext, expected, sizeof(expected)) != 0) {
        printk("AES result:");
        for (size_t i = 0; i < sizeof(ciphertext); ++i) {
            printk(" %02x", ciphertext[i]);
        }
        printk("\n");
    }
    zassert_mem_equal(ciphertext, expected, sizeof(expected));

    memset(ciphertext, 0xa5, sizeof(ciphertext));
    zassert_equal(ruuvi_aes_encrypt(NULL, ciphertext,
                                    sizeof(plaintext), key, sizeof(key)), EINVAL);
    zassert_equal(ruuvi_aes_encrypt(plaintext, ciphertext,
                                    sizeof(plaintext) - 1, key, sizeof(key)), EINVAL);
    zassert_equal(ruuvi_aes_encrypt(plaintext, ciphertext,
                                    sizeof(plaintext), key, sizeof(key) - 1), EINVAL);
    for (size_t i = 0; i < sizeof(ciphertext); ++i) {
        zassert_equal(ciphertext[i], 0xa5);
    }
}

ZTEST_SUITE(ruuvi_aes, NULL, NULL, NULL, NULL, NULL);
