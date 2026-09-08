/*
 * Delta Omega gaming command state machine (Gaming layer only).
 *
 *   Command A:  Down Down A  ->  Down Down 9
 *   Command B:  Down Down Down  ->  Down Down C
 *
 * Two physical keys participate and each has its own compatible/driver
 * below, sharing one file-scope state machine:
 *   - gaming-cmd-down: bound to the Down-arrow key. Every press outputs
 *     Down immediately and advances the state (idle -> D1 -> D2), except
 *     the 3rd press within the command window, which is consumed and
 *     outputs C instead (Command B) and resets to idle.
 *   - gaming-cmd-trigger: bound to the A key. If pressed while state is
 *     D2 (two prior downs still armed), the press is consumed and outputs
 *     the substitute keycode (9) instead of A (Command A), resetting to
 *     idle. Otherwise it outputs its normal keycode (A) unchanged, and if
 *     a single pending Down (D1) was armed, that gets cancelled too - a
 *     lone Down followed by A is not either command.
 *
 * Nothing is ever buffered or delayed: every physical press/release still
 * produces its HID event on that same edge. Only the *value* sent for the
 * two edges above is substituted based on state. The window
 * (CONFIG_ZMK_GAMING_CMD_WINDOW_MS) only bounds how long a pending
 * Down/Down is remembered before auto-resetting to idle - it is not an
 * output delay. Press/release are tracked per key so a substituted key's
 * release always matches whatever was actually sent on press (e.g. if A
 * was replaced with 9 on press, releasing the physical A key releases 9,
 * never a stray A release).
 */
#define DT_DRV_COMPAT zmk_behavior_gaming_cmd_down

#include <zephyr/device.h>
#include <zephyr/kernel.h>

#include <drivers/behavior.h>
#include <zmk/behavior.h>
#include <zmk/event_manager.h>
#include <zmk/events/keycode_state_changed.h>
#include <dt-bindings/zmk/keys.h>

enum gcmd_state {
    GCMD_IDLE = 0,
    GCMD_D1, /* one Down armed, waiting to see what follows */
    GCMD_D2, /* two Downs armed - next Down or A fires a command */
};

static enum gcmd_state gcmd_state = GCMD_IDLE;
static struct k_work_delayable gcmd_timeout_work;

static uint32_t gcmd_down_active_code;
static bool gcmd_down_virtual_pressed;

static uint32_t gcmd_trigger_active_code;
static bool gcmd_trigger_virtual_pressed;

static void gcmd_timeout_handler(struct k_work *work) {
    ARG_UNUSED(work);
    gcmd_state = GCMD_IDLE;
}

static void gcmd_arm_window(void) {
    k_work_schedule(&gcmd_timeout_work, K_MSEC(CONFIG_ZMK_GAMING_CMD_WINDOW_MS));
}

static int gcmd_init(const struct device *dev) {
    ARG_UNUSED(dev);
    k_work_init_delayable(&gcmd_timeout_work, gcmd_timeout_handler);
    return 0;
}

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
        gcmd_state = GCMD_IDLE;
        k_work_cancel_delayable(&gcmd_timeout_work);
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
        gcmd_state = GCMD_IDLE;
        k_work_cancel_delayable(&gcmd_timeout_work);
    } else {
        code = normal_code;
        if (gcmd_state == GCMD_D1) {
            /* Lone Down followed by A is not a command - cancel it. */
            gcmd_state = GCMD_IDLE;
            k_work_cancel_delayable(&gcmd_timeout_work);
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
