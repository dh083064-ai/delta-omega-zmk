/*
 * Smart Num - single custom state machine, replacing the previous
 * hold-tap(mo, num_dance) + tap-dance(num_word, sl) nesting.
 *
 * Mirrors LunaKey Pico QMK's smart_num_finished()/smart_num_reset()
 * (itself a reimplementation of this project's own original ZMK design)
 * as one flat state machine instead of two independently-timed ZMK
 * behaviors layered on top of each other:
 *
 *   OFF       - idle.
 *   PENDING   - SmartNum physically down, tap/hold/double-tap still
 *               undecided.
 *   MOMENTARY - NUM on only while physically held (immediate on
 *               interrupt by another key, or on tapping-term timeout if
 *               nothing interrupts) - layer_activate(NUM, false).
 *   STICKY    - NUM on indefinitely until SmartNum is double-tapped
 *               again - layer_activate(NUM, true).
 *
 * Short tap (count==1, released, no interrupt) delegates to the
 * existing &num_word behavior (urob/zmk-auto-layer) exactly once and
 * then steps back to OFF - num_word owns NUM's lifecycle from that
 * point on independently of this state machine, matching how ONESHOT
 * is just a fire-and-forget delegation here (QMK's own NUMSTATE_ONESHOT
 * is a hand-rolled reimplementation of num_word since QMK doesn't have
 * the real module; ZMK doesn't need to remember that state at all).
 *
 * STICKY is tracked here explicitly via `state == SMART_NUM_STICKY`,
 * never inferred from whether NUM happens to be active - NUM can also
 * be on because of num_word or the FN+NUM->SYS conditional layer, and
 * those must not be mistaken for "I turned this on, so I get to turn it
 * off."
 *
 * MOMENTARY uses layer_activate/deactivate(NUM, false) (matching &mo);
 * ZMK core's set_layer_state() already refuses a non-locking disable
 * against a currently-locked layer, so a MOMENTARY release can never
 * stomp a STICKY or num_word (locking=true) activation of NUM. STICKY
 * itself uses locking=true both ways, same as num_word - matching
 * QMK's own layer_on()/layer_off(), which has the identical
 * unprotected-against-each-other characteristic. A plain single tap
 * while STICKY is active still downgrades tracking to a fresh num_word
 * delegation (this file returns to OFF) exactly like QMK's finished()
 * unconditionally setting NUMSTATE_ONESHOT on any non-double resolution
 * - not specially preserved, intentionally, for 1:1 fidelity.
 */
#define DT_DRV_COMPAT zmk_behavior_smart_num

#include <zephyr/device.h>
#include <zephyr/kernel.h>

#include <drivers/behavior.h>
#include <zmk/behavior.h>
#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/keymap.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

enum smart_num_state {
    SMART_NUM_OFF = 0,
    SMART_NUM_PENDING,
    SMART_NUM_MOMENTARY,
    SMART_NUM_STICKY,
};

struct behavior_smart_num_config {
    const char *num_word_dev;
    int32_t tapping_term_ms;
};

static enum smart_num_state sn_state = SMART_NUM_OFF;
static uint8_t sn_tap_count;
static bool sn_pressed_now;
static uint32_t sn_position = UINT32_MAX;
static uint8_t sn_layer;
static const struct behavior_smart_num_config *sn_config;

static struct k_work_delayable sn_timer;
static struct k_work_sync sn_timer_sync;

static const char *sn_state_name(enum smart_num_state s) {
    switch (s) {
    case SMART_NUM_OFF:
        return "OFF";
    case SMART_NUM_PENDING:
        return "PENDING";
    case SMART_NUM_MOMENTARY:
        return "MOMENTARY";
    case SMART_NUM_STICKY:
        return "STICKY";
    default:
        return "UNKNOWN";
    }
}

static void sn_set_state(enum smart_num_state new_state) {
    if (sn_state != new_state) {
        LOG_DBG("smart_num: %s -> %s", sn_state_name(sn_state), sn_state_name(new_state));
        sn_state = new_state;
    }
}

static void sn_invoke_num_word(int64_t timestamp) {
    LOG_DBG("smart_num: single tap - invoking num_word on layer %d", sn_layer);

    struct zmk_behavior_binding_event event = {
        .position = sn_position,
        .timestamp = timestamp,
    };
    struct zmk_behavior_binding binding = {
        .behavior_dev = sn_config->num_word_dev,
        .param1 = sn_layer,
    };

    zmk_behavior_invoke_binding(&binding, event, true);
    zmk_behavior_invoke_binding(&binding, event, false);
}

/* Timer fired with SmartNum released and no further tap pending -
 * finalize the dance to ONESHOT (delegated) or STICKY toggle. */
static void sn_finish_dance(int64_t timestamp) {
    if (sn_tap_count >= 2) {
        if (sn_state == SMART_NUM_STICKY) {
            LOG_DBG("smart_num: double-tap while STICKY -> OFF");
            zmk_keymap_layer_deactivate(sn_layer, true);
            sn_set_state(SMART_NUM_OFF);
        } else {
            LOG_DBG("smart_num: double-tap -> STICKY");
            zmk_keymap_layer_activate(sn_layer, true);
            sn_set_state(SMART_NUM_STICKY);
        }
    } else if (sn_tap_count == 1) {
        sn_invoke_num_word(timestamp);
        if (sn_state == SMART_NUM_PENDING) {
            sn_set_state(SMART_NUM_OFF);
        }
    }
    sn_tap_count = 0;
}

static void sn_timer_handler(struct k_work *work) {
    ARG_UNUSED(work);

    if (sn_pressed_now) {
        /* Held past the tapping term with no interrupt - hold-timeout MOMENTARY. */
        LOG_DBG("smart_num: tapping-term expired while still held -> MOMENTARY");
        sn_tap_count = 0;
        zmk_keymap_layer_activate(sn_layer, false);
        sn_set_state(SMART_NUM_MOMENTARY);
        return;
    }

    sn_finish_dance(k_uptime_get());
}

/* Any other key going down while SmartNum is still physically held and
 * undecided resolves it as MOMENTARY immediately, before that key's own
 * event reaches keymap dispatch - mirrors QMK's finished() firing early
 * with state->pressed == true on interrupt. */
static int sn_position_state_changed_listener(const zmk_event_t *eh) {
    const struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);

    if (ev == NULL || !ev->state || ev->position == sn_position) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (sn_pressed_now && sn_state != SMART_NUM_MOMENTARY) {
        LOG_DBG("smart_num: interrupted by position %d while held -> MOMENTARY", ev->position);
        k_work_cancel_delayable_sync(&sn_timer, &sn_timer_sync);
        sn_tap_count = 0;
        zmk_keymap_layer_activate(sn_layer, false);
        sn_set_state(SMART_NUM_MOMENTARY);
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(behavior_smart_num, sn_position_state_changed_listener);
ZMK_SUBSCRIPTION(behavior_smart_num, zmk_position_state_changed);

static int sn_keymap_binding_pressed(struct zmk_behavior_binding *binding,
                                     struct zmk_behavior_binding_event event) {
    const struct device *dev = zmk_behavior_get_binding(binding->behavior_dev);

    sn_config = dev->config;
    sn_position = event.position;
    sn_layer = binding->param1;
    sn_pressed_now = true;
    sn_tap_count++;
    sn_set_state(SMART_NUM_PENDING);

    LOG_DBG("smart_num: press #%d (position %d, layer %d)", sn_tap_count, sn_position, sn_layer);

    k_work_reschedule(&sn_timer, K_MSEC(sn_config->tapping_term_ms));

    return ZMK_BEHAVIOR_OPAQUE;
}

static int sn_keymap_binding_released(struct zmk_behavior_binding *binding,
                                      struct zmk_behavior_binding_event event) {
    ARG_UNUSED(binding);
    ARG_UNUSED(event);

    sn_pressed_now = false;

    if (sn_state == SMART_NUM_MOMENTARY) {
        LOG_DBG("smart_num: release -> OFF (was MOMENTARY)");
        k_work_cancel_delayable_sync(&sn_timer, &sn_timer_sync);
        zmk_keymap_layer_deactivate(sn_layer, false);
        sn_set_state(SMART_NUM_OFF);
        sn_tap_count = 0;
        return ZMK_BEHAVIOR_OPAQUE;
    }

    /* Quick release with no interrupt yet - keep waiting out the full
     * tapping-term window in case a second tap follows, exactly like
     * QMK's tap dance: releasing never resolves the dance early by
     * itself. */
    k_work_reschedule(&sn_timer, K_MSEC(sn_config->tapping_term_ms));

    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api behavior_smart_num_driver_api = {
    .binding_pressed = sn_keymap_binding_pressed,
    .binding_released = sn_keymap_binding_released,
};

static int behavior_smart_num_init(const struct device *dev) {
    ARG_UNUSED(dev);
    k_work_init_delayable(&sn_timer, sn_timer_handler);
    return 0;
}

#define SMART_NUM_INST(n)                                                                        \
    static const struct behavior_smart_num_config behavior_smart_num_config_##n = {              \
        .num_word_dev = DEVICE_DT_NAME(DT_INST_PHANDLE_BY_IDX(n, bindings, 0)),                  \
        .tapping_term_ms = DT_INST_PROP(n, tapping_term_ms),                                     \
    };                                                                                            \
    BEHAVIOR_DT_INST_DEFINE(n, behavior_smart_num_init, NULL, NULL,                              \
                            &behavior_smart_num_config_##n, POST_KERNEL,                         \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &behavior_smart_num_driver_api);

DT_INST_FOREACH_STATUS_OKAY(SMART_NUM_INST)
