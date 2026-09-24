/*
 * Magic Shift - single custom state machine, replacing the previous
 * zmk,behavior-hold-tap + mod-morph pair.
 *
 * Mirrors LunaKey Pico QMK's magic_shift_eng_finished/reset and
 * magic_shift_kor_finished/reset (single tap-dance state machine, no
 * count check - only state->pressed matters) with a flat OFF/PENDING/
 * HELD machine:
 *
 *   OFF     - idle.
 *   PENDING - physically down, undecided (short tap vs. hold vs.
 *             interrupted-by-another-key still open).
 *   HELD    - the modifier (LSHFT) is registered, either because
 *             another key went down while still pressed, or because
 *             the tapping term expired while still pressed.
 *
 * Deliberately NOT a zmk,behavior-hold-tap: ZMK's hold-tap module
 * resolves an interrupt by CAPTURING the other key's raw position
 * event and replaying it once decided. Two hold-tap instances capturing
 * and re-raising each other's events in a short timeframe is exactly
 * the trigger pattern behind zmkfirmware/zmk#2001 and #986 (mod-tap
 * modifiers getting stuck at the host, confirmed on "balanced" and
 * "hold-preferred", both still open upstream). This behavior never
 * captures anything - on interrupt it registers the modifier
 * immediately and lets the interrupting event bubble through
 * completely normally, so Magic Shift itself cannot participate in
 * that capture/replay chain. It does NOT fix the same class of bug on
 * the *other* side (a real zmk,behavior-hold-tap HRM key can still
 * capture a Magic Shift press/release as its own "other key" and get
 * its own modifier stuck via its own logic) - that is unchanged and
 * out of scope here.
 *
 * `ms_held` mirrors QMK's own magic_shift_*_held statics (state-
 * >pressed is already false by the time reset() runs for a genuine
 * hold-then-release, so QMK tracks registration itself rather than
 * trusting state->pressed there either) - it is the single source of
 * truth for whether *this* instance currently owns a registered
 * modifier, set true only right after a successful press invoke and
 * back to false immediately after the matching release invoke, so
 * release always unregisters exactly the one modifier this instance
 * registered, exactly once.
 *
 * ENG repeat / KOR no-repeat / Caps Word are unchanged: the short-tap
 * path still delegates to the existing magic_shift_tap /
 * magic_shift_kor_tap mod-morphs (shift_repeat, caps_word) exactly as
 * before - this file only replaces the outer press/interrupt/timeout/
 * release arbitration.
 */
#define DT_DRV_COMPAT zmk_behavior_magic_shift

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <drivers/behavior.h>
#include <zmk/behavior.h>
#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

enum magic_shift_state {
    MAGIC_SHIFT_OFF = 0,
    MAGIC_SHIFT_PENDING,
    MAGIC_SHIFT_HELD,
};

struct behavior_magic_shift_config {
    const char *mod_dev;  /* &kp - the modifier to hold, e.g. LSHFT */
    const char *tap_dev;  /* magic_shift_tap / magic_shift_kor_tap mod-morph */
    int32_t tapping_term_ms;
};

static enum magic_shift_state ms_state;
static bool ms_pressed_now;
static bool ms_held; /* do *we* currently own a registered modifier? */
static uint32_t ms_position = UINT32_MAX;
static uint32_t ms_mod_param;
static const struct behavior_magic_shift_config *ms_config;

static struct k_work_delayable ms_timer;
static struct k_work_sync ms_timer_sync;

static const char *ms_state_name(enum magic_shift_state s) {
    switch (s) {
    case MAGIC_SHIFT_OFF:
        return "OFF";
    case MAGIC_SHIFT_PENDING:
        return "PENDING";
    case MAGIC_SHIFT_HELD:
        return "HELD";
    default:
        return "UNKNOWN";
    }
}

static void ms_set_state(enum magic_shift_state new_state) {
    if (ms_state != new_state) {
        LOG_DBG("magic_shift: %s -> %s", ms_state_name(ms_state), ms_state_name(new_state));
        ms_state = new_state;
    }
}

static void ms_register_hold(int64_t timestamp) {
    LOG_DBG("magic_shift: registering hold modifier (position %d)", ms_position);

    struct zmk_behavior_binding_event event = {
        .position = ms_position,
        .timestamp = timestamp,
    };
    struct zmk_behavior_binding binding = {
        .behavior_dev = ms_config->mod_dev,
        .param1 = ms_mod_param,
    };

    zmk_behavior_invoke_binding(&binding, event, true);
    ms_held = true;
}

static void ms_unregister_hold(int64_t timestamp) {
    if (!ms_held) {
        return;
    }

    LOG_DBG("magic_shift: unregistering hold modifier (position %d)", ms_position);

    struct zmk_behavior_binding_event event = {
        .position = ms_position,
        .timestamp = timestamp,
    };
    struct zmk_behavior_binding binding = {
        .behavior_dev = ms_config->mod_dev,
        .param1 = ms_mod_param,
    };

    zmk_behavior_invoke_binding(&binding, event, false);
    ms_held = false;
}

static void ms_invoke_tap(int64_t timestamp) {
    LOG_DBG("magic_shift: short tap - invoking tap-side behavior");

    struct zmk_behavior_binding_event event = {
        .position = ms_position,
        .timestamp = timestamp,
    };
    struct zmk_behavior_binding binding = {
        .behavior_dev = ms_config->tap_dev,
        .param1 = 0,
    };

    zmk_behavior_invoke_binding(&binding, event, true);
    zmk_behavior_invoke_binding(&binding, event, false);
}

static void ms_timer_handler(struct k_work *work) {
    ARG_UNUSED(work);

    if (ms_pressed_now && ms_state == MAGIC_SHIFT_PENDING) {
        LOG_DBG("magic_shift: tapping-term expired while still held -> HELD");
        ms_register_hold(k_uptime_get());
        ms_set_state(MAGIC_SHIFT_HELD);
    }
}

/* Any other key going down while Magic Shift is still physically held
 * and undecided registers the modifier immediately and lets that key's
 * own event bubble through untouched - never captured, so Magic Shift
 * cannot end up in a capture/replay chain with another hold-tap. */
static int ms_position_state_changed_listener(const zmk_event_t *eh) {
    const struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);

    if (ev == NULL || !ev->state || ev->position == ms_position) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (ms_pressed_now && ms_state == MAGIC_SHIFT_PENDING) {
        LOG_DBG("magic_shift: interrupted by position %d while held -> HELD", ev->position);
        k_work_cancel_delayable_sync(&ms_timer, &ms_timer_sync);
        ms_register_hold(ev->timestamp);
        ms_set_state(MAGIC_SHIFT_HELD);
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(behavior_magic_shift, ms_position_state_changed_listener);
ZMK_SUBSCRIPTION(behavior_magic_shift, zmk_position_state_changed);

static int ms_keymap_binding_pressed(struct zmk_behavior_binding *binding,
                                     struct zmk_behavior_binding_event event) {
    const struct device *dev = zmk_behavior_get_binding(binding->behavior_dev);

    ms_config = dev->config;
    ms_position = event.position;
    ms_mod_param = binding->param1;
    ms_pressed_now = true;
    ms_set_state(MAGIC_SHIFT_PENDING);

    LOG_DBG("magic_shift: press (position %d)", ms_position);

    k_work_reschedule(&ms_timer, K_MSEC(ms_config->tapping_term_ms));

    return ZMK_BEHAVIOR_OPAQUE;
}

static int ms_keymap_binding_released(struct zmk_behavior_binding *binding,
                                      struct zmk_behavior_binding_event event) {
    ARG_UNUSED(binding);

    ms_pressed_now = false;
    k_work_cancel_delayable_sync(&ms_timer, &ms_timer_sync);

    if (ms_state == MAGIC_SHIFT_HELD) {
        LOG_DBG("magic_shift: release -> OFF (was HELD)");
        ms_unregister_hold(event.timestamp);
        ms_set_state(MAGIC_SHIFT_OFF);
        return ZMK_BEHAVIOR_OPAQUE;
    }

    /* Released before any interrupt or timeout - a plain short tap.
     * QMK's finished() doesn't wait for anything else here either
     * (no count check), so resolve immediately. */
    if (ms_state == MAGIC_SHIFT_PENDING) {
        ms_invoke_tap(event.timestamp);
        ms_set_state(MAGIC_SHIFT_OFF);
    }

    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api behavior_magic_shift_driver_api = {
    .binding_pressed = ms_keymap_binding_pressed,
    .binding_released = ms_keymap_binding_released,
};

static int behavior_magic_shift_init(const struct device *dev) {
    ARG_UNUSED(dev);
    k_work_init_delayable(&ms_timer, ms_timer_handler);
    return 0;
}

#define MAGIC_SHIFT_INST(n)                                                                       \
    static const struct behavior_magic_shift_config behavior_magic_shift_config_##n = {          \
        .mod_dev = DEVICE_DT_NAME(DT_INST_PHANDLE_BY_IDX(n, bindings, 0)),                        \
        .tap_dev = DEVICE_DT_NAME(DT_INST_PHANDLE_BY_IDX(n, bindings, 1)),                        \
        .tapping_term_ms = DT_INST_PROP(n, tapping_term_ms),                                      \
    };                                                                                             \
    BEHAVIOR_DT_INST_DEFINE(n, behavior_magic_shift_init, NULL, NULL,                             \
                            &behavior_magic_shift_config_##n, POST_KERNEL,                        \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &behavior_magic_shift_driver_api);

DT_INST_FOREACH_STATUS_OKAY(MAGIC_SHIFT_INST)
