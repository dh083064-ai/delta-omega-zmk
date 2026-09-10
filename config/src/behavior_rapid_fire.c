/*
 * Delta Omega rapid-fire behavior for ZMK v0.3.
 *
 * High-duty-cycle turbo: physical hold => the virtual HID key spends
 * nearly all of each cycle DOWN and only pulses UP for
 * CONFIG_ZMK_RAPID_FIRE_TAP_MS (now a *break* width, see below) right
 * before the next DOWN, so it reads as "mostly held" in-game while
 * still re-triggering the game's own key-repeat/rate-of-fire at a
 * fresh randomized DOWN-to-DOWN interval
 * (CONFIG_ZMK_RAPID_FIRE_MIN/MAX_INTERVAL_MS) each cycle. This is the
 * same duty-cycle shape just verified on real hardware on the QMK port
 * of this behavior (lunakey-pico-qmk) - the structure below (per-
 * position slot table, two k_work_delayable items, sync-cancel on
 * press/release) is unchanged from the previous stable version; only
 * what each work item schedules changes. Deliberately NOT built on the
 * esb-tdma-pcbang-gaming-gburst branch's rapid-fire - that one is a
 * separate, previously-abandoned structure and is not touched here.
 */
#define DT_DRV_COMPAT zmk_behavior_rapid_fire

#include <errno.h>
#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/random/random.h>

#include <drivers/behavior.h>
#include <zmk/behavior.h>
#include <zmk/event_manager.h>
#include <zmk/events/keycode_state_changed.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define RF_MAX_POSITIONS 64

struct rf_slot {
    /* Fires when the current DOWN phase's time is up - releases and
     * starts the break. */
    struct k_work_delayable up_work;
    /* Fires CONFIG_ZMK_RAPID_FIRE_TAP_MS after up_work - draws a fresh
     * interval and presses again, starting the next cycle's DOWN
     * phase. */
    struct k_work_delayable down_work;
    /*
     * Dedicated, persistent storage for each work item's sync-cancel -
     * Zephyr requires this to outlive the call and never be shared
     * between concurrent cancel/flush operations, so it can't be a
     * function-local stack variable.
     */
    struct k_work_sync up_sync;
    struct k_work_sync down_sync;
    uint32_t encoded_keycode;
    bool active;
    bool virtual_pressed;
};

static struct rf_slot rf_slots[RF_MAX_POSITIONS];

static uint32_t rf_next_interval_ms(void) {
    const uint32_t min_ms = CONFIG_ZMK_RAPID_FIRE_MIN_INTERVAL_MS;
    const uint32_t max_ms = CONFIG_ZMK_RAPID_FIRE_MAX_INTERVAL_MS;

    if (max_ms <= min_ms) {
        return min_ms;
    }

    /* Inclusive range: [min_ms, max_ms]. */
    return min_ms + (sys_rand32_get() % (max_ms - min_ms + 1U));
}

static void rf_send_press(struct rf_slot *slot) {
    if (slot->virtual_pressed || !slot->active) {
        return;
    }
    slot->virtual_pressed = true;
    raise_zmk_keycode_state_changed_from_encoded(slot->encoded_keycode, true, k_uptime_get());
}

static void rf_send_release(struct rf_slot *slot) {
    if (!slot->virtual_pressed) {
        return;
    }
    slot->virtual_pressed = false;
    raise_zmk_keycode_state_changed_from_encoded(slot->encoded_keycode, false, k_uptime_get());
}

/* Down-phase timer expired: release (start of the break pulse), then
 * schedule down_work to end the break CONFIG_ZMK_RAPID_FIRE_TAP_MS
 * later. Deliberately does NOT re-press here - that's down_work's job,
 * kept as its own separate work item/handler so the UP edge always
 * gets its own dispatch before the next DOWN is even considered. */
static void rf_up_work_handler(struct k_work *work) {
    struct k_work_delayable *dwork = k_work_delayable_from_work(work);
    struct rf_slot *slot = CONTAINER_OF(dwork, struct rf_slot, up_work);

    if (!slot->active) {
        return;
    }

    rf_send_release(slot);
    k_work_schedule(&slot->down_work, K_MSEC(CONFIG_ZMK_RAPID_FIRE_TAP_MS));
}

/* Break-phase timer expired: draw a fresh 20-26ms interval for the new
 * cycle, press again, and schedule up_work for (interval - break) ms
 * from now so the DOWN phase occupies the rest of that interval. */
static void rf_down_work_handler(struct k_work *work) {
    struct k_work_delayable *dwork = k_work_delayable_from_work(work);
    struct rf_slot *slot = CONTAINER_OF(dwork, struct rf_slot, down_work);

    if (!slot->active) {
        return;
    }

    uint32_t interval_ms = rf_next_interval_ms();
    uint32_t down_ms = (interval_ms > CONFIG_ZMK_RAPID_FIRE_TAP_MS) ? (interval_ms - CONFIG_ZMK_RAPID_FIRE_TAP_MS) : 1U;

    rf_send_press(slot);
    k_work_schedule(&slot->up_work, K_MSEC(down_ms));
}

static int rapid_fire_init(const struct device *dev) {
    ARG_UNUSED(dev);

    for (int i = 0; i < RF_MAX_POSITIONS; i++) {
        k_work_init_delayable(&rf_slots[i].up_work, rf_up_work_handler);
        k_work_init_delayable(&rf_slots[i].down_work, rf_down_work_handler);
    }

    return 0;
}

static int on_keymap_binding_pressed(struct zmk_behavior_binding *binding,
                                     struct zmk_behavior_binding_event event) {
    if (event.position >= RF_MAX_POSITIONS) {
        return -EINVAL;
    }

    struct rf_slot *slot = &rf_slots[event.position];

    /*
     * _sync blocks until any in-flight handler actually finishes, unlike
     * plain cancel_delayable() (which per Zephyr's own docs may return
     * while the handler is still mid-execution on another context). Without
     * this, a stale up_work/down_work that was already running when we
     * cancel it could still fire afterward, see virtual_pressed from THIS
     * new press, and desync the DOWN/UP edges - or inject an extra cycle
     * into the new session. Safe here because this runs in normal thread
     * context (the keymap/behavior dispatch path), never inside
     * rf_up_work_handler/rf_down_work_handler themselves - a work item
     * must never sync-cancel itself.
     */
    k_work_cancel_delayable_sync(&slot->up_work, &slot->up_sync);
    k_work_cancel_delayable_sync(&slot->down_work, &slot->down_sync);
    rf_send_release(slot);

    slot->encoded_keycode = binding->param1;
    slot->active = true;

    uint32_t interval_ms = rf_next_interval_ms();
    uint32_t down_ms = (interval_ms > CONFIG_ZMK_RAPID_FIRE_TAP_MS) ? (interval_ms - CONFIG_ZMK_RAPID_FIRE_TAP_MS) : 1U;

    rf_send_press(slot);
    k_work_schedule(&slot->up_work, K_MSEC(down_ms));

    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_keymap_binding_released(struct zmk_behavior_binding *binding,
                                      struct zmk_behavior_binding_event event) {
    ARG_UNUSED(binding);

    if (event.position >= RF_MAX_POSITIONS) {
        return -EINVAL;
    }

    struct rf_slot *slot = &rf_slots[event.position];

    slot->active = false;
    k_work_cancel_delayable_sync(&slot->up_work, &slot->up_sync);
    k_work_cancel_delayable_sync(&slot->down_work, &slot->down_sync);
    rf_send_release(slot);

    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api behavior_rapid_fire_driver_api = {
    .binding_pressed = on_keymap_binding_pressed,
    .binding_released = on_keymap_binding_released,
};

#define RF_INST(n)                                                                                 \
    BEHAVIOR_DT_INST_DEFINE(n, rapid_fire_init, NULL, NULL, NULL, POST_KERNEL,                    \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &behavior_rapid_fire_driver_api);

DT_INST_FOREACH_STATUS_OKAY(RF_INST)
