#ifndef RUUVI_UI_H_
#define RUUVI_UI_H_

#include <stdbool.h>

/* Missing sw0/led0/led1 aliases are supported. No storage or boot action is performed. */
int ruuvi_ui_init(void);
void ruuvi_ui_activity(bool on);
void ruuvi_ui_error(bool on);
/* Cleared 60 seconds after the last short release, or on a recovery request. */
bool ruuvi_ui_config_pending(void);
/* Latched until reboot; the caller must decide how to handle recovery safely. */
bool ruuvi_ui_recovery_requested(void);

#endif /* RUUVI_UI_H_ */
