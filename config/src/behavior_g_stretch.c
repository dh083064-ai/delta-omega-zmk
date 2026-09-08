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

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define GSTRETCH_MAX_POSITIONS 64

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
};

static struct gstretch_slot gstretch_slots[GSTRETCH_MAX_POSITIONS];

static inline uint32_t gstretch_elapsed_ms(uint32_t press_cycles) {
    return k_cyc_to_ms_floor32(k_cycle_get_32() - press_cycles);
}

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
    if (elapsed_ms >= CONFIG_ZMK_G_STRETCH_MIN_HOLD_MS) {
        gstretch_send_release(slot);
        return ZMK_BEHAVIOR_OPAQUE;
    }

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
