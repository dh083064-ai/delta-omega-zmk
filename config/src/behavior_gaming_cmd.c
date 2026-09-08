/*
 * Delta Omega gaming command state machine (Gaming layer only).
 *
 *   Command A:  Down Down A     ->  Down Down 9
 *   Command B:  Down Down Down  ->  Down Down C
 *
 * Any OTHER physical key pressed between the Downs, or between the 2nd
 * Down and the trigger, immediately cancels the pending command - e.g.
 * "Down W Down A", "Down Down W A" and "Down A Down" are all plain,
 * unmodified input, never a command. This is on top of (not instead of)
 * the CONFIG_ZMK_GAMING_CMD_WINDOW_MS timeout: whichever happens first -
 * an unrelated key, or the window expiring - resets the state to idle.
 *
 * Three physical keys/positions participate, sharing one file-scope state
 * machine:
 *   - gaming-cmd-down (bound to the Down-arrow key, GCMD_DOWN_POSITION):
 *     every press outputs Down immediately and advances the state
 *     (idle -> D1 -> D2), except the 3rd press while D2 is armed, which is
 *     consumed and outputs C instead (Command B) and resets to idle.
 *   - gaming-cmd-trigger (bound to the A key, GCMD_TRIGGER_POSITION): if
 *     pressed while D2 is armed, the press is consumed and outputs the
 *     substitute keycode (9) instead of A (Command A), resetting to idle.
 *     Otherwise it outputs its normal keycode (A) unchanged, and if a
 *     single pending Down (D1) was armed, that gets cancelled too - a lone
 *     Down followed by A is not either command.
 *   - a zmk_position_state_changed listener covering every OTHER physical
 *     position: on any such position's *press* (never release), if a
 *     command is currently armed (D1/D2), it's cancelled back to idle and
 *     that key is left to its own normal binding untouched. This is a
 *     physical-position event, raised directly from the raw key matrix
 *     before any behavior/layer lookup, so it only ever sees genuine
 *     physical presses - never the synthetic Down/9/C HID keycode events
 *     this file raises itself (those go out on a different, position-less
 *     event type - zmk_keycode_state_changed - that this listener does
 *     not subscribe to). GCMD_DOWN_POSITION/GCMD_TRIGGER_POSITION are
 *     excluded from this listener since those two positions already
 *     handle their own state transitions above; this also sidesteps any
 *     question of event-ordering between this listener and those two
 *     behaviors for the same physical press.
 *
 * Nothing is ever buffered or delayed: every physical press/release still
 * produces its HID event on that same edge. Only the *value* sent for the
 * Down/A edges above is substituted based on state. Press/release are
 * tracked per key so a substituted key's release always matches whatever
 * was actually sent on press (e.g. if A was replaced with 9 on press,
 * releasing the physical A key releases 9, never a stray A release).
 */
#define DT_DRV_COMPAT zmk_behavior_gaming_cmd_down

#include <zephyr/device.h>
#include <zephyr/kernel.h>

#include <drivers/behavior.h>
#include <zmk/behavior.h>
#include <zmk/event_manager.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/events/position_state_changed.h>
#include <dt-bindings/zmk/keys.h>

/*
 * Fixed physical positions (this project's convention - see the combos
 * block in delta_omega.keymap, which hardcodes positions the same way).
 * Down lives at 17, A at 10, in every layer that binds these behaviors.
 */
#define GCMD_DOWN_POSITION 17
#define GCMD_TRIGGER_POSITION 10

enum gcmd_state {
    GCMD_IDLE = 0,
    GCMD_D1, /* one Down armed, waiting to see what follows */
    GCMD_D2, /* two Downs armed - next Down or A fires a command */
};

static enum gcmd_state gcmd_state = GCMD_IDLE;
static struct k_work_delayable gcmd_timeout_work;
/*
 * Dedicated, persistent storage for the sync-cancel below - Zephyr
 * requires this to outlive the call and never be shared between
 * concurrent cancel/flush operations, so it can't be a function-local
 * stack variable.
 */
static struct k_work_sync gcmd_timeout_sync;

static uint32_t gcmd_down_active_code;
static bool gcmd_down_virtual_pressed;

static uint32_t gcmd_trigger_active_code;
static bool gcmd_trigger_virtual_pressed;

static void gcmd_reset(void) {
    gcmd_state = GCMD_IDLE;
    /*
     * _sync so a timeout handler already mid-fire can't clobber a state
     * change a caller makes right after this returns - see the identical
     * reasoning in behavior_rapid_fire.c. Safe here because every caller
     * (the two behavior press handlers below, and the position listener)
     * runs in normal thread context, never inside gcmd_timeout_handler
     * itself - a work item must never sync-cancel itself.
     */
    k_work_cancel_delayable_sync(&gcmd_timeout_work, &gcmd_timeout_sync);
}

static void gcmd_timeout_handler(struct k_work *work) {
    ARG_UNUSED(work);
    gcmd_state = GCMD_IDLE;
}

static void gcmd_arm_window(void) {
    /*
     * reschedule (not schedule): schedule() is a no-op while a deadline is
     * already pending, so the 2nd Down wouldn't actually push the window
     * out - the whole Down-Down-trigger sequence would be capped at
     * WINDOW_MS from the 1st Down instead of each step getting its own
     * WINDOW_MS. reschedule() unconditionally resets the deadline from
     * now, giving every step the full window.
     */
    k_work_reschedule(&gcmd_timeout_work, K_MSEC(CONFIG_ZMK_GAMING_CMD_WINDOW_MS));
}

static int gcmd_init(const struct device *dev) {
    ARG_UNUSED(dev);
    k_work_init_delayable(&gcmd_timeout_work, gcmd_timeout_handler);
    return 0;
}

/* Any other physical key press cancels a pending command. */
static int gcmd_position_state_changed_listener(const zmk_event_t *eh) {
    const struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);

    if (ev == NULL || !ev->state) {
        /* Not a position event, or a release - releases never reset. */
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (ev->position == GCMD_DOWN_POSITION || ev->position == GCMD_TRIGGER_POSITION) {
        /* Handled by their own dedicated behaviors below. */
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (gcmd_state != GCMD_IDLE) {
        gcmd_reset();
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(gaming_cmd_reset, gcmd_position_state_changed_listener);
ZMK_SUBSCRIPTION(gaming_cmd_reset, zmk_position_state_changed);

static int gcmd_down_pressed(struct zmk_behavior_binding *binding,
                              struct zmk_behavior_binding_event event) {
    ARG_UNUSED(binding);
    ARG_UNUSED(event);

    uint32_t code;

    switch (gcmd_state) {
    case GCMD_IDLE:
        code = DOWN;
        gcmd_state = GCMD_D1;
        gcmd_arm_window();
        break;
    case GCMD_D1:
        code = DOWN;
        gcmd_state = GCMD_D2;
        gcmd_arm_window();
        break;
    case GCMD_D2:
    default:
        /* 3rd Down within the window - Command B. */
        code = C;
        gcmd_reset();
        break;
    }

    gcmd_down_active_code = code;
    gcmd_down_virtual_pressed = true;
    raise_zmk_keycode_state_changed_from_encoded(code, true, k_uptime_get());

    return ZMK_BEHAVIOR_OPAQUE;
}

static int gcmd_down_released(struct zmk_behavior_binding *binding,
                               struct zmk_behavior_binding_event event) {
    ARG_UNUSED(binding);
    ARG_UNUSED(event);

    if (gcmd_down_virtual_pressed) {
        gcmd_down_virtual_pressed = false;
        raise_zmk_keycode_state_changed_from_encoded(gcmd_down_active_code, false, k_uptime_get());
    }

    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api behavior_gaming_cmd_down_driver_api = {
    .binding_pressed = gcmd_down_pressed,
    .binding_released = gcmd_down_released,
};

#define GCMD_DOWN_INST(n)                                                                        \
    BEHAVIOR_DT_INST_DEFINE(n, gcmd_init, NULL, NULL, NULL, POST_KERNEL,                         \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,                                 \
                            &behavior_gaming_cmd_down_driver_api);

DT_INST_FOREACH_STATUS_OKAY(GCMD_DOWN_INST)

#undef DT_DRV_COMPAT
#define DT_DRV_COMPAT zmk_behavior_gaming_cmd_trigger

static int gcmd_trigger_pressed(struct zmk_behavior_binding *binding,
                                 struct zmk_behavior_binding_event event) {
    ARG_UNUSED(event);

    uint32_t normal_code = binding->param1;
    uint32_t substitute_code = binding->param2;
    uint32_t code;

    if (gcmd_state == GCMD_D2) {
        /* Two Downs armed - Command A. */
        code = substitute_code;
        gcmd_reset();
    } else {
        code = normal_code;
        if (gcmd_state == GCMD_D1) {
            /* Lone Down followed by A is not a command - cancel it. */
            gcmd_reset();
        }
    }

    gcmd_trigger_active_code = code;
    gcmd_trigger_virtual_pressed = true;
    raise_zmk_keycode_state_changed_from_encoded(code, true, k_uptime_get());

    return ZMK_BEHAVIOR_OPAQUE;
}

static int gcmd_trigger_released(struct zmk_behavior_binding *binding,
                                  struct zmk_behavior_binding_event event) {
    ARG_UNUSED(binding);
    ARG_UNUSED(event);

    if (gcmd_trigger_virtual_pressed) {
        gcmd_trigger_virtual_pressed = false;
        raise_zmk_keycode_state_changed_from_encoded(gcmd_trigger_active_code, false,
                                                      k_uptime_get());
    }

    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api behavior_gaming_cmd_trigger_driver_api = {
    .binding_pressed = gcmd_trigger_pressed,
    .binding_released = gcmd_trigger_released,
};

#define GCMD_TRIGGER_INST(n)                                                                     \
    BEHAVIOR_DT_INST_DEFINE(n, NULL, NULL, NULL, NULL, POST_KERNEL,                              \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,                                 \
                            &behavior_gaming_cmd_trigger_driver_api);

DT_INST_FOREACH_STATUS_OKAY(GCMD_TRIGGER_INST)
