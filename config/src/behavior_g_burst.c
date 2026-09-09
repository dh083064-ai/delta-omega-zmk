/*
 * Delta Omega G burst (experimental, esb-tdma-pcbang-gaming-gburst).
 *
 * v1: the most conservative possible A/B change from plain rapid-fire -
 * everything about the repeat itself is identical (same
 * CONFIG_ZMK_RAPID_FIRE_MIN_INTERVAL_MS/MAX_INTERVAL_MS/TAP_MS, not
 * redefined here, just read as-is), the ONLY difference is a hard cap on
 * how long the repeat cycle is allowed to run from the physical press:
 *
 *   burst_runtime_ms = min(physical_hold_ms, CONFIG_ZMK_G_BURST_MAX_MS)
 *
 * Physical release always stops the burst immediately, exactly like
 * plain rapid-fire - v1 adds no grace, no anchor, no direction-key
 * interaction of any kind. Direction keys (LEFT/RIGHT) and every other
 * key are completely untouched and have zero effect on this behavior;
 * sequence-aware termination is deliberately deferred to a later
 * iteration rather than combined with the timing cap in one step.
 *
 * Physical hold longer than MAX_MS doesn't change what's sent - it just
 * stops the repeat early and lets the physical release (still watched
 * for) fall through as a no-op once the cap has already ended things.
 *
 * Only G participates (one physical key bound to this behavior), so this
 * uses plain static state rather than the per-position slot arrays
 * behavior_rapid_fire.c uses for keys bindable at multiple positions.
 * The k_work_cancel_delayable_sync + persistent k_work_sync race-safety
 * pattern is carried over unchanged from the validated rapid-fire fix.
 */
#define DT_DRV_COMPAT zmk_behavior_g_burst

#include <errno.h>
#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/random/random.h>
#include <zephyr/sys/time_units.h>

#include <drivers/behavior.h>
#include <zmk/behavior.h>
#include <zmk/event_manager.h>
#include <zmk/events/keycode_state_changed.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

static bool g_active;
static bool g_virtual_pressed;
static uint32_t g_encoded_keycode;
static uint32_t g_press_cycles;
static uint32_t g_retry_count;

static struct k_work_delayable g_repeat_work;
static struct k_work_delayable g_release_work;
static struct k_work_delayable g_cap_work;
/* Persistent, not stack-local - see behavior_rapid_fire.c for why. */
static struct k_work_sync g_repeat_sync;
static struct k_work_sync g_release_sync;
static struct k_work_sync g_cap_sync;

static inline uint32_t gburst_elapsed_ms(uint32_t cycles) {
    return k_cyc_to_ms_floor32(k_cycle_get_32() - cycles);
}

/* Same formula as behavior_rapid_fire.c's rf_next_interval_ms(), reading
 * the identical Kconfig symbols - duplicated rather than shared so this
 * file never has to touch behavior_rapid_fire.c. */
static uint32_t gburst_next_interval_ms(void) {
    const uint32_t min_ms = CONFIG_ZMK_RAPID_FIRE_MIN_INTERVAL_MS;
    const uint32_t max_ms = CONFIG_ZMK_RAPID_FIRE_MAX_INTERVAL_MS;

    if (max_ms <= min_ms) {
        return min_ms;
    }

    return min_ms + (sys_rand32_get() % (max_ms - min_ms + 1U));
}

static void gburst_send_press(void) {
    if (g_virtual_pressed || !g_active) {
        return;
    }
    g_virtual_pressed = true;
    raise_zmk_keycode_state_changed_from_encoded(g_encoded_keycode, true, k_uptime_get());
}

static void gburst_send_release(void) {
    if (!g_virtual_pressed) {
        return;
    }
    g_virtual_pressed = false;
    raise_zmk_keycode_state_changed_from_encoded(g_encoded_keycode, false, k_uptime_get());
}

static void gburst_stop(void) {
    g_active = false;
    k_work_cancel_delayable_sync(&g_repeat_work, &g_repeat_sync);
    k_work_cancel_delayable_sync(&g_release_work, &g_release_sync);
    k_work_cancel_delayable_sync(&g_cap_work, &g_cap_sync);
    gburst_send_release();
}

static void gburst_release_work_handler(struct k_work *work) {
    ARG_UNUSED(work);
    gburst_send_release();
}

static void gburst_repeat_work_handler(struct k_work *work) {
    ARG_UNUSED(work);

    if (!g_active) {
        return;
    }

    gburst_send_release();
    gburst_send_press();
    g_retry_count++;
    k_work_schedule(&g_release_work, K_MSEC(CONFIG_ZMK_RAPID_FIRE_TAP_MS));
    k_work_schedule(&g_repeat_work, K_MSEC(gburst_next_interval_ms()));
}

static void gburst_cap_work_handler(struct k_work *work) {
    ARG_UNUSED(work);

    if (!g_active) {
        /* Already stopped by physical release. */
        return;
    }

    gburst_stop();
}

static int gburst_init(const struct device *dev) {
    ARG_UNUSED(dev);
    k_work_init_delayable(&g_repeat_work, gburst_repeat_work_handler);
    k_work_init_delayable(&g_release_work, gburst_release_work_handler);
    k_work_init_delayable(&g_cap_work, gburst_cap_work_handler);
    return 0;
}

static int on_keymap_binding_pressed(struct zmk_behavior_binding *binding,
                                     struct zmk_behavior_binding_event event) {
    ARG_UNUSED(event);

    /*
     * Same up-front sync-cancel-then-reset as behavior_rapid_fire.c's
     * on_keymap_binding_pressed, for a fast repress: guarantees no stale
     * repeat/release/cap handler from a previous session can touch this
     * new one after this call returns.
     */
    k_work_cancel_delayable_sync(&g_repeat_work, &g_repeat_sync);
    k_work_cancel_delayable_sync(&g_release_work, &g_release_sync);
    k_work_cancel_delayable_sync(&g_cap_work, &g_cap_sync);
    gburst_send_release();

    g_encoded_keycode = binding->param1;
    g_press_cycles = k_cycle_get_32();
    g_retry_count = 0;
    g_active = true;

    gburst_send_press();
    k_work_schedule(&g_release_work, K_MSEC(CONFIG_ZMK_RAPID_FIRE_TAP_MS));
    k_work_schedule(&g_repeat_work, K_MSEC(gburst_next_interval_ms()));
    k_work_schedule(&g_cap_work, K_MSEC(CONFIG_ZMK_G_BURST_MAX_MS));

    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_keymap_binding_released(struct zmk_behavior_binding *binding,
                                      struct zmk_behavior_binding_event event) {
    ARG_UNUSED(binding);
    ARG_UNUSED(event);

    if (!g_active) {
        /* Already stopped by the max-burst cap - nothing to do. */
        return ZMK_BEHAVIOR_OPAQUE;
    }

    gburst_stop();

    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api behavior_g_burst_driver_api = {
    .binding_pressed = on_keymap_binding_pressed,
    .binding_released = on_keymap_binding_released,
};

#define GBURST_INST(n)                                                                           \
    BEHAVIOR_DT_INST_DEFINE(n, gburst_init, NULL, NULL, NULL, POST_KERNEL,                       \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &behavior_g_burst_driver_api);

DT_INST_FOREACH_STATUS_OKAY(GBURST_INST)
