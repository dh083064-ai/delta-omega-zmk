/*
 * Delta Omega rapid-fire behavior for ZMK v0.3.
 * Physical hold => repeated HID taps with a fresh randomized interval each cycle.
 */
#define DT_DRV_COMPAT zmk_behavior_rapid_fire

#include <errno.h>
#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/random/random.h>

#include <drivers/behavior.h>
#include <zmk/behavior.h>
#include <zmk/event_manager.h>
#include <zmk/events/keycode_state_changed.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define RF_MAX_POSITIONS 64

struct rf_slot {
    struct k_work_delayable repeat_work;
    struct k_work_delayable release_work;
    uint32_t encoded_keycode;
    bool active;
    bool virtual_pressed;
};

static struct rf_slot rf_slots[RF_MAX_POSITIONS];

static uint32_t rf_next_interval_ms(void) {
    const uint32_t min_ms = CONFIG_ZMK_RAPID_FIRE_MIN_INTERVAL_MS;
    const uint32_t max_ms = CONFIG_ZMK_RAPID_FIRE_MAX_INTERVAL_MS;

    if (max_ms <= min_ms) {
        return min_ms;
    }

    /* Inclusive range: [min_ms, max_ms]. */
    return min_ms + (sys_rand32_get() % (max_ms - min_ms + 1U));
}


static void rf_send_press(struct rf_slot *slot) {
    if (slot->virtual_pressed || !slot->active) {
        return;
    }
    slot->virtual_pressed = true;
    raise_zmk_keycode_state_changed_from_encoded(slot->encoded_keycode, true, k_uptime_get());
}

static void rf_send_release(struct rf_slot *slot) {
    if (!slot->virtual_pressed) {
        return;
    }
    slot->virtual_pressed = false;
    raise_zmk_keycode_state_changed_from_encoded(slot->encoded_keycode, false, k_uptime_get());
}

static void rf_release_work_handler(struct k_work *work) {
    struct k_work_delayable *dwork = k_work_delayable_from_work(work);
    struct rf_slot *slot = CONTAINER_OF(dwork, struct rf_slot, release_work);
    rf_send_release(slot);
}

static void rf_repeat_work_handler(struct k_work *work) {
    struct k_work_delayable *dwork = k_work_delayable_from_work(work);
    struct rf_slot *slot = CONTAINER_OF(dwork, struct rf_slot, repeat_work);

    if (!slot->active) {
        return;
    }

    rf_send_release(slot);
    rf_send_press(slot);
    k_work_schedule(&slot->release_work, K_MSEC(CONFIG_ZMK_RAPID_FIRE_TAP_MS));
    k_work_schedule(&slot->repeat_work, K_MSEC(rf_next_interval_ms()));
}

static int rapid_fire_init(const struct device *dev) {
    ARG_UNUSED(dev);

    for (int i = 0; i < RF_MAX_POSITIONS; i++) {
        k_work_init_delayable(&rf_slots[i].repeat_work, rf_repeat_work_handler);
        k_work_init_delayable(&rf_slots[i].release_work, rf_release_work_handler);
    }

    return 0;
}

static int on_keymap_binding_pressed(struct zmk_behavior_binding *binding,
                                     struct zmk_behavior_binding_event event) {
    if (event.position >= RF_MAX_POSITIONS) {
        return -EINVAL;
    }

    struct rf_slot *slot = &rf_slots[event.position];

    k_work_cancel_delayable(&slot->repeat_work);
    k_work_cancel_delayable(&slot->release_work);
    rf_send_release(slot);

    slot->encoded_keycode = binding->param1;
    slot->active = true;

    rf_send_press(slot);
    k_work_schedule(&slot->release_work, K_MSEC(CONFIG_ZMK_RAPID_FIRE_TAP_MS));
    k_work_schedule(&slot->repeat_work, K_MSEC(rf_next_interval_ms()));

    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_keymap_binding_released(struct zmk_behavior_binding *binding,
                                      struct zmk_behavior_binding_event event) {
    ARG_UNUSED(binding);

    if (event.position >= RF_MAX_POSITIONS) {
        return -EINVAL;
    }

    struct rf_slot *slot = &rf_slots[event.position];
    slot->active = false;
    k_work_cancel_delayable(&slot->repeat_work);
    k_work_cancel_delayable(&slot->release_work);
    rf_send_release(slot);

    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api behavior_rapid_fire_driver_api = {
    .binding_pressed = on_keymap_binding_pressed,
    .binding_released = on_keymap_binding_released,
};

#define RF_INST(n)                                                                                 \
    BEHAVIOR_DT_INST_DEFINE(n, rapid_fire_init, NULL, NULL, NULL, POST_KERNEL,                    \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &behavior_rapid_fire_driver_api);

DT_INST_FOREACH_STATUS_OKAY(RF_INST)
