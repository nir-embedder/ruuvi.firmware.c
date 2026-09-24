#ifndef RUUVI_HISTORY_H
#define RUUVI_HISTORY_H

#include "history_codec.h"

#include <stdint.h>

/* Mount and scan committed slots without clearing them. Calling init again
 * simulates a restart: unflushed samples and RAM-only configuration are lost.
 */
int ruuvi_history_init(void);

/* Accept a caller-timestamped sample when the interval has elapsed (including
 * the first sample). Return 1 if accepted, 0 if skipped, or negative errno.
 * Accepted samples are RAM-only until a full block writes successfully or
 * flush succeeds. If an automatic full-block write fails, a negative errno
 * is returned but that last sample remains in RAM for a flush retry.
 * Timestamps must not go backwards within one session or precede the most
 * recent committed block's end timestamp after a restart.
 */
int ruuvi_history_process(const ruuvi_history_element_t *sample);

/* Persist the pending partial block. On failure it remains pending for retry. */
int ruuvi_history_flush(void);

/* Chronological, zero-based sample index at or after min_timestamp_s. Records
 * are ordered by wrap-safe generation, then timestamp for equal generations;
 * the unflushed RAM block follows committed records. -ENOENT means end of log.
 * This is not a snapshot: restart traversal after a concurrent write/clear.
 */
int ruuvi_history_read(uint32_t min_timestamp_s, uint32_t index,
                       ruuvi_history_element_t *sample);
/* Last committed or pending timestamp, in caller-defined seconds. Returns
 * -ENOENT for an empty log; unlike a full read, this scans at most one slot.
 */
int ruuvi_history_latest_timestamp(uint32_t *timestamp_s);

/* Configuration is RAM-only; it resets to defaults on init/reboot. A change
 * flushes the previous block first and does not take effect if flush fails.
 * interval_s must be nonzero. overflow=false refuses to overwrite a slot.
 */
int ruuvi_history_config_set(const ruuvi_history_config_t *config);
int ruuvi_history_config_get(ruuvi_history_config_t *config);

/* Erase the partition and discard unflushed samples. A failed clear requires
 * init before further use; flash_clear is not power-fail atomic.
 */
int ruuvi_history_clear(void);

#endif
