#pragma once

/*
 * The settings table itself (one row per user-tunable value) plus the boot and
 * per-loop hooks that bring up the registry and flash persistence. See
 * profiles/settings.h for what a setting is and why it's built this way.
 */

/* Registers the table, captures every setting's default from its module, loads
 * the saved snapshot from flash (if any) and applies it. Call ONCE at boot,
 * after every module a row binds to has initialized (pedal, expression,
 * cv_gate, lighting, DIN MIDI) and before the main loop. If flash can't be used
 * safely the table still works -- changes just don't persist (printed once). */
void tiles_settings_boot(void);

/* Call every main-loop iteration: notices changed settings and saves them
 * (debounced, and only while the pads are idle). Cheap between polls. */
void tiles_settings_scan(void);
