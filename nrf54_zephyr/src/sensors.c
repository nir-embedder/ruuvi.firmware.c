#include "sensors.h"

#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

#if DT_NODE_HAS_STATUS(DT_ALIAS(accel0), okay)
static atomic_t motion_events;
static const struct sensor_trigger motion_trigger = {
    .type = SENSOR_TRIG_MOTION,
    .chan = SENSOR_CHAN_ACCEL_XYZ,
};

static void on_motion(const struct device *dev, const struct sensor_trigger *trigger)
{
    ARG_UNUSED(dev);
    ARG_UNUSED(trigger);
    (void)atomic_inc(&motion_events);
}
#endif

int ruuvi_sensor_motion_init(void)
{
#if DT_NODE_HAS_STATUS(DT_ALIAS(accel0), okay)
    const struct device *accel = DEVICE_DT_GET(DT_ALIAS(accel0));
    /* Legacy 0.064 g threshold in Zephyr's m/s² sensor_value units. */
    const struct sensor_value threshold = { .val1 = 0, .val2 = 627626 };

    atomic_clear(&motion_events);
    if (!device_is_ready(accel)) {
        return -ENODEV;
    }
    int rc = sensor_trigger_set(accel, &motion_trigger, NULL);
    if (rc < 0) {
        return rc;
    }
    rc = sensor_attr_set(accel, SENSOR_CHAN_ACCEL_XYZ, SENSOR_ATTR_UPPER_THRESH,
                         &threshold);
    if (rc < 0) {
        return rc;
    }
    return sensor_trigger_set(accel, &motion_trigger, on_motion);
#else
    return -ENOTSUP;
#endif
}

uint32_t ruuvi_sensor_motion_count_get(void)
{
#if DT_NODE_HAS_STATUS(DT_ALIAS(accel0), okay)
    return (uint32_t)atomic_get(&motion_events);
#else
    return 0U;
#endif
}

#if DT_NODE_HAS_STATUS(DT_ALIAS(battery0), okay)
static re_float cached_battery_v;
static bool cached_battery_valid;
static int64_t battery_sample_due_ms;
#endif

#if DT_NODE_HAS_STATUS(DT_ALIAS(env0), okay) || \
    DT_NODE_HAS_STATUS(DT_ALIAS(temp0), okay) || \
    DT_NODE_HAS_STATUS(DT_ALIAS(humidity0), okay) || \
    DT_NODE_HAS_STATUS(DT_ALIAS(pressure0), okay)
static int read_env_channel(const struct device *dev, enum sensor_channel channel,
                            double scale, re_float *destination)
{
    struct sensor_value value;
    int rc = sensor_channel_get(dev, channel, &value);

    if (rc == -ENOTSUP || rc == -ENODATA || rc == -EINVAL) {
        return 0;
    }
    if (rc < 0) {
        return rc;
    }

    *destination = (re_float)(sensor_value_to_double(&value) * scale);
    return 1;
}
#if DT_NODE_HAS_STATUS(DT_ALIAS(temp0), okay) || \
    DT_NODE_HAS_STATUS(DT_ALIAS(humidity0), okay) || \
    DT_NODE_HAS_STATUS(DT_ALIAS(pressure0), okay)
static int read_single_channel(const struct device *dev, enum sensor_channel channel,
                               double scale, re_float *destination)
{
    if (!device_is_ready(dev)) {
        return 0;
    }
    int rc = sensor_sample_fetch(dev);
    if (rc == -ENOTSUP || rc == -ENODATA) {
        return 0;
    }
    if (rc < 0) {
        return rc;
    }
    return read_env_channel(dev, channel, scale, destination);
}
#endif
#endif

int ruuvi_sensor_read(re_5_data_t *sample)
{
    int count = 0;
    int first_error = 0;

    if (sample == NULL) {
        return -EINVAL;
    }

    sample->temperature_c = NAN;
    sample->humidity_rh = NAN;
    sample->pressure_pa = NAN;
    sample->accelerationx_g = NAN;
    sample->accelerationy_g = NAN;
    sample->accelerationz_g = NAN;
    sample->battery_v = NAN;

#if DT_NODE_HAS_STATUS(DT_ALIAS(env0), okay)
    const struct device *env = DEVICE_DT_GET(DT_ALIAS(env0));

    if (device_is_ready(env)) {
        int rc = sensor_sample_fetch(env);

        if (rc == 0) {
            rc = read_env_channel(env, SENSOR_CHAN_AMBIENT_TEMP, 1.0,
                                  &sample->temperature_c);
            if (rc < 0 && first_error == 0) {
                first_error = rc;
            } else if (rc > 0) {
                count += rc;
            }

            rc = read_env_channel(env, SENSOR_CHAN_HUMIDITY, 1.0,
                                   &sample->humidity_rh);
            if (rc < 0 && first_error == 0) {
                first_error = rc;
            } else if (rc > 0) {
                count += rc;
            }

            rc = read_env_channel(env, SENSOR_CHAN_PRESS, 1000.0,
                                   &sample->pressure_pa);
            if (rc < 0 && first_error == 0) {
                first_error = rc;
            } else if (rc > 0) {
                count += rc;
            }
        } else if (rc != -ENOTSUP && rc != -ENODATA && first_error == 0) {
            first_error = rc;
        }
    }
#endif

#if DT_NODE_HAS_STATUS(DT_ALIAS(temp0), okay)
    {
        bool had_temperature = !isnan(sample->temperature_c);
        int rc = read_single_channel(DEVICE_DT_GET(DT_ALIAS(temp0)),
                                     SENSOR_CHAN_AMBIENT_TEMP, 1.0,
                                     &sample->temperature_c);
        if (rc < 0 && first_error == 0) {
            first_error = rc;
        } else if (rc > 0 && !had_temperature) {
            count++;
        }
    }
#endif
#if DT_NODE_HAS_STATUS(DT_ALIAS(humidity0), okay)
    {
        bool had_humidity = !isnan(sample->humidity_rh);
        int rc = read_single_channel(DEVICE_DT_GET(DT_ALIAS(humidity0)),
                                     SENSOR_CHAN_HUMIDITY, 1.0,
                                     &sample->humidity_rh);
        if (rc < 0 && first_error == 0) {
            first_error = rc;
        } else if (rc > 0 && !had_humidity) {
            count++;
        }
    }
#endif
#if DT_NODE_HAS_STATUS(DT_ALIAS(pressure0), okay)
    {
        bool had_pressure = !isnan(sample->pressure_pa);
        int rc = read_single_channel(DEVICE_DT_GET(DT_ALIAS(pressure0)),
                                     SENSOR_CHAN_PRESS, 1000.0,
                                     &sample->pressure_pa);
        if (rc < 0 && first_error == 0) {
            first_error = rc;
        } else if (rc > 0 && !had_pressure) {
            count++;
        }
    }
#endif

#if DT_HAS_COMPAT_STATUS_OKAY(nordic_nrf_temp)
    if (isnan(sample->temperature_c)) {
        const struct device *die = DEVICE_DT_GET_ANY(nordic_nrf_temp);

        if (device_is_ready(die)) {
            int rc = sensor_sample_fetch(die);

            if (rc == 0) {
                struct sensor_value value;

                rc = sensor_channel_get(die, SENSOR_CHAN_DIE_TEMP, &value);
                if (rc == 0) {
                    sample->temperature_c = (re_float)sensor_value_to_double(&value);
                    count++;
                } else if (rc != -ENOTSUP && rc != -ENODATA && rc != -EINVAL &&
                           first_error == 0) {
                    first_error = rc;
                }
            } else if (rc != -ENOTSUP && rc != -ENODATA && first_error == 0) {
                first_error = rc;
            }
        }
    }
#endif

#if DT_NODE_HAS_STATUS(DT_ALIAS(accel0), okay)
    const struct device *accel = DEVICE_DT_GET(DT_ALIAS(accel0));

    if (device_is_ready(accel)) {
        struct sensor_value axes[3];
        int rc = sensor_sample_fetch(accel);

        if (rc == 0) {
            rc = sensor_channel_get(accel, SENSOR_CHAN_ACCEL_XYZ, axes);
            if (rc == 0) {
                sample->accelerationx_g = (re_float)(sensor_value_to_double(&axes[0]) /
                                                      9.80665);
                sample->accelerationy_g = (re_float)(sensor_value_to_double(&axes[1]) /
                                                      9.80665);
                sample->accelerationz_g = (re_float)(sensor_value_to_double(&axes[2]) /
                                                      9.80665);
                count += 3;
            } else if (rc != -ENOTSUP && rc != -ENODATA && rc != -EINVAL &&
                       first_error == 0) {
                first_error = rc;
            }
        } else if (rc != -ENOTSUP && rc != -ENODATA && first_error == 0) {
            first_error = rc;
        }
    }
#endif

#if DT_NODE_HAS_STATUS(DT_ALIAS(battery0), okay)
    const struct device *battery = DEVICE_DT_GET(DT_ALIAS(battery0));

    if (device_is_ready(battery)) {
        int64_t now_ms = k_uptime_get();
        if (now_ms >= battery_sample_due_ms) {
            int rc = sensor_sample_fetch(battery);
            if (rc == 0) {
                struct sensor_value voltage;
                rc = sensor_channel_get(battery, SENSOR_CHAN_VOLTAGE, &voltage);
                if (rc == 0) {
                    cached_battery_v = (re_float)sensor_value_to_double(&voltage);
                    cached_battery_valid = true;
                    battery_sample_due_ms = now_ms + 60000;
                }
            }
            if (rc == -ENOTSUP || rc == -ENODATA || rc == -EINVAL) {
                cached_battery_valid = false;
                battery_sample_due_ms = now_ms + 60000;
            } else if (rc < 0 && first_error == 0) {
                first_error = rc;
            }
        }
        if (cached_battery_valid) {
            sample->battery_v = cached_battery_v;
            count++;
        }
    }
#endif

    return count > 0 ? count : first_error;
}
