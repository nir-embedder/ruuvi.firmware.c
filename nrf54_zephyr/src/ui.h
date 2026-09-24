#ifndef RUUVI_UI_H_
#define RUUVI_UI_H_

#include <stdbool.h>

/* Missing sw0/led0/led1/led2 aliases are supported. No storage or boot action is performed. */
int ruuvi_ui_init(void);
void ruuvi_ui_activity(bool on);
void ruuvi_ui_error(bool on);
/* After the first accepted heartbeat, clear boot error and signal success for 1 s. */
void ruuvi_ui_startup_success(void);
/* When led2 exists, activity moves from led0 to led2 in configuration mode. */
void ruuvi_ui_configuration(bool on);
/* Set on short release until claimed, expired after 60 seconds or cancelled by recovery. */
bool ruuvi_ui_config_pending(void);
/* Claim the next-connection request once from either the BLE callback or main thread. */
bool ruuvi_ui_config_claim(void);
/* Latched until reboot; the caller must decide how to handle recovery safely. */
bool ruuvi_ui_recovery_requested(void);

#endif /* RUUVI_UI_H_ */
