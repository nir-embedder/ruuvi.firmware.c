#ifndef RUUVI_HISTORY_SETTINGS_H
#define RUUVI_HISTORY_SETTINGS_H

#include "history_codec.h"

/* Mount NVS across storage_partition. Other clients of this partition must
 * use the same NVS geometry; independent mounts with different sector layouts
 * cannot safely share it. Returns -ENOTSUP if NVS or the partition is absent.
 */
int ruuvi_history_settings_init(void);

/* Leave *config unchanged on error. -ENOENT means no saved configuration;
 * the caller owns initialization of its defaults. Invalid stored data is
 * reported as -EBADMSG rather than treated as an absent configuration.
 */
int ruuvi_history_settings_load(ruuvi_history_config_t *config);
int ruuvi_history_settings_store(const ruuvi_history_config_t *config);

#endif
