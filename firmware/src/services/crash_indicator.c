#include "crash_indicator.h"

#include "board_layout.h"
#include "buttons.h"

/* Real feedback, after the first hardware pass: "dismiss is a single
 * shift click not a hold" -- corrects this module's original 2-second-
 * hold spec once it had actually been tried. A click fires on release,
 * requiring circle to have been the ONLY function button down for the
 * ENTIRE press, not just at the instant of release -- s_circle_was_
 * pressed_alone tracks that across the whole press so a hand lifting
 * off a multi-button combo (the debug-mode combo, the expression-mute
 * combo -- both also involve circle) one finger at a time, circle
 * last, can never be mistaken for this click just because circle
 * happened to be the only one still down at the exact release instant. */

static bool s_active = false;
static bool s_circle_pressed = false;
static bool s_circle_was_pressed_alone = false;

void tiles_crash_indicator_init(bool crash_recovered) {
    s_active = crash_recovered;
    s_circle_pressed = false;
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

    if (circle && !s_circle_pressed) {
        s_circle_pressed = true;
        s_circle_was_pressed_alone = !others_held;
    } else if (circle && s_circle_pressed && others_held) {
        s_circle_was_pressed_alone = false;
    } else if (!circle && s_circle_pressed) {
        s_circle_pressed = false;
        if (s_circle_was_pressed_alone) {
            s_active = false;
        }
    }
}

bool tiles_crash_indicator_is_active(void) {
    return s_active;
}
