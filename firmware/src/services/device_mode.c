#include "device_mode.h"

#include "settings_persist.h"
#include "storage_flash.h"

#include "hardware/watchdog.h"
#include "pico/platform/sections.h"
#include "pico/time.h"

#include <stdio.h>

/* Long enough for a SET's OK to reach the host before the restart. */
#define RESTART_DELAY_MS 300u
/* Survives the restart (RAM isn't cleared by a watchdog reboot); random
 * after a power cut, so only this exact value counts. */
#define QUICK_RESTART_MAGIC 0x51A7C0DEu

static uint32_t __uninitialized_ram(s_quick_restart_word);

static tiles_device_mode_t s_running;
static tiles_device_mode_t s_boot_choice;
static bool s_quick_restart;
static uint32_t s_setting;
static bool s_loaded;
static bool s_restart_pending;
static uint32_t s_restart_at_ms;

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

void tiles_device_mode_service(uint32_t now_ms) {
    if (!s_restart_pending || (int32_t)(now_ms - s_restart_at_ms) < 0) {
        return;
    }
    (void)tiles_settings_persist_save_now();
    printf("[mode] restarting in %s mode\n", s_setting == TILES_DEVICE_STANDALONE ? "standalone" : "controller");
    s_quick_restart_word = QUICK_RESTART_MAGIC;
    watchdog_reboot(0, 0, 1u);
    while (true) {
        tight_loop_contents(); /* not fed: resets within 1 ms */
    }
}
