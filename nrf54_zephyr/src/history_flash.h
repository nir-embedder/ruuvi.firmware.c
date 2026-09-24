#ifndef RUUVI_HISTORY_FLASH_H
#define RUUVI_HISTORY_FLASH_H

#include "history_codec.h"

#include <stdint.h>

/* Requires a dedicated, page-aligned ruuvi_history_partition of at least
 * 15 x 4096 bytes. Slots 0..13 contain 4020-byte payloads with a committed
 * 16-byte trailer at the end of each page; page 14 journals an overwritten
 * record. Call init before use. Only init's replay, write, and clear mutate
 * flash. Clear is not atomic across slots; a failed overwrite can leave the
 * previous committed record in the journal for reading/replay.
 */
int ruuvi_history_flash_init(void);
int ruuvi_history_flash_read(uint8_t slot, ruuvi_history_record_t *record,
                             uint32_t *generation);
int ruuvi_history_flash_write(uint8_t slot, const ruuvi_history_record_t *record,
                              uint32_t generation);
int ruuvi_history_flash_clear(void);

#endif
