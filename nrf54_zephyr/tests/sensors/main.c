#include <errno.h>
#include <stdbool.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include "sensors.h"

enum fake_role { ACCEL, ENVIRONMENT, TEMPERATURE, HUMIDITY, PRESSURE };
static const enum fake_role accel_role = ACCEL;
static const enum fake_role environment_role = ENVIRONMENT;
static const enum fake_role temperature_role = TEMPERATURE;
static const enum fake_role humidity_role = HUMIDITY;
static const enum fake_role pressure_role = PRESSURE;
static bool environment_available = true;
static bool temperature_available = true;
static bool humidity_available = true;
static bool pressure_available = true;
static bool supports_motion;
static bool fail_threshold;
static int probe_calls;
static int threshold_calls;
static int enable_calls;
static struct sensor_value captured_threshold;
static sensor_trigger_handler_t captured_handler;
static const struct sensor_trigger *captured_trigger;

static int fake_trigger_set(const struct device *dev, const struct sensor_trigger *trigger,
                            sensor_trigger_handler_t handler)
{
    if (*(const enum fake_role *)dev->config != ACCEL || !supports_motion) {
        return -ENOTSUP;
    }
    captured_trigger = trigger;
    captured_handler = handler;
    if (handler == NULL) {
        ++probe_calls;
    } else {
        ++enable_calls;
    }
    return 0;
}

static int fake_attr_set(const struct device *dev, enum sensor_channel channel,
                         enum sensor_attribute attribute, const struct sensor_value *value)
{
    if (*(const enum fake_role *)dev->config != ACCEL ||
        channel != SENSOR_CHAN_ACCEL_XYZ || attribute != SENSOR_ATTR_UPPER_THRESH) {
        return -ENOTSUP;
    }
    ++threshold_calls;
    captured_threshold = *value;
    return fail_threshold ? -EIO : 0;
}

static int fake_sample_fetch(const struct device *dev, enum sensor_channel channel)
{
    ARG_UNUSED(channel);
    return (*(const enum fake_role *)dev->config == ENVIRONMENT &&
            !environment_available) ? -ENODATA : 0;
}

static int fake_channel_get(const struct device *dev, enum sensor_channel channel,
                            struct sensor_value *values)
{
    switch (*(const enum fake_role *)dev->config) {
    case ACCEL:
        if (channel == SENSOR_CHAN_ACCEL_XYZ) {
            values[0] = (struct sensor_value) {0, 0};
            values[1] = (struct sensor_value) {0, 0};
            values[2] = (struct sensor_value) {9, 806650};
            return 0;
        }
        break;
    case ENVIRONMENT:
        if (channel == SENSOR_CHAN_AMBIENT_TEMP) {
            *values = (struct sensor_value) {10, 500000};
            return 0;
        }
        if (channel == SENSOR_CHAN_HUMIDITY) {
            *values = (struct sensor_value) {33, 0};
            return 0;
        }
        if (channel == SENSOR_CHAN_PRESS) {
            *values = (struct sensor_value) {101, 200000};
            return 0;
        }
        break;
    case TEMPERATURE:
        if (channel == SENSOR_CHAN_AMBIENT_TEMP && temperature_available) {
            *values = (struct sensor_value) {22, 250000};
            return 0;
        }
        break;
    case HUMIDITY:
        if (channel == SENSOR_CHAN_HUMIDITY && humidity_available) {
            *values = (struct sensor_value) {55, 500000};
            return 0;
        }
        break;
    case PRESSURE:
        if (channel == SENSOR_CHAN_PRESS && pressure_available) {
            *values = (struct sensor_value) {100, 125000};
            return 0;
        }
        break;
    }
    return -ENODATA;
}

static const struct sensor_driver_api fake_api = {
    .trigger_set = fake_trigger_set,
    .attr_set = fake_attr_set,
    .sample_fetch = fake_sample_fetch,
    .channel_get = fake_channel_get,
};
DEVICE_DT_DEFINE(DT_NODELABEL(motion_test), NULL, NULL, NULL, &accel_role,
                 POST_KERNEL, CONFIG_SENSOR_INIT_PRIORITY, &fake_api);
DEVICE_DT_DEFINE(DT_NODELABEL(environment_test), NULL, NULL, NULL, &environment_role,
                 POST_KERNEL, CONFIG_SENSOR_INIT_PRIORITY, &fake_api);
DEVICE_DT_DEFINE(DT_NODELABEL(temperature_test), NULL, NULL, NULL, &temperature_role,
                 POST_KERNEL, CONFIG_SENSOR_INIT_PRIORITY, &fake_api);
DEVICE_DT_DEFINE(DT_NODELABEL(humidity_test), NULL, NULL, NULL, &humidity_role,
                 POST_KERNEL, CONFIG_SENSOR_INIT_PRIORITY, &fake_api);
DEVICE_DT_DEFINE(DT_NODELABEL(pressure_test), NULL, NULL, NULL, &pressure_role,
                 POST_KERNEL, CONFIG_SENSOR_INIT_PRIORITY, &fake_api);

ZTEST(ruuvi_motion, test_real_sensor_api_and_counter)
{
    const struct device *accel = DEVICE_DT_GET(DT_ALIAS(accel0));
    re_5_data_t sample;

    zassert_true(device_is_ready(accel));
    zassert_equal(ruuvi_sensor_motion_init(), -ENOTSUP);
    zassert_equal(threshold_calls, 0); /* Unsupported drivers are left untouched. */
    zassert_equal(ruuvi_sensor_motion_count_get(), 0U);

    supports_motion = true;
    zassert_equal(ruuvi_sensor_motion_init(), 0);
    zassert_equal(probe_calls, 1);
    zassert_equal(threshold_calls, 1);
    zassert_equal(enable_calls, 1);
    zassert_equal(captured_threshold.val1, 0);
    zassert_equal(captured_threshold.val2, 627626);
    zassert_not_null(captured_handler);
    zassert_not_null(captured_trigger);
    zassert_equal(captured_trigger->type, SENSOR_TRIG_MOTION);
    zassert_equal(captured_trigger->chan, SENSOR_CHAN_ACCEL_XYZ);
    zassert_equal(ruuvi_sensor_read(&sample), 6);
    zassert_equal(ruuvi_sensor_motion_count_get(), 0U); /* Reads do not become events. */

    for (uint32_t i = 0; i < 65536U; ++i) {
        captured_handler(accel, captured_trigger);
    }
    zassert_equal(ruuvi_sensor_motion_count_get(), 65536U);

    fail_threshold = true;
    zassert_equal(ruuvi_sensor_motion_init(), -EIO);
    zassert_equal(ruuvi_sensor_motion_count_get(), 0U);
    zassert_is_null(captured_handler); /* Failed configuration never arms a trigger. */
}

ZTEST(ruuvi_motion, test_split_environmental_channels)
{
    re_5_data_t sample;

    environment_available = true;
    temperature_available = true;
    humidity_available = true;
    pressure_available = true;
    zassert_equal(ruuvi_sensor_read(&sample), 6); /* Three fields, not six duplicates. */
    zassert_equal(sample.temperature_c, 22.25f);
    zassert_equal(sample.humidity_rh, 55.5f);
    zassert_equal(sample.pressure_pa, 100125.0f);

    environment_available = false;
    zassert_equal(ruuvi_sensor_read(&sample), 6);
    zassert_equal(sample.temperature_c, 22.25f);
    zassert_equal(sample.humidity_rh, 55.5f);
    zassert_equal(sample.pressure_pa, 100125.0f);

    environment_available = true;
    pressure_available = false;
    zassert_equal(ruuvi_sensor_read(&sample), 6);
    zassert_equal(sample.pressure_pa, 101200.0f); /* Fall back to env0. */

    temperature_available = false;
    humidity_available = false;
    pressure_available = false;
    zassert_equal(ruuvi_sensor_read(&sample), 6);
    zassert_equal(sample.temperature_c, 10.5f);
    zassert_equal(sample.humidity_rh, 33.0f);
    zassert_equal(sample.pressure_pa, 101200.0f);

    temperature_available = true;
    humidity_available = true;
    pressure_available = true;
}

ZTEST_SUITE(ruuvi_motion, NULL, NULL, NULL, NULL, NULL);
