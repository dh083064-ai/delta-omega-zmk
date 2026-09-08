/*
 * Delta Omega G latch (experimental, esb-tdma-pcbang-gaming-glatch).
 *
 * A/B alternative to rapid-fire for a single key: G's HID press is never
 * repeated (no DOWN/UP/DOWN/UP edges), and physical release does NOT
 * immediately release the HID key either - it latches, staying HID-down
 * until either a direction key (LEFT/RIGHT) ends it, or a maximum total
 * hold time is reached, whichever comes first:
 *
 *   IDLE
 *     physical press -> HID down now, record press timestamp, -> HELD
 *
 *   HELD
 *     physical release, elapsed >= MAX_HOLD_MS -> HID up now
 *       (reason: the physical hold alone already used up the whole
 *       budget - "physical_long_hold"), -> IDLE
 *     physical release, elapsed <  MAX_HOLD_MS -> schedule a timeout for
 *       (MAX_HOLD_MS - elapsed) - i.e. MAX_HOLD_MS is a cap on *total*
 *       hold time measured from the physical press, not extra time added
 *       after release - -> LATCHED
 *
 *   LATCHED
 *     timeout fires -> HID up (reason: "timeout"), -> IDLE
 *     LEFT/RIGHT is freshly pressed (glatch_direction, see below) -> HID
 *       up (reason: "direction") BEFORE that key's own down, -> IDLE
 *     G physical press (fast re-press before the latch ended) -> cancel
 *       the pending timeout, HID stays down (no new edge), -> HELD. The
 *       original press timestamp is kept (not bumped), so total hold
 *       time is still capped from the *original* press.
 *
 * Direction-key ordering (G up strictly before LEFT/RIGHT down) is
 * handled by making LEFT/RIGHT use the glatch_direction behavior below
 * instead of plain &kp - its pressed handler calls glatch_release_latch()
 * synchronously before raising its own press event, rather than relying
 * on cross-listener ordering between two independent event subscribers.
 * glatch_direction otherwise behaves exactly like &kp for its own
 * keycode, and is a no-op passthrough when G isn't latched.
 *
 * Only G itself participates in the state machine (one physical key), so
 * this uses plain static state rather than the per-position slot arrays
 * behavior_rapid_fire.c/behavior_g_stretch.c use for keys that can be
 * bound at multiple positions.
 */
#define DT_DRV_COMPAT zmk_behavior_g_latch

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

enum glatch_state {
    GLATCH_IDLE = 0,
    GLATCH_HELD,
    GLATCH_LATCHED,
};

static enum glatch_state g_state = GLATCH_IDLE;
static uint32_t g_encoded_keycode;
static uint32_t g_press_cycles;
/* Physical hold duration, fixed the moment physical release starts the
 * latch (HELD -> LATCHED) - the timeout is scheduled off this, and it's
 * kept around so a later direction-triggered release can still report
 * "physical" alongside the final "hid"/"added" numbers. */
static uint32_t g_physical_elapsed_ms;
/* DEBUG-only: cycle timestamp of the physical release (HELD->LATCHED),
 * so a direction-triggered release can report time-since-physical-release. */
static uint32_t g_release_cycles;
static struct k_work_delayable g_timeout_work;
/* Persistent, not stack-local - see behavior_rapid_fire.c for why. */
static struct k_work_sync g_timeout_sync;

static inline uint32_t glatch_elapsed_ms(uint32_t cycles) {
    return k_cyc_to_ms_floor32(k_cycle_get_32() - cycles);
}

static void glatch_send_release(void) {
    raise_zmk_keycode_state_changed_from_encoded(g_encoded_keycode, false, k_uptime_get());
    g_state = GLATCH_IDLE;
}

static void glatch_timeout_handler(struct k_work *work) {
    ARG_UNUSED(work);

    if (g_state != GLATCH_LATCHED) {
        /* Cancelled/superseded by a re-press or direction release. */
        return;
    }

    uint32_t hid_ms = CONFIG_ZMK_G_LATCH_MAX_HOLD_MS;
    LOG_INF("glatch: physical=%ums hid=%ums added=%ums reason=timeout", g_physical_elapsed_ms,
            hid_ms, hid_ms - g_physical_elapsed_ms);
    glatch_send_release();
}

static int glatch_init(const struct device *dev) {
    ARG_UNUSED(dev);
    k_work_init_delayable(&g_timeout_work, glatch_timeout_handler);
    return 0;
}

/*
 * Called from glatch_direction's pressed handler, before it emits its own
 * press - ends an active latch so G's HID up always precedes the
 * direction key's HID down. No-op if G isn't currently latched (e.g.
 * still physically held, or already idle).
 */
static void glatch_release_by_direction(uint32_t direction_position) {
    if (g_state != GLATCH_LATCHED) {
        return;
    }

    k_work_cancel_delayable_sync(&g_timeout_work, &g_timeout_sync);

    uint32_t hid_ms = glatch_elapsed_ms(g_press_cycles);
    LOG_INF("glatch: physical=%ums hid=%ums added=%ums reason=direction pos=%d "
            "direction_after_physical_release_ms=%ums",
            g_physical_elapsed_ms, hid_ms, hid_ms - g_physical_elapsed_ms, direction_position,
            glatch_elapsed_ms(g_release_cycles));
    glatch_send_release();
}

static int on_keymap_binding_pressed(struct zmk_behavior_binding *binding,
                                     struct zmk_behavior_binding_event event) {
    ARG_UNUSED(event);

    if (g_state == GLATCH_LATCHED) {
        /*
         * Fast re-press before the latch timed out. _sync so a handler
         * already mid-fire can't still send the UP edge right after we
         * decide to keep holding - safe here (normal keymap/behavior
         * dispatch context, never glatch_timeout_handler itself).
         * HID is already down, so no new press edge - just resume HELD.
         * g_physical_elapsed_ms/g_press_cycles are deliberately left
         * untouched, so the total-hold cap still counts from the
         * original press.
         */
        k_work_cancel_delayable_sync(&g_timeout_work, &g_timeout_sync);
        g_state = GLATCH_HELD;
        LOG_INF("glatch: pending timeout cancelled by repress");
        return ZMK_BEHAVIOR_OPAQUE;
    }

    if (g_state == GLATCH_HELD) {
        /* Physically impossible (key already down) - ignore defensively. */
        return ZMK_BEHAVIOR_OPAQUE;
    }

    g_encoded_keycode = binding->param1;
    g_press_cycles = k_cycle_get_32();
    g_state = GLATCH_HELD;
    raise_zmk_keycode_state_changed_from_encoded(g_encoded_keycode, true, k_uptime_get());

    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_keymap_binding_released(struct zmk_behavior_binding *binding,
                                      struct zmk_behavior_binding_event event) {
    ARG_UNUSED(binding);
    ARG_UNUSED(event);

    if (g_state != GLATCH_HELD) {
        /* Not physically down from our side (stray release) - ignore. */
        return ZMK_BEHAVIOR_OPAQUE;
    }

    uint32_t elapsed_ms = glatch_elapsed_ms(g_press_cycles);

    if (elapsed_ms >= CONFIG_ZMK_G_LATCH_MAX_HOLD_MS) {
        LOG_INF("glatch: physical=%ums hid=%ums added=0ms reason=physical_long_hold", elapsed_ms,
                elapsed_ms);
        glatch_send_release();
        return ZMK_BEHAVIOR_OPAQUE;
    }

    g_physical_elapsed_ms = elapsed_ms;
    g_release_cycles = k_cycle_get_32();
    g_state = GLATCH_LATCHED;
    k_work_schedule(&g_timeout_work, K_MSEC(CONFIG_ZMK_G_LATCH_MAX_HOLD_MS - elapsed_ms));

    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api behavior_g_latch_driver_api = {
    .binding_pressed = on_keymap_binding_pressed,
    .binding_released = on_keymap_binding_released,
};

#define GLATCH_INST(n)                                                                           \
    BEHAVIOR_DT_INST_DEFINE(n, glatch_init, NULL, NULL, NULL, POST_KERNEL,                       \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &behavior_g_latch_driver_api);

DT_INST_FOREACH_STATUS_OKAY(GLATCH_INST)

#undef DT_DRV_COMPAT
#define DT_DRV_COMPAT zmk_behavior_glatch_direction

static int direction_pressed(struct zmk_behavior_binding *binding,
                             struct zmk_behavior_binding_event event) {
    glatch_release_by_direction(event.position);
    raise_zmk_keycode_state_changed_from_encoded(binding->param1, true, k_uptime_get());
    return ZMK_BEHAVIOR_OPAQUE;
}

static int direction_released(struct zmk_behavior_binding *binding,
                              struct zmk_behavior_binding_event event) {
    ARG_UNUSED(event);
    raise_zmk_keycode_state_changed_from_encoded(binding->param1, false, k_uptime_get());
    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api behavior_glatch_direction_driver_api = {
    .binding_pressed = direction_pressed,
    .binding_released = direction_released,
};

#define GLATCH_DIR_INST(n)                                                                       \
    BEHAVIOR_DT_INST_DEFINE(n, NULL, NULL, NULL, NULL, POST_KERNEL,                              \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,                                 \
                            &behavior_glatch_direction_driver_api);

DT_INST_FOREACH_STATUS_OKAY(GLATCH_DIR_INST)
