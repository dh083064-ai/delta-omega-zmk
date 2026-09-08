/*
 * Delta Omega Cleanse alternator.
 *
 * Two skills (bound to LALT and F8 in-game) share one physical thumb key
 * and a ~3min cooldown each, so the intent is to use them alternately:
 * press 1 -> LALT, press 2 -> F8, press 3 -> LALT, ... This is NOT a
 * tap-dance (no waiting for a second tap) and NOT auto-repeat - each
 * physical press outputs exactly one HID key, immediately, and only the
 * internal alternator bit advances afterward.
 */
#define DT_DRV_COMPAT zmk_behavior_cleanse_alt

#include <zephyr/device.h>
#include <zephyr/kernel.h>

#include <drivers/behavior.h>
#include <zmk/behavior.h>
#include <zmk/event_manager.h>
#include <zmk/events/keycode_state_changed.h>
#include <dt-bindings/zmk/keys.h>

/* Single physical key drives this - one instance of state is correct. */
static bool cleanse_next_is_f8;
static uint32_t cleanse_active_code;
static bool cleanse_virtual_pressed;

static int cleanse_alt_pressed(struct zmk_behavior_binding *binding,
                                struct zmk_behavior_binding_event event) {
    ARG_UNUSED(binding);
    ARG_UNUSED(event);

    uint32_t code = cleanse_next_is_f8 ? F8 : LALT;
    cleanse_next_is_f8 = !cleanse_next_is_f8;

    cleanse_active_code = code;
    cleanse_virtual_pressed = true;
    raise_zmk_keycode_state_changed_from_encoded(code, true, k_uptime_get());

    return ZMK_BEHAVIOR_OPAQUE;
}

static int cleanse_alt_released(struct zmk_behavior_binding *binding,
                                 struct zmk_behavior_binding_event event) {
    ARG_UNUSED(binding);
    ARG_UNUSED(event);

    if (cleanse_virtual_pressed) {
        cleanse_virtual_pressed = false;
        raise_zmk_keycode_state_changed_from_encoded(cleanse_active_code, false, k_uptime_get());
    }

    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api behavior_cleanse_alt_driver_api = {
    .binding_pressed = cleanse_alt_pressed,
    .binding_released = cleanse_alt_released,
};

#define CLEANSE_ALT_INST(n)                                                                      \
    BEHAVIOR_DT_INST_DEFINE(n, NULL, NULL, NULL, NULL, POST_KERNEL,                              \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &behavior_cleanse_alt_driver_api);

DT_INST_FOREACH_STATUS_OKAY(CLEANSE_ALT_INST)
