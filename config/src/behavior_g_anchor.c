/*
 * Delta Omega G direction-anchored hold (experimental,
 * esb-tdma-pcbang-gaming-ganchor).
 *
 * Built from measuring the stable rapid-fire branch's real input
 * sequences: direction keys (LEFT/RIGHT) around G are NOT brief taps -
 * median hold ~330ms, essentially never <=50ms. A direction press is a
 * movement-phase *anchor*, not an end-of-dash signal, so unlike
 * glatch/groll this behavior never treats a direction press as a trigger
 * to release G early. Instead the direction press's own *timing* anchors
 * where G's HID release lands, while direction itself is left completely
 * untouched (LEFT/RIGHT stay plain &kp in the keymap - no wrapper
 * behavior, no reordering, no delay - overlap between G and direction is
 * intentional).
 *
 *   IDLE
 *     physical press -> HID down now, record press timestamp,
 *       dir_seen=false, -> HELD
 *
 *   HELD
 *     LEFT/RIGHT pressed (global listener, see below) -> just record
 *       dir_seen=true and the direction's press timestamp; G is NOT
 *       touched, direction proceeds as a completely normal, undelayed
 *       press
 *     physical release, elapsed >= MAX_TOTAL_HOLD_MS -> HID up now
 *       (release_reason=physical_long_hold - the hold alone already used
 *       the whole budget), -> IDLE
 *     physical release, elapsed < MAX_TOTAL_HOLD_MS -> compute
 *       release_target (see below) and schedule/fire accordingly,
 *       -> PENDING
 *
 *   PENDING (physically released, HID still down, release scheduled)
 *     release_target reached -> HID up, -> IDLE
 *       release_reason is whichever term of the clamp decided
 *       release_target: min_total / dir_tail / max_total
 *     LEFT/RIGHT pressed for the first time this session (dir_seen was
 *       false) -> record it, recompute release_target, reschedule the
 *       pending work for whatever time remains (or release immediately
 *       if that time has already passed) - direction itself is still
 *       completely untouched, only G's own timer moves
 *     G physical press (fast re-press before release fired) -> cancel
 *       the pending release via k_work_cancel_delayable_sync (persistent
 *       k_work_sync), HID stays down (no duplicate down edge), and per
 *       spec this starts a fresh session: press timestamp resets to now
 *       and dir_seen resets to false (release_reason=repress_merge is
 *       logged for this event, though no actual HID release happens -
 *       the session continues)
 *
 * release_target (ms from the physical press) =
 *   dir_seen ? clamp(dir_press_relative_ms + DIR_TAIL_MS,
 *                     MIN_TOTAL_HOLD_MS, MAX_TOTAL_HOLD_MS)
 *            : MAX_TOTAL_HOLD_MS   (waiting for a direction that hasn't
 *                                   shown up yet)
 *
 * Only G participates in this state machine (one physical key, plus a
 * position-based listener that only ever *reads* LEFT/RIGHT presses), so
 * this uses plain static state rather than the per-position slot arrays
 * behavior_rapid_fire.c/behavior_g_stretch.c use for keys bindable at
 * multiple positions.
 */
#define DT_DRV_COMPAT zmk_behavior_g_anchor

#include <errno.h>
#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/time_units.h>

#include <drivers/behavior.h>
#include <zmk/behavior.h>
#include <zmk/event_manager.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/events/position_state_changed.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/*
 * Fixed physical positions (this project's convention - see the combos
 * block in delta_omega.keymap, which hardcodes positions the same way).
 * LEFT/RIGHT live at 16/18 in every layer that keeps this Gaming layout.
 */
#define GANCHOR_LEFT_POSITION 16
#define GANCHOR_RIGHT_POSITION 18

enum ganchor_state {
    GANCHOR_IDLE = 0,
    GANCHOR_HELD,
    GANCHOR_PENDING,
};

enum ganchor_reason {
    GANCHOR_REASON_NONE = 0,
    GANCHOR_REASON_MIN_TOTAL,
    GANCHOR_REASON_DIR_TAIL,
    GANCHOR_REASON_MAX_TOTAL,
    GANCHOR_REASON_PHYSICAL_LONG_HOLD,
};

static enum ganchor_state g_state = GANCHOR_IDLE;
static uint32_t g_encoded_keycode;
static uint32_t g_press_cycles;
static bool g_dir_seen;
static uint32_t g_dir_press_cycles;
static enum ganchor_reason g_pending_reason;
static struct k_work_delayable g_release_work;
/* Persistent, not stack-local - see behavior_rapid_fire.c for why. */
static struct k_work_sync g_release_sync;

static inline uint32_t ganchor_elapsed_ms(uint32_t cycles) {
    return k_cyc_to_ms_floor32(k_cycle_get_32() - cycles);
}

static void ganchor_send_release(void) {
    raise_zmk_keycode_state_changed_from_encoded(g_encoded_keycode, false, k_uptime_get());
    g_state = GANCHOR_IDLE;
}

/*
 * Computes release_target from current state, then either fires the
 * release immediately (if that target is already in the past) or
 * (re)schedules the delayed release for whatever time remains.
 * k_work_reschedule is used unconditionally - it schedules cleanly
 * whether or not a delayed release was already pending, so this same
 * function works both for the very first schedule (at physical release)
 * and for recomputing when a direction shows up mid-PENDING.
 */
static void ganchor_recompute(void) {
    uint32_t elapsed_now = ganchor_elapsed_ms(g_press_cycles);
    uint32_t target;

    if (g_dir_seen) {
        uint32_t dir_rel_ms = k_cyc_to_ms_floor32(g_dir_press_cycles - g_press_cycles);
        uint32_t wanted = dir_rel_ms + CONFIG_ZMK_G_ANCHOR_DIR_TAIL_MS;

        if (wanted < CONFIG_ZMK_G_ANCHOR_MIN_TOTAL_HOLD_MS) {
            target = CONFIG_ZMK_G_ANCHOR_MIN_TOTAL_HOLD_MS;
            g_pending_reason = GANCHOR_REASON_MIN_TOTAL;
        } else if (wanted > CONFIG_ZMK_G_ANCHOR_MAX_TOTAL_HOLD_MS) {
            target = CONFIG_ZMK_G_ANCHOR_MAX_TOTAL_HOLD_MS;
            g_pending_reason = GANCHOR_REASON_MAX_TOTAL;
        } else {
            target = wanted;
            g_pending_reason = GANCHOR_REASON_DIR_TAIL;
        }
    } else {
        target = CONFIG_ZMK_G_ANCHOR_MAX_TOTAL_HOLD_MS;
        g_pending_reason = GANCHOR_REASON_MAX_TOTAL;
    }

    if (elapsed_now >= target) {
        ganchor_send_release();
    } else {
        k_work_reschedule(&g_release_work, K_MSEC(target - elapsed_now));
    }
}

static void ganchor_release_work_handler(struct k_work *work) {
    ARG_UNUSED(work);

    if (g_state != GANCHOR_PENDING) {
        /* Cancelled/superseded by a re-press. */
        return;
    }

    ganchor_send_release();
}

static int ganchor_init(const struct device *dev) {
    ARG_UNUSED(dev);
    k_work_init_delayable(&g_release_work, ganchor_release_work_handler);
    return 0;
}

/* Direction keys stay plain &kp - this listener only ever *observes*
 * their physical press, never delays or reorders them. See the file
 * header for why position_state_changed (not the position-less
 * keycode_state_changed our own behavior raises) is the right event. */
static int ganchor_position_state_changed_listener(const zmk_event_t *eh) {
    const struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);

    if (ev == NULL || !ev->state) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (ev->position != GANCHOR_LEFT_POSITION && ev->position != GANCHOR_RIGHT_POSITION) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (g_state == GANCHOR_IDLE || g_dir_seen) {
        /* Not relevant, or already have our one anchor for this session. */
        return ZMK_EV_EVENT_BUBBLE;
    }

    g_dir_seen = true;
    g_dir_press_cycles = k_cycle_get_32();

    if (g_state == GANCHOR_PENDING) {
        ganchor_recompute();
    }
    /* GANCHOR_HELD: nothing more to do now - accounted for once G
     * physically releases. */

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(ganchor_direction, ganchor_position_state_changed_listener);
ZMK_SUBSCRIPTION(ganchor_direction, zmk_position_state_changed);

static int on_keymap_binding_pressed(struct zmk_behavior_binding *binding,
                                     struct zmk_behavior_binding_event event) {
    ARG_UNUSED(event);

    if (g_state == GANCHOR_PENDING) {
        /*
         * Fast re-press before the computed release fired. _sync so a
         * handler already mid-fire can't still send the UP edge right
         * after we decide to keep holding - safe here (normal keymap/
         * behavior dispatch context, never ganchor_release_work_handler
         * itself). HID is already down, so no new press edge. Per spec
         * this is treated as a fresh session: press reference and
         * dir_seen both reset.
         */
        k_work_cancel_delayable_sync(&g_release_work, &g_release_sync);
        g_state = GANCHOR_HELD;
        g_press_cycles = k_cycle_get_32();
        g_dir_seen = false;
        return ZMK_BEHAVIOR_OPAQUE;
    }

    if (g_state == GANCHOR_HELD) {
        /* Physically impossible (key already down) - ignore defensively. */
        return ZMK_BEHAVIOR_OPAQUE;
    }

    g_encoded_keycode = binding->param1;
    g_press_cycles = k_cycle_get_32();
    g_dir_seen = false;
    g_state = GANCHOR_HELD;
    raise_zmk_keycode_state_changed_from_encoded(g_encoded_keycode, true, k_uptime_get());

    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_keymap_binding_released(struct zmk_behavior_binding *binding,
                                      struct zmk_behavior_binding_event event) {
    ARG_UNUSED(binding);
    ARG_UNUSED(event);

    if (g_state != GANCHOR_HELD) {
        /* Not physically down from our side (stray release) - ignore. */
        return ZMK_BEHAVIOR_OPAQUE;
    }

    uint32_t elapsed_ms = ganchor_elapsed_ms(g_press_cycles);

    if (elapsed_ms >= CONFIG_ZMK_G_ANCHOR_MAX_TOTAL_HOLD_MS) {
        g_pending_reason = GANCHOR_REASON_PHYSICAL_LONG_HOLD;
        ganchor_send_release();
        return ZMK_BEHAVIOR_OPAQUE;
    }

    g_state = GANCHOR_PENDING;
    ganchor_recompute();

    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api behavior_g_anchor_driver_api = {
    .binding_pressed = on_keymap_binding_pressed,
    .binding_released = on_keymap_binding_released,
};

#define GANCHOR_INST(n)                                                                          \
    BEHAVIOR_DT_INST_DEFINE(n, ganchor_init, NULL, NULL, NULL, POST_KERNEL,                      \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &behavior_g_anchor_driver_api);

DT_INST_FOREACH_STATUS_OKAY(GANCHOR_INST)
