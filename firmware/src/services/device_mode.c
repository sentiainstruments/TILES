#include "device_mode.h"

#include "board_layout.h"
#include "board_pins.h"
#include "buttons.h"
#include "lighting.h"
#include "settings_persist.h"
#include "storage_flash.h"
#include "usb_device.h"

#include "hardware/watchdog.h"
#include "pico/platform/sections.h"
#include "pico/time.h"

#include <stdio.h>

/* Long enough for a SET's OK to reach the host before the restart. */
#define RESTART_DELAY_MS 300u
/* Survives the restart (RAM isn't cleared by a watchdog reboot); random
 * after a power cut, so only this exact value counts. */
#define QUICK_RESTART_MAGIC 0x51A7C0DEu
#define TRIANGLE_SWITCH_HOLD_MS 10000u
/* The new mode's colour rises, holds, then TILES restarts. */
#define SWITCH_FLASH_RISE_MS 300u
#define SWITCH_FLASH_HOLD_MS 300u
#define SWITCH_FLASH_FRAME_MS 30u

static uint32_t __uninitialized_ram(s_quick_restart_word);

static tiles_device_mode_t s_running;
static tiles_device_mode_t s_boot_choice;
static bool s_quick_restart;
static uint32_t s_setting;
static bool s_loaded;
static bool s_restart_pending;
static uint32_t s_restart_at_ms;
static bool s_flash_before_restart;
/* Starts true: a press carried over from the boot animation never counts. */
static bool s_triangle_was_down = true;
static bool s_triangle_hold_live;
static uint32_t s_triangle_down_ms;

void tiles_device_mode_boot(void) {
    s_quick_restart = s_quick_restart_word == QUICK_RESTART_MAGIC;
    s_quick_restart_word = 0u;
    uint32_t saved = TILES_DEVICE_CONTROLLER;
    if (tiles_storage_settings_region_safe()) {
        (void)tiles_settings_persist_peek(tiles_storage_settings_ops(), TILES_DEVICE_MODE_SETTING_ID, &saved);
    }
    s_running = saved == TILES_DEVICE_STANDALONE ? TILES_DEVICE_STANDALONE : TILES_DEVICE_CONTROLLER;
    s_boot_choice = s_running;
    s_setting = s_running;
}

tiles_device_mode_t tiles_device_mode(void) {
    return s_running;
}

bool tiles_device_mode_is_standalone(void) {
    return s_running == TILES_DEVICE_STANDALONE;
}

void tiles_device_mode_color(tiles_device_mode_t mode, float *r, float *g, float *b) {
    bool standalone = mode == TILES_DEVICE_STANDALONE;
    *r = standalone ? 1.0f : 0.0f;
    *g = standalone ? 0.0f : 0.08f;
    *b = 1.0f;
}

bool tiles_device_mode_quick_restart(void) {
    return s_quick_restart;
}

void tiles_device_mode_boot_toggle(void) {
    s_boot_choice = s_boot_choice == TILES_DEVICE_STANDALONE ? TILES_DEVICE_CONTROLLER : TILES_DEVICE_STANDALONE;
}

tiles_device_mode_t tiles_device_mode_boot_choice(void) {
    return s_boot_choice;
}

/* Before the settings load, the default (controller): what the registry
 * captures as the default, so standalone is always saved when chosen. */
uint32_t tiles_device_mode_setting_get(void) {
    return s_loaded ? s_setting : (uint32_t)TILES_DEVICE_CONTROLLER;
}

static void request(uint32_t value, uint32_t delay_ms) {
    s_setting = value;
    s_restart_pending = value != (uint32_t)s_running;
    s_restart_at_ms = to_ms_since_boot(get_absolute_time()) + delay_ms;
}

void tiles_device_mode_setting_set(uint32_t value) {
    if (s_loaded) {
        request(value, RESTART_DELAY_MS);
    }
    /* Before: the saved value, already decided in tiles_device_mode_boot(). */
}

void tiles_device_mode_settings_loaded(void) {
    s_loaded = true;
    if (s_boot_choice != s_running) {
        request((uint32_t)s_boot_choice, 0u);
    }
}

/* Triangle alone: any other button joining ends the hold (they make combos). */
static void watch_triangle_hold(uint32_t now_ms) {
    bool down = tiles_button_is_pressed(TILES_TRIANGLE_BUTTON_ID);
    bool others = tiles_button_is_pressed(TILES_MINUS_BUTTON_ID) || tiles_button_is_pressed(TILES_PLUS_BUTTON_ID) ||
                  tiles_button_is_pressed(TILES_DIAMOND_BUTTON_ID) || tiles_button_is_pressed(TILES_SQUARE_BUTTON_ID) ||
                  tiles_button_is_pressed(TILES_CIRCLE_BUTTON_ID);
    if (down && !s_triangle_was_down) {
        s_triangle_hold_live = true;
        s_triangle_down_ms = now_ms;
    }
    if (!down || others) {
        s_triangle_hold_live = false;
    }
    s_triangle_was_down = down;
    if (s_triangle_hold_live && s_loaded && !s_restart_pending && now_ms - s_triangle_down_ms >= TRIANGLE_SWITCH_HOLD_MS) {
        s_triangle_hold_live = false;
        s_flash_before_restart = true;
        request(s_running == TILES_DEVICE_STANDALONE ? TILES_DEVICE_CONTROLLER : TILES_DEVICE_STANDALONE, 0u);
    }
}

static void flash_new_mode(tiles_device_mode_t mode) {
    float r, g, b;
    tiles_device_mode_color(mode, &r, &g, &b);
    tiles_lighting_set_standby_active(true);
    for (uint32_t t = 0u; t <= SWITCH_FLASH_RISE_MS + SWITCH_FLASH_HOLD_MS; t += SWITCH_FLASH_FRAME_MS) {
        float k = t >= SWITCH_FLASH_RISE_MS ? 1.0f : (float)t / (float)SWITCH_FLASH_RISE_MS;
        for (uint8_t pad = 1u; pad <= TILES_NUM_PADS; pad++) {
            tiles_lighting_set_standby_pad_rgb(pad, r * k, g * k, b * k);
        }
        for (uint8_t i = 0u; i < TILES_NUM_UNDERGLOW_ANCHORS; i++) {
            tiles_lighting_set_standby_underglow_rgb(i, r * k, g * k, b * k);
        }
        tiles_lighting_flush_standby();
        watchdog_update();
        absolute_time_t deadline = make_timeout_time_ms(SWITCH_FLASH_FRAME_MS);
        do {
            tiles_usb_task();
        } while (!time_reached(deadline));
    }
}

void tiles_device_mode_service(uint32_t now_ms) {
    watch_triangle_hold(now_ms);
    if (!s_restart_pending || (int32_t)(now_ms - s_restart_at_ms) < 0) {
        return;
    }
    if (s_flash_before_restart) {
        flash_new_mode((tiles_device_mode_t)s_setting);
    }
    (void)tiles_settings_persist_save_now();
    printf("[mode] restarting in %s mode\n", s_setting == TILES_DEVICE_STANDALONE ? "standalone" : "controller");
    s_quick_restart_word = QUICK_RESTART_MAGIC;
    watchdog_reboot(0, 0, 1u);
    while (true) {
        tight_loop_contents(); /* not fed: resets within 1 ms */
    }
}
