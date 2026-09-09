/*
 * Delta Omega chatter-guard behavior.
 *
 * Passes physical press/release straight through as HID down/up - hold
 * duration and OS auto-repeat behave exactly like a plain &kp, since
 * nothing here caps or delays either edge. The only thing this adds: a
 * press that arrives within CONFIG_ZMK_CHATTER_GUARD_MS of the last
 * accepted release on the same physical position is treated as switch
 * chatter, and its entire press/release pair is dropped - not just the
 * press - so a suppressed press can never leave behind a stray HID down
 * or an unmatched HID up later. Fully synchronous: a press or release is
 * resolved (forwarded or dropped) inside the same callback that received
 * it, no timers or workqueue involved.
 */
#define DT_DRV_COMPAT zmk_behavior_chatter_guard

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <drivers/behavior.h>
#include <zmk/behavior.h>
#include <zmk/event_manager.h>
#include <zmk/events/keycode_state_changed.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define CG_MAX_POSITIONS 64

struct cg_slot {
    uint32_t encoded_keycode;
    int64_t last_release_uptime;
    bool suppressing;     /* true while the current physical press/release
                            * pair is being dropped as chatter. */
    bool has_last_release; /* false until the first accepted release, so
                             * the very first press after boot is never
                             * mistaken for chatter. */
};

static struct cg_slot cg_slots[CG_MAX_POSITIONS];

static int on_keymap_binding_pressed(struct zmk_behavior_binding *binding,
                                     struct zmk_behavior_binding_event event) {
    if (event.position >= CG_MAX_POSITIONS) {
        return -EINVAL;
    }

    struct cg_slot *slot = &cg_slots[event.position];
    int64_t now = k_uptime_get();

    if (slot->has_last_release && (now - slot->last_release_uptime < CONFIG_ZMK_CHATTER_GUARD_MS)) {
        /* Too soon after the last accepted release - almost certainly
         * switch chatter, not a deliberate new press. Suppress it, and
         * remember to suppress its matching release too. */
        slot->suppressing = true;
        return ZMK_BEHAVIOR_OPAQUE;
    }

    slot->suppressing = false;
    slot->encoded_keycode = binding->param1;
    raise_zmk_keycode_state_changed_from_encoded(slot->encoded_keycode, true, now);

    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_keymap_binding_released(struct zmk_behavior_binding *binding,
                                      struct zmk_behavior_binding_event event) {
    ARG_UNUSED(binding);

    if (event.position >= CG_MAX_POSITIONS) {
        return -EINVAL;
    }

    struct cg_slot *slot = &cg_slots[event.position];
    int64_t now = k_uptime_get();

    if (slot->suppressing) {
        /* Matching release for a press already dropped as chatter - drop
         * this too, and leave last_release_uptime untouched (this was
         * never a real key-up). */
        slot->suppressing = false;
        return ZMK_BEHAVIOR_OPAQUE;
    }

    slot->has_last_release = true;
    slot->last_release_uptime = now;
    raise_zmk_keycode_state_changed_from_encoded(slot->encoded_keycode, false, now);

    return ZMK_BEHAVIOR_OPAQUE;
}

static int cg_init(const struct device *dev) {
    ARG_UNUSED(dev);
    return 0;
}

static const struct behavior_driver_api behavior_chatter_guard_driver_api = {
    .binding_pressed = on_keymap_binding_pressed,
    .binding_released = on_keymap_binding_released,
};

#define CG_INST(n)                                                                                \
    BEHAVIOR_DT_INST_DEFINE(n, cg_init, NULL, NULL, NULL, POST_KERNEL,                            \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &behavior_chatter_guard_driver_api);

DT_INST_FOREACH_STATUS_OKAY(CG_INST)
