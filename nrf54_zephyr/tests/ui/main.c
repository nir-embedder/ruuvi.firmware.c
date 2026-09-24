#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/gpio/gpio_emul.h>
#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include "ui.h"

static const struct gpio_dt_spec green = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
static const struct gpio_dt_spec red = GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios);
static const struct gpio_dt_spec blue = GPIO_DT_SPEC_GET(DT_ALIAS(led2), gpios);
static const struct gpio_dt_spec button = GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);

static void check_leds(int green_on, int red_on, int blue_on)
{
    zassert_equal(gpio_emul_output_get(green.port, green.pin), green_on);
    zassert_equal(gpio_emul_output_get(red.port, red.pin), red_on);
    zassert_equal(gpio_emul_output_get(blue.port, blue.pin), blue_on);
}

ZTEST(ruuvi_ui, test_feedback_and_button_windows)
{
    zassert_equal(ruuvi_ui_init(), 0);
    k_sleep(K_MSEC(70)); /* Settle the initial button debounce. */
    check_leds(0, 0, 0);

    ruuvi_ui_activity(true);
    check_leds(1, 0, 0);
    ruuvi_ui_configuration(true);
    check_leds(0, 0, 1);
    ruuvi_ui_activity(false);
    check_leds(0, 0, 0);
    ruuvi_ui_configuration(false);
    ruuvi_ui_activity(true);
    check_leds(1, 0, 0);

    zassert_equal(gpio_emul_input_set(button.port, button.pin, 1), 0);
    k_sleep(K_MSEC(100));
    check_leds(1, 0, 1);
    zassert_equal(gpio_emul_input_set(button.port, button.pin, 0), 0);
    k_sleep(K_MSEC(100));
    check_leds(1, 0, 0);
    zassert_true(ruuvi_ui_config_pending());
    zassert_true(ruuvi_ui_config_claim());
    zassert_false(ruuvi_ui_config_claim()); /* Callback and main cannot both consume it. */
    zassert_false(ruuvi_ui_config_pending());
    zassert_false(ruuvi_ui_recovery_requested());

    ruuvi_ui_configuration(true);
    check_leds(0, 0, 1);
    ruuvi_ui_error(true);
    check_leds(0, 1, 1);
    ruuvi_ui_error(false);
    ruuvi_ui_activity(false);
    check_leds(0, 0, 0);

    zassert_equal(gpio_emul_input_set(button.port, button.pin, 1), 0);
    k_sleep(K_MSEC(100));
    zassert_equal(gpio_emul_input_set(button.port, button.pin, 0), 0);
    k_sleep(K_MSEC(100));
    zassert_true(ruuvi_ui_config_claim()); /* Another release opens a new window. */
    zassert_false(ruuvi_ui_config_claim());

    zassert_equal(gpio_emul_input_set(button.port, button.pin, 1), 0);
    k_sleep(K_MSEC(5200));
    zassert_true(ruuvi_ui_recovery_requested());
    zassert_false(ruuvi_ui_config_pending());
    zassert_equal(gpio_emul_input_set(button.port, button.pin, 0), 0);
    k_sleep(K_MSEC(100));
    check_leds(0, 0, 0);
}

ZTEST_SUITE(ruuvi_ui, NULL, NULL, NULL, NULL, NULL);
