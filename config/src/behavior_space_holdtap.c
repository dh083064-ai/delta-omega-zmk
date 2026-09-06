/*
 * Delta Omega space hold-tap with chatter tolerance.
 *
 * A from-scratch hold-tap for a single flaky switch (Space). Two problems
 * with a stock zmk,behavior-hold-tap here, both caused by physical switch
 * chatter that the global kscan debounce is intentionally kept low for
 * (every other key wants the fast debounce):
 *
 *   1. Tap side: a bounce (brief open/close while "held") can be seen as
 *      several independent press+release cycles, each resolving as its
 *      own tap -> repeated Space characters. Fixed with a per-position
 *      cooldown: a tap within CONFIG_ZMK_DEBOUNCE_TAP_COOLDOWN_MS of the
 *      last real one is ignored.
 *
 *   2. Hold side: once the hold has resolved and the layer is active, a
 *      chatter blip mid-hold looks exactly like a genuine physical
 *      release, so the layer would immediately deactivate even though
 *      the finger never left the key. Fixed with a release grace period:
 *      on release while the hold is active, don't deactivate the layer
 *      immediately - wait CONFIG_ZMK_SPACE_HT_RELEASE_GRACE_MS, and if a
 *      new press on the same position arrives before that, treat it as
 *      the same continuous hold (cancel the pending deactivation).
 */
#define DT_DRV_COMPAT zmk_behavior_space_holdtap

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <drivers/behavior.h>
#include <zmk/behavior.h>
#include <zmk/event_manager.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/keymap.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define SHT_MAX_POSITIONS 64

enum sht_state {
    SHT_IDLE,
    SHT_PENDING,     /* physically down, still within tapping-term */
    SHT_HOLD_ACTIVE, /* resolved hold, layer is on */
    SHT_HOLD_GRACE,  /* released while hold-active, waiting to see if it was a bounce */
};

struct sht_slot {
    struct k_work_delayable term_work;
    struct k_work_delayable grace_work;
    struct k_work_delayable tap_release_work;
    enum sht_state state;
    uint32_t encoded_keycode;
    uint8_t layer;
    int64_t last_tap_fire;
    bool tap_virtual_pressed;
};

static struct sht_slot sht_slots[SHT_MAX_POSITIONS];

static void sht_term_work_handler(struct k_work *work) {
    struct k_work_delayable *dwork = k_work_delayable_from_work(work);
    struct sht_slot *slot = CONTAINER_OF(dwork, struct sht_slot, term_work);

    if (slot->state != SHT_PENDING) {
        return;
    }

    slot->state = SHT_HOLD_ACTIVE;
    zmk_keymap_layer_activate(slot->layer);
}

static void sht_grace_work_handler(struct k_work *work) {
    struct k_work_delayable *dwork = k_work_delayable_from_work(work);
    struct sht_slot *slot = CONTAINER_OF(dwork, struct sht_slot, grace_work);

    if (slot->state != SHT_HOLD_GRACE) {
        return;
    }

    slot->state = SHT_IDLE;
    zmk_keymap_layer_deactivate(slot->layer);
}

static void sht_tap_release_work_handler(struct k_work *work) {
    struct k_work_delayable *dwork = k_work_delayable_from_work(work);
    struct sht_slot *slot = CONTAINER_OF(dwork, struct sht_slot, tap_release_work);

    if (!slot->tap_virtual_pressed) {
        return;
    }

    slot->tap_virtual_pressed = false;
    raise_zmk_keycode_state_changed_from_encoded(slot->encoded_keycode, false, k_uptime_get());
}

static int sht_init(const struct device *dev) {
    ARG_UNUSED(dev);

    for (int i = 0; i < SHT_MAX_POSITIONS; i++) {
        k_work_init_delayable(&sht_slots[i].term_work, sht_term_work_handler);
        k_work_init_delayable(&sht_slots[i].grace_work, sht_grace_work_handler);
        k_work_init_delayable(&sht_slots[i].tap_release_work, sht_tap_release_work_handler);
    }

    return 0;
}

static int on_keymap_binding_pressed(struct zmk_behavior_binding *binding,
                                     struct zmk_behavior_binding_event event) {
    if (event.position >= SHT_MAX_POSITIONS) {
        return -EINVAL;
    }

    struct sht_slot *slot = &sht_slots[event.position];
    slot->layer = (uint8_t)binding->param1;
    slot->encoded_keycode = binding->param2;

    switch (slot->state) {
    case SHT_HOLD_GRACE:
        /* Bounce absorbed - the hold never really stopped. */
        k_work_cancel_delayable(&slot->grace_work);
        slot->state = SHT_HOLD_ACTIVE;
        break;
    case SHT_IDLE:
        slot->state = SHT_PENDING;
        k_work_schedule(&slot->term_work, K_MSEC(CONFIG_ZMK_SPACE_HT_TAPPING_TERM_MS));
        break;
    case SHT_PENDING:
    case SHT_HOLD_ACTIVE:
    default:
        /* Stray extra press with no matching release yet - ignore. */
        break;
    }

    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_keymap_binding_released(struct zmk_behavior_binding *binding,
                                      struct zmk_behavior_binding_event event) {
    ARG_UNUSED(binding);

    if (event.position >= SHT_MAX_POSITIONS) {
        return -EINVAL;
    }

    struct sht_slot *slot = &sht_slots[event.position];

    switch (slot->state) {
    case SHT_PENDING: {
        k_work_cancel_delayable(&slot->term_work);
        slot->state = SHT_IDLE;

        int64_t now = k_uptime_get();
        if (now - slot->last_tap_fire < CONFIG_ZMK_DEBOUNCE_TAP_COOLDOWN_MS) {
            /* Chatter-driven duplicate tap - ignore. */
            break;
        }
        slot->last_tap_fire = now;
        slot->tap_virtual_pressed = true;
        raise_zmk_keycode_state_changed_from_encoded(slot->encoded_keycode, true, now);
        k_work_schedule(&slot->tap_release_work, K_MSEC(CONFIG_ZMK_DEBOUNCE_TAP_MS));
        break;
    }
    case SHT_HOLD_ACTIVE:
        slot->state = SHT_HOLD_GRACE;
        k_work_schedule(&slot->grace_work, K_MSEC(CONFIG_ZMK_SPACE_HT_RELEASE_GRACE_MS));
        break;
    case SHT_HOLD_GRACE:
    case SHT_IDLE:
    default:
        break;
    }

    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api behavior_space_holdtap_driver_api = {
    .binding_pressed = on_keymap_binding_pressed,
    .binding_released = on_keymap_binding_released,
};

#define SHT_INST(n)                                                                               \
    BEHAVIOR_DT_INST_DEFINE(n, sht_init, NULL, NULL, NULL, POST_KERNEL,                          \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &behavior_space_holdtap_driver_api);

DT_INST_FOREACH_STATUS_OKAY(SHT_INST)
