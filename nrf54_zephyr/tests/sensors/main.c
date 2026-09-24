#include <errno.h>
#include <stdbool.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include "sensors.h"

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
    ARG_UNUSED(dev);
    if (!supports_motion) {
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
    ARG_UNUSED(dev);
    if (channel != SENSOR_CHAN_ACCEL_XYZ || attribute != SENSOR_ATTR_UPPER_THRESH) {
        return -ENOTSUP;
    }
    ++threshold_calls;
    captured_threshold = *value;
    return fail_threshold ? -EIO : 0;
}

static int fake_sample_fetch(const struct device *dev, enum sensor_channel channel)
{
    ARG_UNUSED(dev);
    ARG_UNUSED(channel);
    return 0;
}

static int fake_channel_get(const struct device *dev, enum sensor_channel channel,
                            struct sensor_value *values)
{
    ARG_UNUSED(dev);
    if (channel != SENSOR_CHAN_ACCEL_XYZ) {
        return -ENOTSUP;
    }
    values[0] = (struct sensor_value) {0, 0};
    values[1] = (struct sensor_value) {0, 0};
    values[2] = (struct sensor_value) {9, 806650};
    return 0;
}

static const struct sensor_driver_api fake_api = {
    .trigger_set = fake_trigger_set,
    .attr_set = fake_attr_set,
    .sample_fetch = fake_sample_fetch,
    .channel_get = fake_channel_get,
};
DEVICE_DT_DEFINE(DT_NODELABEL(motion_test), NULL, NULL, NULL, NULL,
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
    zassert_equal(ruuvi_sensor_read(&sample), 3);
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

ZTEST_SUITE(ruuvi_motion, NULL, NULL, NULL, NULL, NULL);
