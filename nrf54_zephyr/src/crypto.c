#include "crypto.h"

#include <errno.h>
#include <string.h>

#include <psa/crypto.h>

uint32_t ruuvi_aes_encrypt(const uint8_t *cleartext, uint8_t *ciphertext,
                           size_t data_size, const uint8_t *key, size_t key_size)
{
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_key_id_t key_id = PSA_KEY_ID_NULL;
    psa_status_t status;
    psa_status_t destroy_status;
    uint8_t block[16] = {0};
    volatile uint8_t *const wipe = block;
    size_t output_size = 0;
    uint32_t result = (uint32_t)EIO;

    if (cleartext == NULL || ciphertext == NULL || key == NULL ||
        data_size != sizeof(block) || key_size != sizeof(block)) {
        return (uint32_t)EINVAL;
    }

    status = psa_crypto_init();
    if (status != PSA_SUCCESS) {
        goto done;
    }

    psa_set_key_type(&attributes, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attributes, 128);
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_ENCRYPT);
    psa_set_key_algorithm(&attributes, PSA_ALG_ECB_NO_PADDING);

    status = psa_import_key(&attributes, key, key_size, &key_id);
    if (status != PSA_SUCCESS) {
        goto done;
    }

    status = psa_cipher_encrypt(key_id, PSA_ALG_ECB_NO_PADDING,
                                cleartext, data_size, block, sizeof(block),
                                &output_size);
    destroy_status = psa_destroy_key(key_id);
    if (status == PSA_SUCCESS && destroy_status == PSA_SUCCESS &&
        output_size == sizeof(block)) {
        memcpy(ciphertext, block, sizeof(block));
        result = 0U;
    }

done:
    psa_reset_key_attributes(&attributes);
    for (size_t i = 0; i < sizeof(block); ++i) {
        wipe[i] = 0;
    }
    return result;
}
