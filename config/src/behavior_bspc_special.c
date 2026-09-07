/*
 * Delta Omega Backspace special: a from-scratch 3-function key.
 *
 *   1. Quick tap                    -> one Backspace.
 *   2. Held (from a cold start, or  -> Backspace held down (the host's own
 *      as the second press after a     key-repeat then deletes repeatedly,
 *      tap)                            same as a stock keyboard).
 *   3. Tap, then a second quick tap -> Caps Word.
 *
 * Backspace fires immediately on the first tap (no artificial delay), since
 * that's by far the most common case. The second-press logic only kicks in
 * if another press follows within CONFIG_ZMK_BSPC_SPECIAL_FOLLOWUP_MS of the
 * first tap's release.
 */
#define DT_DRV_COMPAT zmk_behavior_bspc_special

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <drivers/behavior.h>
#include <dt-bindings/zmk/keys.h>
#include <zmk/behavior.h>
#include <zmk/event_manager.h>
#include <zmk/events/keycode_state_changed.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define BSPC_MAX_POSITIONS 64

enum bspc_state {
    BSPC_IDLE,
    BSPC_P1_PENDING,     /* first press, still within the hold threshold */
    BSPC_AFTER_TAP_WAIT, /* first tap already fired Bspc; watching for a follow-up press */
    BSPC_P2_PENDING,     /* second press, still within the hold threshold */
    BSPC_REPEAT_ACTIVE,  /* resolved as a hold (cold or after a tap): Bspc is held down */
};

enum bspc_pending_release {
    BSPC_RELEASE_NONE,
    BSPC_RELEASE_TAP,
    BSPC_RELEASE_CAPS_WORD,
};

struct bspc_slot {
    struct k_work_delayable term_work;
    struct k_work_delayable followup_work;
    struct k_work_delayable virtual_release_work;
    enum bspc_state state;
    enum bspc_pending_release pending_release;
    int32_t position;
    const struct device *dev;
};

static struct bspc_slot bspc_slots[BSPC_MAX_POSITIONS];

struct behavior_bspc_special_config {
    char *caps_word_dev;
};

static struct zmk_behavior_binding_event slot_event(struct bspc_slot *slot) {
    return (struct zmk_behavior_binding_event){
        .position = slot->position,
        .timestamp = k_uptime_get(),
    };
}

static void fire_key(uint32_t keycode, bool pressed) {
    raise_zmk_keycode_state_changed_from_encoded(keycode, pressed, k_uptime_get());
}

static void invoke_caps_word(struct bspc_slot *slot, bool pressed) {
    const struct behavior_bspc_special_config *cfg = slot->dev->config;
    struct zmk_behavior_binding binding = {.behavior_dev = cfg->caps_word_dev};
    zmk_behavior_invoke_binding(&binding, slot_event(slot), pressed);
}

static void virtual_release_work_handler(struct k_work *work) {
    struct k_work_delayable *dwork = k_work_delayable_from_work(work);
    struct bspc_slot *slot = CONTAINER_OF(dwork, struct bspc_slot, virtual_release_work);

    switch (slot->pending_release) {
    case BSPC_RELEASE_TAP:
        fire_key(BSPC, false);
        break;
    case BSPC_RELEASE_CAPS_WORD:
        invoke_caps_word(slot, false);
        break;
    default:
        break;
    }
    slot->pending_release = BSPC_RELEASE_NONE;
}

static void term_work_handler(struct k_work *work) {
    struct k_work_delayable *dwork = k_work_delayable_from_work(work);
    struct bspc_slot *slot = CONTAINER_OF(dwork, struct bspc_slot, term_work);

    if (slot->state == BSPC_P1_PENDING || slot->state == BSPC_P2_PENDING) {
        slot->state = BSPC_REPEAT_ACTIVE;
        fire_key(BSPC, true);
    }
}

static void followup_work_handler(struct k_work *work) {
    struct k_work_delayable *dwork = k_work_delayable_from_work(work);
    struct bspc_slot *slot = CONTAINER_OF(dwork, struct bspc_slot, followup_work);

    if (slot->state == BSPC_AFTER_TAP_WAIT) {
        slot->state = BSPC_IDLE;
    }
}

static int on_keymap_binding_pressed(struct zmk_behavior_binding *binding,
                                     struct zmk_behavior_binding_event event) {
    if (event.position >= BSPC_MAX_POSITIONS) {
        return -EINVAL;
    }

    struct bspc_slot *slot = &bspc_slots[event.position];
    slot->position = event.position;
    slot->dev = zmk_behavior_get_binding(binding->behavior_dev);

    switch (slot->state) {
    case BSPC_IDLE:
        slot->state = BSPC_P1_PENDING;
        k_work_schedule(&slot->term_work, K_MSEC(CONFIG_ZMK_BSPC_SPECIAL_HOLD_MS));
        break;
    case BSPC_AFTER_TAP_WAIT:
        k_work_cancel_delayable(&slot->followup_work);
        slot->state = BSPC_P2_PENDING;
        k_work_schedule(&slot->term_work, K_MSEC(CONFIG_ZMK_BSPC_SPECIAL_HOLD_MS));
        break;
    case BSPC_P1_PENDING:
    case BSPC_P2_PENDING:
    case BSPC_REPEAT_ACTIVE:
    default:
        /* Stray extra press with no matching release yet - ignore. */
        break;
    }

    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_keymap_binding_released(struct zmk_behavior_binding *binding,
                                      struct zmk_behavior_binding_event event) {
    ARG_UNUSED(binding);

    if (event.position >= BSPC_MAX_POSITIONS) {
        return -EINVAL;
    }

    struct bspc_slot *slot = &bspc_slots[event.position];

    switch (slot->state) {
    case BSPC_P1_PENDING:
        k_work_cancel_delayable(&slot->term_work);
        slot->state = BSPC_AFTER_TAP_WAIT;
        fire_key(BSPC, true);
        slot->pending_release = BSPC_RELEASE_TAP;
        k_work_schedule(&slot->virtual_release_work, K_MSEC(CONFIG_ZMK_DEBOUNCE_TAP_MS));
        k_work_schedule(&slot->followup_work, K_MSEC(CONFIG_ZMK_BSPC_SPECIAL_FOLLOWUP_MS));
        break;
    case BSPC_P2_PENDING:
        k_work_cancel_delayable(&slot->term_work);
        slot->state = BSPC_IDLE;
        invoke_caps_word(slot, true);
        slot->pending_release = BSPC_RELEASE_CAPS_WORD;
        k_work_schedule(&slot->virtual_release_work, K_MSEC(CONFIG_ZMK_DEBOUNCE_TAP_MS));
        break;
    case BSPC_REPEAT_ACTIVE:
        fire_key(BSPC, false);
        slot->state = BSPC_IDLE;
        break;
    case BSPC_IDLE:
    case BSPC_AFTER_TAP_WAIT:
    default:
        break;
    }

    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api behavior_bspc_special_driver_api = {
    .binding_pressed = on_keymap_binding_pressed,
    .binding_released = on_keymap_binding_released,
};

static int behavior_bspc_special_init(const struct device *dev) {
    for (int i = 0; i < BSPC_MAX_POSITIONS; i++) {
        k_work_init_delayable(&bspc_slots[i].term_work, term_work_handler);
        k_work_init_delayable(&bspc_slots[i].followup_work, followup_work_handler);
        k_work_init_delayable(&bspc_slots[i].virtual_release_work, virtual_release_work_handler);
    }

    return 0;
}

#define BSPC_INST(n)                                                                             \
    static const struct behavior_bspc_special_config behavior_bspc_special_config_##n = {         \
        .caps_word_dev = DEVICE_DT_NAME(DT_INST_PHANDLE_BY_IDX(n, bindings, 0)),                  \
    };                                                                                            \
    BEHAVIOR_DT_INST_DEFINE(n, behavior_bspc_special_init, NULL, NULL,                            \
                            &behavior_bspc_special_config_##n, POST_KERNEL,                       \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &behavior_bspc_special_driver_api);

DT_INST_FOREACH_STATUS_OKAY(BSPC_INST)
