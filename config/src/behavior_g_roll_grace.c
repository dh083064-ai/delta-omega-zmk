/*
 * Delta Omega G roll-aware grace (experimental, esb-tdma-pcbang-gaming-groll).
 *
 * Built directly from measuring the stable rapid-fire branch's actual
 * input sequences: ~43% of G sessions already have a direction key
 * (LEFT/RIGHT) pressed WHILE G is still physically held (the "roll"
 * pattern - direction switches mid-dash), not after G releases. glatch's
 * "wait for a NEW direction press after release" design structurally
 * misses all of those, falling through to its timeout instead. This
 * behavior instead watches for direction *during* the hold and, if seen,
 * releases G the instant it's physically released - no grace needed at
 * all for the roll case. Only the remaining case (no direction seen yet)
 * gets a short post-release grace.
 *
 *   IDLE
 *     physical press -> HID down now, record press timestamp,
 *       direction_seen=false, -> HELD
 *
 *   HELD
 *     LEFT/RIGHT pressed (groll_direction, see below) -> just note
 *       direction_seen=true; G is NOT touched, stays HID-down, no
 *       release here - the direction key proceeds as a completely
 *       normal press
 *     physical release, direction_seen==true -> HID up now
 *       (release_reason: "direction_before_release" - the roll case),
 *       -> IDLE
 *     physical release, direction_seen==false -> schedule a grace
 *       timeout for CONFIG_ZMK_G_ROLL_GRACE_MS, -> GRACE
 *
 *   GRACE
 *     grace timeout fires -> HID up (release_reason: "timeout"), -> IDLE
 *     LEFT/RIGHT is freshly pressed (groll_direction) -> cancel the
 *       grace timeout, HID up (release_reason: "direction_after_release")
 *       BEFORE that key's own down, -> IDLE
 *     G physical press (fast re-press before grace ended) -> cancel the
 *       grace timeout, HID stays down (no new edge), direction_seen
 *       reset to false for the resumed hold, -> HELD
 *
 * Note this is NOT a cap on total hold time like gstretch/glatch's
 * MAX_HOLD_MS - there is no such cap here. The grace only ever runs
 * *after* physical release, and only in the no-direction-seen case.
 *
 * Direction-key ordering (G up strictly before LEFT/RIGHT down, in the
 * grace-ended-by-direction case) uses the same wrapper-behavior approach
 * as glatch/glatch_direction: groll_direction's pressed handler calls
 * groll_notice_direction() synchronously before raising its own press
 * event, rather than relying on cross-listener ordering between two
 * independent event subscribers.
 *
 * Only G itself participates (one physical key), so this uses plain
 * static state rather than the per-position slot arrays
 * behavior_rapid_fire.c/behavior_g_stretch.c use for keys bindable at
 * multiple positions.
 */
#define DT_DRV_COMPAT zmk_behavior_g_roll_grace

#include <errno.h>
#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/time_units.h>

#include <drivers/behavior.h>
#include <zmk/behavior.h>
#include <zmk/event_manager.h>
#include <zmk/events/keycode_state_changed.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

enum groll_state {
    GROLL_IDLE = 0,
    GROLL_HELD,
    GROLL_GRACE,
};

static enum groll_state g_state = GROLL_IDLE;
static uint32_t g_encoded_keycode;
static uint32_t g_press_cycles;
static bool g_direction_seen;
static struct k_work_delayable g_grace_work;
/* Persistent, not stack-local - see behavior_rapid_fire.c for why. */
static struct k_work_sync g_grace_sync;

static inline uint32_t groll_elapsed_ms(uint32_t cycles) {
    return k_cyc_to_ms_floor32(k_cycle_get_32() - cycles);
}

static void groll_send_release(void) {
    raise_zmk_keycode_state_changed_from_encoded(g_encoded_keycode, false, k_uptime_get());
    g_state = GROLL_IDLE;
}

static void groll_grace_work_handler(struct k_work *work) {
    ARG_UNUSED(work);

    if (g_state != GROLL_GRACE) {
        /* Cancelled/superseded by a re-press or direction release. */
        return;
    }

    groll_send_release();
}

static int groll_init(const struct device *dev) {
    ARG_UNUSED(dev);
    k_work_init_delayable(&g_grace_work, groll_grace_work_handler);
    return 0;
}

/*
 * Called from groll_direction's pressed handler, before it emits its own
 * press. While G is HELD, this just records that a direction was seen -
 * G is untouched. While G is in its post-release GRACE window, this ends
 * the grace immediately (HID G up) so G's up strictly precedes the
 * direction key's own down. No-op if G is IDLE.
 */
static void groll_notice_direction(uint32_t direction_position) {
    ARG_UNUSED(direction_position);

    if (g_state == GROLL_HELD) {
        g_direction_seen = true;
        return;
    }

    if (g_state == GROLL_GRACE) {
        k_work_cancel_delayable_sync(&g_grace_work, &g_grace_sync);
        groll_send_release();
    }
}

static int on_keymap_binding_pressed(struct zmk_behavior_binding *binding,
                                     struct zmk_behavior_binding_event event) {
    ARG_UNUSED(event);

    if (g_state == GROLL_GRACE) {
        /*
         * Fast re-press before the grace ended. _sync so a handler
         * already mid-fire can't still send the UP edge right after we
         * decide to keep holding - safe here (normal keymap/behavior
         * dispatch context, never groll_grace_work_handler itself). HID
         * is already down, so no new press edge - just resume HELD.
         * direction_seen resets: this is a fresh hold period.
         */
        k_work_cancel_delayable_sync(&g_grace_work, &g_grace_sync);
        g_state = GROLL_HELD;
        g_direction_seen = false;
        return ZMK_BEHAVIOR_OPAQUE;
    }

    if (g_state == GROLL_HELD) {
        /* Physically impossible (key already down) - ignore defensively. */
        return ZMK_BEHAVIOR_OPAQUE;
    }

    g_encoded_keycode = binding->param1;
    g_press_cycles = k_cycle_get_32();
    g_direction_seen = false;
    g_state = GROLL_HELD;
    raise_zmk_keycode_state_changed_from_encoded(g_encoded_keycode, true, k_uptime_get());

    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_keymap_binding_released(struct zmk_behavior_binding *binding,
                                      struct zmk_behavior_binding_event event) {
    ARG_UNUSED(binding);
    ARG_UNUSED(event);

    if (g_state != GROLL_HELD) {
        /* Not physically down from our side (stray release) - ignore. */
        return ZMK_BEHAVIOR_OPAQUE;
    }

    if (g_direction_seen) {
        /* Roll case: direction already happened during the hold. */
        groll_send_release();
        return ZMK_BEHAVIOR_OPAQUE;
    }

    g_state = GROLL_GRACE;
    k_work_schedule(&g_grace_work, K_MSEC(CONFIG_ZMK_G_ROLL_GRACE_MS));

    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api behavior_g_roll_grace_driver_api = {
    .binding_pressed = on_keymap_binding_pressed,
    .binding_released = on_keymap_binding_released,
};

#define GROLL_INST(n)                                                                            \
    BEHAVIOR_DT_INST_DEFINE(n, groll_init, NULL, NULL, NULL, POST_KERNEL,                        \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,                                 \
                            &behavior_g_roll_grace_driver_api);

DT_INST_FOREACH_STATUS_OKAY(GROLL_INST)

#undef DT_DRV_COMPAT
#define DT_DRV_COMPAT zmk_behavior_groll_direction

static int direction_pressed(struct zmk_behavior_binding *binding,
                             struct zmk_behavior_binding_event event) {
    groll_notice_direction(event.position);
    raise_zmk_keycode_state_changed_from_encoded(binding->param1, true, k_uptime_get());
    return ZMK_BEHAVIOR_OPAQUE;
}

static int direction_released(struct zmk_behavior_binding *binding,
                              struct zmk_behavior_binding_event event) {
    ARG_UNUSED(event);
    raise_zmk_keycode_state_changed_from_encoded(binding->param1, false, k_uptime_get());
    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api behavior_groll_direction_driver_api = {
    .binding_pressed = direction_pressed,
    .binding_released = direction_released,
};

#define GROLL_DIR_INST(n)                                                                        \
    BEHAVIOR_DT_INST_DEFINE(n, NULL, NULL, NULL, NULL, POST_KERNEL,                              \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,                                 \
                            &behavior_groll_direction_driver_api);

DT_INST_FOREACH_STATUS_OKAY(GROLL_DIR_INST)
