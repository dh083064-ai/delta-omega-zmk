/*
 * Delta Omega G minimum-hold stretch (experimental, esb-tdma-pcbang-gaming-gstretch).
 *
 * A/B alternative to rapid-fire for a single key: instead of repeated
 * DOWN/UP edges while held, this sends exactly ONE HID press and ONE HID
 * release per physical hold, but stretches a too-short release out so the
 * HID-visible hold duration is never below CONFIG_ZMK_G_STRETCH_MIN_HOLD_MS:
 *
 *   HID hold duration = max(physical hold duration, MIN_HOLD_MS)
 *
 * Press is never delayed - only release ever waits, and only when the
 * physical hold was already shorter than the minimum.
 *
 *   IDLE
 *     physical press -> HID down now, record press timestamp, -> HELD
 *
 *   HELD
 *     physical release, elapsed >= MIN_HOLD_MS -> HID up now, -> IDLE
 *     physical release, elapsed <  MIN_HOLD_MS -> schedule delayed
 *       release for (MIN_HOLD_MS - elapsed), -> RELEASE_PENDING
 *
 *   RELEASE_PENDING
 *     delayed release fires -> HID up, -> IDLE
 *     physical press (fast re-press before the delayed release fired) ->
 *       sync-cancel the pending release, HID stays down (no new edge -
 *       still the same continuous press), -> HELD. The original press
 *       timestamp is kept as-is (not bumped to the re-press moment), so a
 *       later release only needs to wait out whatever's left of the
 *       *original* MIN_HOLD_MS window rather than restarting it - this is
 *       what keeps back-to-back taps from accumulating extra hold time.
 */
#define DT_DRV_COMPAT zmk_behavior_g_stretch

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

#define GSTRETCH_MAX_POSITIONS 64

/*
 * DEBUG-only (temporary, this measurement round): fixed physical
 * positions per this project's convention (combos/gcmd already hardcode
 * positions the same way - see delta_omega.keymap's GAMING row1/row0).
 * G lives at 14, the marker key (Y, plain &kp Y, untouched) at 5.
 */
#define GSTRETCH_G_POSITION 14
#define GSTRETCH_Y_MARKER_POSITION 5

enum gstretch_state {
    GSTRETCH_IDLE = 0,
    GSTRETCH_HELD,
    GSTRETCH_RELEASE_PENDING,
};

struct gstretch_slot {
    struct k_work_delayable release_work;
    /* Persistent, not stack-local - see behavior_rapid_fire.c for why. */
    struct k_work_sync release_sync;
    uint32_t encoded_keycode;
    uint32_t press_cycles;
    enum gstretch_state state;
    /* DEBUG-only: physical hold at the moment release was scheduled, so
     * the delayed-fire log line can report physical vs. HID vs. added. */
    uint32_t debug_physical_elapsed_ms;
};

static struct gstretch_slot gstretch_slots[GSTRETCH_MAX_POSITIONS];

/*
 * DEBUG-only tracking below, all temporary for this measurement round:
 *   - g_release_cycles / g_watching_next_key: armed at G's physical
 *     release, cleared by the first other physical key press after it
 *     (or by a G re-press) - reports how long after G let go the next
 *     real key came in, and whether G's HID was still held at that point.
 *   - g_last_*: the most recently *completed* G session's numbers, so a
 *     marker (Y) press after G has fully released can still report them.
 */
static bool g_watching_next_key;
static uint32_t g_release_cycles;
static bool g_last_valid;
static uint32_t g_last_physical_ms;
static uint32_t g_last_hid_ms;
static uint32_t g_last_added_ms;

static inline uint32_t gstretch_elapsed_ms(uint32_t press_cycles) {
    return k_cyc_to_ms_floor32(k_cycle_get_32() - press_cycles);
}

static void gstretch_arm_release_tracking(void) {
    g_release_cycles = k_cycle_get_32();
    g_watching_next_key = true;
}

static void gstretch_log_marker(void) {
    struct gstretch_slot *g = &gstretch_slots[GSTRETCH_G_POSITION];

    switch (g->state) {
    case GSTRETCH_HELD:
        /* G is physically still down right now - hasn't released yet. */
        LOG_INF("MARKER_Y: G_currently_held=yes, physical_so_far=%ums",
                gstretch_elapsed_ms(g->press_cycles));
        break;
    case GSTRETCH_RELEASE_PENDING: {
        /* Physically released, HID stretch still counting down - the
         * eventual physical/hid/added numbers are already fully known
         * at this point (schedule time fixed them), just not yet logged
         * by gstretch_release_work_handler. */
        uint32_t added_ms = CONFIG_ZMK_G_STRETCH_MIN_HOLD_MS - g->debug_physical_elapsed_ms;
        LOG_INF("MARKER_Y: G_hid_still_held=yes, physical=%ums hid=%ums added=%ums, "
                "since_g_release=%ums",
                g->debug_physical_elapsed_ms, CONFIG_ZMK_G_STRETCH_MIN_HOLD_MS, added_ms,
                gstretch_elapsed_ms(g_release_cycles));
        break;
    }
    case GSTRETCH_IDLE:
    default:
        if (g_last_valid) {
            LOG_INF("MARKER_Y: G_hid_still_held=no, physical=%ums hid=%ums added=%ums, "
                    "since_g_release=%ums",
                    g_last_physical_ms, g_last_hid_ms, g_last_added_ms,
                    gstretch_elapsed_ms(g_release_cycles));
        } else {
            LOG_INF("MARKER_Y: no prior G input this session");
        }
        break;
    }
}

/* Any other physical key press after G's release - see the reasoning in
 * behavior_gaming_cmd.c for why position_state_changed (not the
 * position-less keycode_state_changed our own behaviors raise) is the
 * right event for "did the user physically press something else". */
static int gstretch_position_state_changed_listener(const zmk_event_t *eh) {
    const struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);

    if (ev == NULL || !ev->state) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (ev->position == GSTRETCH_G_POSITION) {
        /* Fresh G session (or repress) starting - stale watch, if any. */
        g_watching_next_key = false;
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (g_watching_next_key) {
        g_watching_next_key = false;
        bool g_hid_still_held = gstretch_slots[GSTRETCH_G_POSITION].state != GSTRETCH_IDLE;
        LOG_INF("next_key: pos=%d +%ums (G_hid_still_held=%s)", ev->position,
                gstretch_elapsed_ms(g_release_cycles), g_hid_still_held ? "yes" : "no");
    }

    if (ev->position == GSTRETCH_Y_MARKER_POSITION) {
        gstretch_log_marker();
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(gstretch_marker, gstretch_position_state_changed_listener);
ZMK_SUBSCRIPTION(gstretch_marker, zmk_position_state_changed);

static void gstretch_send_release(struct gstretch_slot *slot) {
    raise_zmk_keycode_state_changed_from_encoded(slot->encoded_keycode, false, k_uptime_get());
    slot->state = GSTRETCH_IDLE;
}

static void gstretch_release_work_handler(struct k_work *work) {
    struct k_work_delayable *dwork = k_work_delayable_from_work(work);
    struct gstretch_slot *slot = CONTAINER_OF(dwork, struct gstretch_slot, release_work);

    if (slot->state != GSTRETCH_RELEASE_PENDING) {
        /* Cancelled/superseded by a re-press between scheduling and firing. */
        return;
    }

    g_last_physical_ms = slot->debug_physical_elapsed_ms;
    g_last_hid_ms = CONFIG_ZMK_G_STRETCH_MIN_HOLD_MS;
    g_last_added_ms = CONFIG_ZMK_G_STRETCH_MIN_HOLD_MS - slot->debug_physical_elapsed_ms;
    g_last_valid = true;
    LOG_INF("gstretch: physical=%ums hid=%ums added=%ums", g_last_physical_ms, g_last_hid_ms,
            g_last_added_ms);
    gstretch_send_release(slot);
}

static int gstretch_init(const struct device *dev) {
    ARG_UNUSED(dev);

    for (int i = 0; i < GSTRETCH_MAX_POSITIONS; i++) {
        k_work_init_delayable(&gstretch_slots[i].release_work, gstretch_release_work_handler);
    }

    return 0;
}

static int on_keymap_binding_pressed(struct zmk_behavior_binding *binding,
                                     struct zmk_behavior_binding_event event) {
    if (event.position >= GSTRETCH_MAX_POSITIONS) {
        return -EINVAL;
    }

    struct gstretch_slot *slot = &gstretch_slots[event.position];

    if (slot->state == GSTRETCH_RELEASE_PENDING) {
        /*
         * Fast re-press before the stretched release fired. Cancel it -
         * _sync so a handler already mid-fire can't still send the UP
         * edge right after we decide to keep holding (self-cancel is
         * never a risk here: this call site is the normal keymap/behavior
         * dispatch path, never gstretch_release_work_handler itself).
         * HID is already down, so no new press edge - just resume HELD.
         * press_cycles is deliberately left untouched (see file header).
         */
        k_work_cancel_delayable_sync(&slot->release_work, &slot->release_sync);
        slot->state = GSTRETCH_HELD;
        LOG_INF("gstretch: pending release cancelled by repress (pos %d)", event.position);
        return ZMK_BEHAVIOR_OPAQUE;
    }

    if (slot->state == GSTRETCH_HELD) {
        /* Physically impossible (key already down) - ignore defensively. */
        return ZMK_BEHAVIOR_OPAQUE;
    }

    slot->encoded_keycode = binding->param1;
    slot->press_cycles = k_cycle_get_32();
    slot->state = GSTRETCH_HELD;
    raise_zmk_keycode_state_changed_from_encoded(slot->encoded_keycode, true, k_uptime_get());

    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_keymap_binding_released(struct zmk_behavior_binding *binding,
                                      struct zmk_behavior_binding_event event) {
    ARG_UNUSED(binding);

    if (event.position >= GSTRETCH_MAX_POSITIONS) {
        return -EINVAL;
    }

    struct gstretch_slot *slot = &gstretch_slots[event.position];

    if (slot->state != GSTRETCH_HELD) {
        /* Not physically down from our side (stray release) - ignore. */
        return ZMK_BEHAVIOR_OPAQUE;
    }

    uint32_t elapsed_ms = gstretch_elapsed_ms(slot->press_cycles);

    if (event.position == GSTRETCH_G_POSITION) {
        gstretch_arm_release_tracking();
    }

    if (elapsed_ms >= CONFIG_ZMK_G_STRETCH_MIN_HOLD_MS) {
        g_last_physical_ms = elapsed_ms;
        g_last_hid_ms = elapsed_ms;
        g_last_added_ms = 0;
        g_last_valid = true;
        LOG_INF("gstretch: physical=%ums hid=%ums added=0ms", elapsed_ms, elapsed_ms);
        gstretch_send_release(slot);
        return ZMK_BEHAVIOR_OPAQUE;
    }

    slot->debug_physical_elapsed_ms = elapsed_ms;
    slot->state = GSTRETCH_RELEASE_PENDING;
    k_work_schedule(&slot->release_work, K_MSEC(CONFIG_ZMK_G_STRETCH_MIN_HOLD_MS - elapsed_ms));

    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api behavior_g_stretch_driver_api = {
    .binding_pressed = on_keymap_binding_pressed,
    .binding_released = on_keymap_binding_released,
};

#define GSTRETCH_INST(n)                                                                          \
    BEHAVIOR_DT_INST_DEFINE(n, gstretch_init, NULL, NULL, NULL, POST_KERNEL,                     \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &behavior_g_stretch_driver_api);

DT_INST_FOREACH_STATUS_OKAY(GSTRETCH_INST)
