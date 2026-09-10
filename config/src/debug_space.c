/*
 * TEMPORARY diagnostic: logs every physical press/release at the Space
 * position (31 in this project's own 0-33 numbering) with a timestamp,
 * before any behavior/combo processing touches it - so a genuinely
 * bouncing/flaky physical switch (multiple press/release pairs within
 * a few ms) can be told apart from a software issue in rapid-fire or
 * anything else layered on top of that position. Space-only, and meant
 * to be removed again once the physical-switch question is settled -
 * see CMakeLists.txt for how this gets compiled in.
 */
#include <zephyr/logging/log.h>

#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>

#define DEBUG_SPACE_POSITION 31

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

static int debug_space_listener(const zmk_event_t *eh) {
    const struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);

    if (ev == NULL || ev->position != DEBUG_SPACE_POSITION) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    LOG_INF("SPACE_DEBUG pos=%d state=%s t=%u", ev->position, ev->state ? "DOWN" : "UP",
            (uint32_t)ev->timestamp);

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(debug_space, debug_space_listener);
ZMK_SUBSCRIPTION(debug_space, zmk_position_state_changed);
