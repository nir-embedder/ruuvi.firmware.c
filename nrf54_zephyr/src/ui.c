#include "ui.h"

#include <errno.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

#define DEBOUNCE_MS 50
#define LONG_PRESS_MS 5000
#define CONFIG_WINDOW_MS 60000
#define STARTUP_OK_MS 1000

static atomic_t config_pending;
static atomic_t recovery_requested;
static K_SEM_DEFINE(ui_events, 0, 1);
static K_MUTEX_DEFINE(feedback_lock);
static bool activity_signal;
static bool startup_signal;
static bool configuration_signal;

#if DT_NODE_HAS_STATUS(DT_ALIAS(led0), okay)
static const struct gpio_dt_spec activity_led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
#endif
#if DT_NODE_HAS_STATUS(DT_ALIAS(led1), okay)
static const struct gpio_dt_spec error_led = GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios);
#endif
#if DT_NODE_HAS_STATUS(DT_ALIAS(led2), okay)
static const struct gpio_dt_spec configuration_led = GPIO_DT_SPEC_GET(DT_ALIAS(led2), gpios);
#endif

#if DT_NODE_HAS_STATUS(DT_ALIAS(sw0), okay)
static const struct gpio_dt_spec button = GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);
static struct gpio_callback button_callback;
static bool button_pressed;
static int64_t pressed_at;
#endif

static void update_feedback(void)
{
#if DT_NODE_HAS_STATUS(DT_ALIAS(led0), okay)
    (void)gpio_pin_set_dt(&activity_led, (activity_signal || startup_signal)
#if DT_NODE_HAS_STATUS(DT_ALIAS(led2), okay)
                          && !configuration_signal
#endif
    );
#endif
#if DT_NODE_HAS_STATUS(DT_ALIAS(led2), okay)
    bool blue_on = (activity_signal || startup_signal) && configuration_signal;
#if DT_NODE_HAS_STATUS(DT_ALIAS(sw0), okay)
    blue_on = blue_on || button_pressed;
#endif
    (void)gpio_pin_set_dt(&configuration_led, blue_on);
#endif
}

static void startup_finished(struct k_work *work)
{
    ARG_UNUSED(work);
    k_mutex_lock(&feedback_lock, K_FOREVER);
    startup_signal = false;
    update_feedback();
    k_mutex_unlock(&feedback_lock);
}

static K_WORK_DELAYABLE_DEFINE(startup_feedback, startup_finished);

#if DT_NODE_HAS_STATUS(DT_ALIAS(sw0), okay)
static void config_expired(struct k_work *work);
static void long_pressed(struct k_work *work);
static void button_debounced(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(config_timeout, config_expired);
static K_WORK_DELAYABLE_DEFINE(long_press, long_pressed);
static K_WORK_DELAYABLE_DEFINE(button_debounce, button_debounced);

static void config_expired(struct k_work *work)
{
    ARG_UNUSED(work);
    atomic_clear(&config_pending);
    k_sem_give(&ui_events);
}

static void long_pressed(struct k_work *work)
{
    ARG_UNUSED(work);
    if (button_pressed && gpio_pin_get_dt(&button) == 1) {
        atomic_set(&recovery_requested, 1);
        atomic_clear(&config_pending);
        (void)k_work_cancel_delayable(&config_timeout);
        k_sem_give(&ui_events);
    }
}

static void button_debounced(struct k_work *work)
{
    ARG_UNUSED(work);
    int state = gpio_pin_get_dt(&button);

    if (state < 0 || (state != 0) == button_pressed) {
        return;
    }
    if (state != 0) {
        k_mutex_lock(&feedback_lock, K_FOREVER);
        button_pressed = true;
        update_feedback();
        k_mutex_unlock(&feedback_lock);
        pressed_at = k_uptime_get();
        (void)k_work_reschedule(&long_press, K_MSEC(LONG_PRESS_MS));
    } else {
        k_mutex_lock(&feedback_lock, K_FOREVER);
        button_pressed = false;
        update_feedback();
        k_mutex_unlock(&feedback_lock);
        (void)k_work_cancel_delayable(&long_press);
        if (atomic_get(&recovery_requested)) {
            return;
        }
        if (k_uptime_get() - pressed_at >= LONG_PRESS_MS) {
            atomic_set(&recovery_requested, 1);
            atomic_clear(&config_pending);
            (void)k_work_cancel_delayable(&config_timeout);
        } else {
            atomic_set(&config_pending, 1);
            (void)k_work_reschedule(&config_timeout, K_MSEC(CONFIG_WINDOW_MS));
        }
        k_sem_give(&ui_events);
    }
}

static void button_changed(const struct device *dev, struct gpio_callback *cb,
                           gpio_port_pins_t pins)
{
    ARG_UNUSED(dev);
    ARG_UNUSED(cb);
    ARG_UNUSED(pins);
    (void)k_work_reschedule(&button_debounce, K_MSEC(DEBOUNCE_MS));
}
#endif

struct k_sem *ruuvi_ui_event_sem(void)
{
    return &ui_events;
}

int ruuvi_ui_init(void)
{
    int err;

    k_sem_reset(&ui_events);
    atomic_clear(&config_pending);
    atomic_clear(&recovery_requested);
    activity_signal = false;
    startup_signal = false;
    configuration_signal = false;
#if DT_NODE_HAS_STATUS(DT_ALIAS(led0), okay)
    if (!gpio_is_ready_dt(&activity_led)) {
        return -ENODEV;
    }
    err = gpio_pin_configure_dt(&activity_led, GPIO_OUTPUT_INACTIVE);
    if (err) {
        return err;
    }
#endif
#if DT_NODE_HAS_STATUS(DT_ALIAS(led1), okay)
    if (!gpio_is_ready_dt(&error_led)) {
        return -ENODEV;
    }
    err = gpio_pin_configure_dt(&error_led, GPIO_OUTPUT_INACTIVE);
    if (err) {
        return err;
    }
#endif
#if DT_NODE_HAS_STATUS(DT_ALIAS(led2), okay)
    if (!gpio_is_ready_dt(&configuration_led)) {
        return -ENODEV;
    }
    err = gpio_pin_configure_dt(&configuration_led, GPIO_OUTPUT_INACTIVE);
    if (err) {
        return err;
    }
#endif
#if DT_NODE_HAS_STATUS(DT_ALIAS(sw0), okay)
    if (!gpio_is_ready_dt(&button)) {
        return -ENODEV;
    }
    err = gpio_pin_configure_dt(&button, GPIO_INPUT);
    if (err) {
        return err;
    }
    button_pressed = false;
    gpio_init_callback(&button_callback, button_changed, BIT(button.pin));
    err = gpio_add_callback(button.port, &button_callback);
    if (err) {
        return err;
    }
    err = gpio_pin_interrupt_configure_dt(&button, GPIO_INT_EDGE_BOTH);
    if (err) {
        (void)gpio_remove_callback(button.port, &button_callback);
        return err;
    }
    (void)k_work_reschedule(&button_debounce, K_MSEC(DEBOUNCE_MS));
#else
    (void)err;
#endif
    return 0;
}

void ruuvi_ui_activity(bool on)
{
    k_mutex_lock(&feedback_lock, K_FOREVER);
    activity_signal = on;
    update_feedback();
    k_mutex_unlock(&feedback_lock);
}

void ruuvi_ui_startup_success(void)
{
    ruuvi_ui_error(false);
    k_mutex_lock(&feedback_lock, K_FOREVER);
    startup_signal = true;
    update_feedback();
    k_mutex_unlock(&feedback_lock);
    (void)k_work_reschedule(&startup_feedback, K_MSEC(STARTUP_OK_MS));
}

void ruuvi_ui_configuration(bool on)
{
    k_mutex_lock(&feedback_lock, K_FOREVER);
    if (configuration_signal != on) {
        configuration_signal = on;
        update_feedback();
    }
    k_mutex_unlock(&feedback_lock);
}

void ruuvi_ui_error(bool on)
{
#if DT_NODE_HAS_STATUS(DT_ALIAS(led1), okay)
    (void)gpio_pin_set_dt(&error_led, on);
#else
    ARG_UNUSED(on);
#endif
}

bool ruuvi_ui_config_pending(void)
{
    return atomic_get(&config_pending) != 0;
}

bool ruuvi_ui_config_claim(void)
{
    return atomic_clear(&config_pending) != 0;
}

bool ruuvi_ui_recovery_requested(void)
{
    return atomic_get(&recovery_requested) != 0;
}
