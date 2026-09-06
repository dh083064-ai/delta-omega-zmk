/*
 * Delta Omega debounce-tap behavior.
 *
 * Sends a single bounded press+release for the given keycode, and ignores
 * any repeat trigger on the same physical position that arrives within
 * CONFIG_ZMK_DEBOUNCE_TAP_COOLDOWN_MS of the last one. Intended for a
 * single chattery key (e.g. Space) without touching the global kscan
 * debounce used by every other key.
 */
#define DT_DRV_COMPAT zmk_behavior_debounce_tap

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <drivers/behavior.h>
#include <zmk/behavior.h>
#include <zmk/event_manager.h>
#include <zmk/events/keycode_state_changed.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define DT_MAX_POSITIONS 64

struct dt_slot {
    struct k_work_delayable release_work;
    uint32_t encoded_keycode;
    int64_t last_fire_uptime;
    bool virtual_pressed;
};

static struct dt_slot dt_slots[DT_MAX_POSITIONS];

static void dt_release_work_handler(struct k_work *work) {
    struct k_work_delayable *dwork = k_work_delayable_from_work(work);
    struct dt_slot *slot = CONTAINER_OF(dwork, struct dt_slot, release_work);

    if (!slot->virtual_pressed) {
        return;
    }

    slot->virtual_pressed = false;
    raise_zmk_keycode_state_changed_from_encoded(slot->encoded_keycode, false, k_uptime_get());
}

static int dt_init(const struct device *dev) {
    ARG_UNUSED(dev);

    for (int i = 0; i < DT_MAX_POSITIONS; i++) {
        k_work_init_delayable(&dt_slots[i].release_work, dt_release_work_handler);
    }

    return 0;
}

static int on_keymap_binding_pressed(struct zmk_behavior_binding *binding,
                                     struct zmk_behavior_binding_event event) {
    if (event.position >= DT_MAX_POSITIONS) {
        return -EINVAL;
    }

    struct dt_slot *slot = &dt_slots[event.position];
    int64_t now = k_uptime_get();

    if (now - slot->last_fire_uptime < CONFIG_ZMK_DEBOUNCE_TAP_COOLDOWN_MS) {
        /* Too soon after the last real fire - almost certainly switch
         * chatter from the same physical press, not a deliberate new tap. */
        return ZMK_BEHAVIOR_OPAQUE;
    }

    slot->last_fire_uptime = now;
    slot->encoded_keycode = binding->param1;
    slot->virtual_pressed = true;

    raise_zmk_keycode_state_changed_from_encoded(slot->encoded_keycode, true, now);
    k_work_schedule(&slot->release_work, K_MSEC(CONFIG_ZMK_DEBOUNCE_TAP_MS));

    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_keymap_binding_released(struct zmk_behavior_binding *binding,
                                      struct zmk_behavior_binding_event event) {
    ARG_UNUSED(binding);
    ARG_UNUSED(event);
    /* Release is time-based (dt_release_work_handler), not tied to the
     * physical release - this is what makes the output a single bounded
     * pulse regardless of how long the physical key stays down. */
    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api behavior_debounce_tap_driver_api = {
    .binding_pressed = on_keymap_binding_pressed,
    .binding_released = on_keymap_binding_released,
};

#define DT_INST(n)                                                                                \
    BEHAVIOR_DT_INST_DEFINE(n, dt_init, NULL, NULL, NULL, POST_KERNEL,                           \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &behavior_debounce_tap_driver_api);

DT_INST_FOREACH_STATUS_OKAY(DT_INST)
