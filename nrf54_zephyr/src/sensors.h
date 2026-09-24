#ifndef RUUVI_SENSORS_H_
#define RUUVI_SENSORS_H_

#include "../../nrf52_oldsdk/src/ruuvi.endpoints.c/src/ruuvi_endpoint_5.h"
#include <stdint.h>

/* Optional board /aliases: env0 -> enabled Zephyr temperature/humidity/pressure
 * sensor (e.g. BME280 on I2C/SPI, SHTCX, TMP117); accel0 -> enabled
 * accelerometer (e.g. LIS2DH); battery0 -> enabled voltage sensor providing
 * SENSOR_CHAN_VOLTAGE in volts, sampled at most once per 60 seconds and
 * cached between heartbeats. The matching drivers must be enabled.
 * If environmental temperature is unavailable, the Nordic die temperature
 * driver supplies a fallback; die temperature is not ambient temperature.
 * Absent/unready devices and unsupported channels remain NAN. Other DF5 metadata
 * is owned by the caller. Returns the count of valid channels (axes count
 * separately), or negative errno for invalid input/unexpected driver errors.
 */
int ruuvi_sensor_read(re_5_data_t *sample);
/* Registers a real accelerometer motion trigger when accel0 supports one.
 * -ENOTSUP/-ENOSYS means this board cannot provide interrupt events.
 */
int ruuvi_sensor_motion_init(void);
uint32_t ruuvi_sensor_motion_count_get(void);

#endif /* RUUVI_SENSORS_H_ */
