#ifndef RUUVI_FORMATS_H
#define RUUVI_FORMATS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Values are the legacy app_dataformat_t enabled-mask bits, not wire headers. */
typedef enum {
    RUUVI_FORMAT_INVALID = 0U,
    RUUVI_FORMAT_3 = 1U << 0,
    RUUVI_FORMAT_5 = 1U << 1,
    RUUVI_FORMAT_7 = 1U << 2,
    RUUVI_FORMAT_8 = 1U << 3,
    RUUVI_FORMAT_C5 = 1U << 4,
    RUUVI_FORMAT_FA = 1U << 5,
} ruuvi_format_t;

#define RUUVI_FORMAT_ALL ((uint32_t)(RUUVI_FORMAT_3 | RUUVI_FORMAT_5 | \
                                     RUUVI_FORMAT_7 | RUUVI_FORMAT_8 | \
                                     RUUVI_FORMAT_C5 | RUUVI_FORMAT_FA))
#define RUUVI_FORMAT_MAX_LENGTH 24U

typedef struct {
    float humidity_rh;
    float pressure_pa;
    float temperature_c;
    float accelerationx_g;
    float accelerationy_g;
    float accelerationz_g;
    float battery_v;
    float luminosity_lux;
    float color_temp_k;
    uint16_t measurement_count;
    uint32_t movement_count; /* Legacy interrupt counter before format-specific modulo. */
    uint8_t motion_intensity;
    bool motion_detected;
    bool presence_detected;
    uint64_t address;  /* 48-bit BLE address, MSB first on the wire. */
    int8_t tx_power;
    uint64_t device_id; /* DF8 only: XORed into the first eight key bytes, LSB first. */
} ruuvi_measurement_t;

/* Must perform real AES-128 ECB encryption of exactly one 16-byte block.
 * Signature and zero-on-success convention match the sibling endpoint codecs.
 */
typedef uint32_t (*ruuvi_aes_encrypt_fn)(const uint8_t *cleartext,
                                         uint8_t *ciphertext,
                                         size_t data_size,
                                         const uint8_t *key,
                                         size_t key_size);

typedef struct {
    ruuvi_aes_encrypt_fn encrypt;
    const uint8_t *key; /* Caller-provisioned 16-byte key; never a built-in default. */
    size_t key_size;
} ruuvi_format_crypto_t;

/* Rotate 3 -> 5 -> 7 -> 8 -> C5 -> FA -> 3, skipping disabled bits.
 * INVALID starts at 3; an empty/unknown-only mask returns INVALID.
 * The caller owns selecting advertising UUIDs and advancing counters.
 */
ruuvi_format_t ruuvi_format_next(uint32_t enabled_mask, ruuvi_format_t current);

/* Encode a raw endpoint payload, without BLE manufacturer ID or AD headers.
 * length is input capacity and output payload length; unchanged on error.
 * The buffer is untouched on error. The caller supplies counters already
 * incremented (legacy starts at 1); each codec applies its legacy modulo.
 * Pass NAN for unavailable physical values. Crypto is only used by 8/FA:
 * no callback/key => -ENOTSUP; an invalid key size => -EINVAL.
 * DF8 derives its key by XORing device_id into the first eight supplied
 * key bytes, just as legacy ep_8_key_generate did. FA uses the key as-is.
 * A successful callback must really encrypt; this layer provides no AES or keys.
 */
int ruuvi_format_encode(ruuvi_format_t format, const ruuvi_measurement_t *measurement,
                        const ruuvi_format_crypto_t *crypto, uint8_t *output,
                        size_t *length);

#endif
