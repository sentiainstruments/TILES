#include "debug_mode.h"

#include "board_layout.h"
#include "buttons.h"

#include "pico/time.h"

#include "tusb.h"

#include <string.h>

/* Real feedback: "how diamond square circle hold for 8 secodns." Deliberately
 * NOT the reserved 4-button combo (triangle+diamond+square+circle,
 * services/game_mode.c's own GM_HOLD_MS) -- that one triggers after only
 * 700ms, so a hand that includes triangle while working toward this
 * combo's 8-second hold would fire game mode's secret entry first,
 * several seconds before debug mode itself would ever toggle. Requiring
 * triangle to be UP (not just "don't care") keeps the two combos from
 * ever being satisfied by the same held hand. */
#define DEBUG_MODE_HOLD_MS 8000u

static bool s_debug_mode_active;
static bool s_combo_held;
static uint32_t s_combo_start_ms;
static bool s_combo_triggered_this_hold;

void tiles_debug_mode_init(void) {
    s_debug_mode_active = false;
    s_combo_held = false;
    s_combo_start_ms = 0u;
    s_combo_triggered_this_hold = false;
}

void tiles_debug_mode_scan(void) {
    uint32_t now_ms = to_ms_since_boot(get_absolute_time());
    bool diamond = tiles_button_is_pressed(TILES_DIAMOND_BUTTON_ID);
    bool square = tiles_button_is_pressed(TILES_SQUARE_BUTTON_ID);
    bool circle = tiles_button_is_pressed(TILES_CIRCLE_BUTTON_ID);
    bool triangle = tiles_button_is_pressed(TILES_TRIANGLE_BUTTON_ID);

    bool combo_now = diamond && square && circle && !triangle;

    if (combo_now && !s_combo_held) {
        s_combo_held = true;
        s_combo_start_ms = now_ms;
        s_combo_triggered_this_hold = false;
    } else if (!combo_now) {
        s_combo_held = false;
    }

    /* One-shot per hold, matching this codebase's own established
     * hold-timer shape (e.g. services/game_mode.c's GM_HOLD_MS,
     * services/op_mode.c's diamond record-arm hold) -- toggles exactly
     * once at the 8-second mark, not repeatedly for as long as the hold
     * continues past it. */
    if (s_combo_held && !s_combo_triggered_this_hold && (now_ms - s_combo_start_ms) >= DEBUG_MODE_HOLD_MS) {
        s_combo_triggered_this_hold = true;
        s_debug_mode_active = !s_debug_mode_active;
    }
}

bool tiles_debug_mode_is_active(void) {
    return s_debug_mode_active;
}

void tiles_debug_trace(char code) {
    if (!s_debug_mode_active) {
        return;
    }
    if (tud_cdc_write_available() < 1u) {
        return;
    }
    tud_cdc_write(&code, 1);
}

void tiles_debug_trace_str(const char *s) {
    if (!s_debug_mode_active || s == NULL) {
        return;
    }
    uint32_t available = tud_cdc_write_available();
    if (available == 0u) {
        return;
    }
    uint32_t len = (uint32_t)strlen(s);
    if (len > available) {
        len = available;
    }
    tud_cdc_write(s, len);
}

void tiles_debug_trace_flush(void) {
    if (!s_debug_mode_active) {
        return;
    }
    tud_cdc_write_flush();
}
