#include "crash_indicator.h"

#include "board_layout.h"
#include "buttons.h"

#include "pico/time.h"

/* Matches this codebase's other one-shot hold gestures (services/
 * debug_mode.c's 8-second combo, services/expression_control.h's
 * 3-second combos) in shape, not duration -- 2 seconds specifically
 * because real feedback asked for exactly that: "presing shift for 2
 * secodns." Deliberately no separate _triggered_this_hold edge-latch
 * like those other gestures use: unlike a toggle, dismissing is a
 * one-way transition (s_active can only go true->false here), so
 * there's nothing a repeat trigger could do wrong even if the hold
 * continued past the threshold -- the very next scan's early-return
 * (once s_active is false) makes the whole hold-tracking block dead
 * until another crash sets s_active true again. */
#define TILES_CRASH_INDICATOR_DISMISS_HOLD_MS 2000u

static bool s_active = false;
static bool s_circle_alone_held = false;
static uint32_t s_circle_alone_start_ms = 0u;

void tiles_crash_indicator_init(bool crash_recovered) {
    s_active = crash_recovered;
    s_circle_alone_held = false;
}

void tiles_crash_indicator_scan(void) {
    if (!s_active) {
        return;
    }

    /* "on its own" -- every other function button must be up, so this
     * can never fire mid-way through the debug-mode combo (diamond+
     * square+circle) or the expression-mute combo (circle+square); see
     * this module's header for why that matters. */
    bool circle = tiles_button_is_pressed(TILES_CIRCLE_BUTTON_ID);
    bool others_held = tiles_button_is_pressed(TILES_MINUS_BUTTON_ID) ||
                        tiles_button_is_pressed(TILES_PLUS_BUTTON_ID) ||
                        tiles_button_is_pressed(TILES_TRIANGLE_BUTTON_ID) ||
                        tiles_button_is_pressed(TILES_DIAMOND_BUTTON_ID) ||
                        tiles_button_is_pressed(TILES_SQUARE_BUTTON_ID);
    bool circle_alone_now = circle && !others_held;

    uint32_t now_ms = to_ms_since_boot(get_absolute_time());

    if (circle_alone_now && !s_circle_alone_held) {
        s_circle_alone_held = true;
        s_circle_alone_start_ms = now_ms;
    } else if (!circle_alone_now) {
        s_circle_alone_held = false;
    }

    if (s_circle_alone_held && (now_ms - s_circle_alone_start_ms) >= TILES_CRASH_INDICATOR_DISMISS_HOLD_MS) {
        s_active = false;
    }
}

bool tiles_crash_indicator_is_active(void) {
    return s_active;
}
