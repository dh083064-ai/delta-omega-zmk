/*
 * Delta Omega DEBUG-only input logger (esb-tdma-pcbang-gaming branch,
 * temporary - not part of the stable rapid-fire behavior).
 *
 * Logs every physical key press/release (raw zmk_position_state_changed,
 * the same event combos/gcmd already key off - see behavior_gaming_cmd.c)
 * with one line per edge. Deliberately touches NOTHING else - no keymap,
 * Kconfig, or existing behavior file changes, so stable rapid-fire
 * behavior/timing is provably unaffected. Purely a listener bolted onto
 * the existing event bus.
 *
 * Timing: relies entirely on Zephyr's own per-line log timestamp (already
 * hardware-timestamped at microsecond resolution at the point of the
 * LOG_INF call, visible as the "[hh:mm:ss.mmm,uuu]" prefix on every
 * captured line) rather than embedding a custom counter - simpler, and
 * avoids any wraparound accounting for a 32-bit cycle counter over a
 * multi-minute capture.
 *
 * G's rapid-fire *virtual* DOWN/UP is intentionally not duplicated here -
 * it's already fully visible in the existing ZMK core hid_listener_
 * keycode_pressed/released debug logs (keycode 0x0A for G) at the same
 * timestamp resolution, with zero risk of this file drifting out of sync
 * with behavior_rapid_fire.c's actual logic.
 *
 * Compiles to nothing when CONFIG_LOG=n (release builds).
 */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

static int debug_input_logger_listener(const zmk_event_t *eh) {
    const struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);

    if (ev == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    LOG_INF("IN: pos=%d %s", ev->position, ev->state ? "press" : "release");

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(debug_input_logger, debug_input_logger_listener);
ZMK_SUBSCRIPTION(debug_input_logger, zmk_position_state_changed);
