#include "crash_indicator.h"

#include "board_layout.h"
#include "buttons.h"

/* Dismiss is a click (fires on release) of circle ALONE for the whole
 * press: s_circle_was_pressed_alone tracks that across the press, so a
 * combo released circle-last doesn't count. */

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

    /* Any other function button down voids the click (see the header). */
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
