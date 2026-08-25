#include "haptics.h"

#include "board_pins.h"
#include "buttons.h"
#include "pad_config.h"
#include "power.h"

#include "pca9685.h"

#include "pico/time.h"

#include <stdbool.h>

/* Brief and strong -- a jolt, not a buzz. Unmeasured: no per-motor
 * spin-up/current data exists yet (see the board map's
 * measured_current_required TODOs). */
#define KICK_DURATION_MS 30u

/* The hard-zero "brake" gap between KICK and SUSTAIN -- see haptics.h's
 * file header for why this, not a soft ramp, is the achievable analog
 * to braking on this hardware. */
#define KICK_GAP_MS 8u

/* Even the weakest strike should give a felt kick, not nothing. */
#define MIN_KICK_DUTY 0.35f
#define MAX_KICK_DUTY 1.0f

/* Capped below the kick's peak, and below 1.0, because sustain can be
 * held continuously for seconds (a long-held chord) where a brief
 * kick's inrush/thermal risk doesn't apply the same way -- unmeasured,
 * a conservative starting guess pending real current data. */
#define MAX_SUSTAIN_DUTY 0.6f

typedef enum {
    HAPTIC_PHASE_IDLE = 0,
    HAPTIC_PHASE_KICK,
    HAPTIC_PHASE_GAP,
    HAPTIC_PHASE_SUSTAIN,
} haptic_phase_t;

typedef struct {
    haptic_phase_t phase;
    uint32_t phase_start_ms;
    uint8_t sustain_target_0_127; /* latest aftertouch value, applied once SUSTAIN begins */
} haptic_pad_state_t;

static haptic_pad_state_t s_pads[TILES_NUM_PADS];

/* Mirrors services/buttons.c's set_button_led_level(), parameterized on
 * active_level instead of hardcoding active-low, since motor channels
 * are active-high (pin high = low-side NMOS on = motor driven) while
 * button LEDs are active-low -- see pad_config.c's tiles_haptic_route_t
 * for why this field exists per pad rather than being assumed. */
static void set_motor_level(const tiles_pad_config_t *cfg, float level_0_to_1) {
    tiles_pca9685_t *pca = tiles_buttons_pca9685_for_addr(cfg->haptic.pca9685_i2c_addr);
    if (pca == NULL) {
        return;
    }
    if (level_0_to_1 < 0.0f) {
        level_0_to_1 = 0.0f;
    }
    if (level_0_to_1 > 1.0f) {
        level_0_to_1 = 1.0f;
    }

    bool active_high = (cfg->haptic.active_level == TILES_ACTIVE_HIGH);
    uint8_t channel = cfg->haptic.channel;

    if (level_0_to_1 <= 0.0f) {
        tiles_pca9685_set_channel_full(pca, channel, !active_high);
        return;
    }
    if (level_0_to_1 >= 1.0f) {
        tiles_pca9685_set_channel_full(pca, channel, active_high);
        return;
    }

    float on_fraction = active_high ? level_0_to_1 : (1.0f - level_0_to_1);
    uint16_t off_count = (uint16_t)(on_fraction * 4095.0f);
    if (off_count < 1u) {
        off_count = 1u;
    }
    if (off_count > 4094u) {
        off_count = 4094u;
    }
    tiles_pca9685_set_pwm(pca, channel, 0u, off_count);
}

static float kick_duty_from_velocity(uint8_t velocity_0_127) {
    float v = (float)velocity_0_127 / 127.0f;
    return MIN_KICK_DUTY + (MAX_KICK_DUTY - MIN_KICK_DUTY) * v;
}

static float sustain_duty_from_aftertouch(uint8_t aftertouch_0_127) {
    return MAX_SUSTAIN_DUTY * ((float)aftertouch_0_127 / 127.0f);
}

static uint8_t active_voice_count(void) {
    uint8_t count = 0;
    for (uint8_t i = 0; i < TILES_NUM_PADS; i++) {
        if (s_pads[i].phase != HAPTIC_PHASE_IDLE) {
            count++;
        }
    }
    return count;
}

void tiles_haptics_init(void) {
    for (uint8_t i = 0; i < TILES_NUM_PADS; i++) {
        s_pads[i] = (haptic_pad_state_t){0};
        s_pads[i].phase = HAPTIC_PHASE_IDLE;
    }
}

void tiles_haptics_trigger_kick(uint8_t logical_pad, uint8_t velocity_0_127) {
    if (logical_pad < 1u || logical_pad > TILES_NUM_PADS) {
        return;
    }
    uint8_t idx = (uint8_t)(logical_pad - 1u);

    /* Ceiling only applies to a genuinely new voice -- re-triggering an
     * already-active pad (shouldn't happen given expression.c's own
     * state machine, but cheap to guard) never gets refused. */
    if (s_pads[idx].phase == HAPTIC_PHASE_IDLE &&
        active_voice_count() >= tiles_power_get_state().max_haptic_voices) {
        return;
    }

    const tiles_pad_config_t *cfg = board_pad_config(logical_pad);
    if (cfg == NULL) {
        return;
    }

    s_pads[idx].phase = HAPTIC_PHASE_KICK;
    s_pads[idx].phase_start_ms = to_ms_since_boot(get_absolute_time());
    s_pads[idx].sustain_target_0_127 = 0u;
    set_motor_level(cfg, kick_duty_from_velocity(velocity_0_127));
}

void tiles_haptics_set_sustain_level(uint8_t logical_pad, uint8_t aftertouch_0_127) {
    if (logical_pad < 1u || logical_pad > TILES_NUM_PADS) {
        return;
    }
    uint8_t idx = (uint8_t)(logical_pad - 1u);
    s_pads[idx].sustain_target_0_127 = aftertouch_0_127;

    if (s_pads[idx].phase == HAPTIC_PHASE_SUSTAIN) {
        const tiles_pad_config_t *cfg = board_pad_config(logical_pad);
        if (cfg != NULL) {
            set_motor_level(cfg, sustain_duty_from_aftertouch(aftertouch_0_127));
        }
    }
}

void tiles_haptics_stop(uint8_t logical_pad) {
    if (logical_pad < 1u || logical_pad > TILES_NUM_PADS) {
        return;
    }
    uint8_t idx = (uint8_t)(logical_pad - 1u);
    if (s_pads[idx].phase == HAPTIC_PHASE_IDLE) {
        return;
    }
    s_pads[idx].phase = HAPTIC_PHASE_IDLE;

    const tiles_pad_config_t *cfg = board_pad_config(logical_pad);
    if (cfg != NULL) {
        set_motor_level(cfg, 0.0f);
    }
}

void tiles_haptics_scan(void) {
    uint32_t now_ms = to_ms_since_boot(get_absolute_time());

    for (uint8_t i = 0; i < TILES_NUM_PADS; i++) {
        haptic_pad_state_t *s = &s_pads[i];
        uint8_t logical_pad = (uint8_t)(i + 1u);

        if (s->phase == HAPTIC_PHASE_KICK && (now_ms - s->phase_start_ms) >= KICK_DURATION_MS) {
            s->phase = HAPTIC_PHASE_GAP;
            s->phase_start_ms = now_ms;
            const tiles_pad_config_t *cfg = board_pad_config(logical_pad);
            if (cfg != NULL) {
                set_motor_level(cfg, 0.0f);
            }
        } else if (s->phase == HAPTIC_PHASE_GAP && (now_ms - s->phase_start_ms) >= KICK_GAP_MS) {
            s->phase = HAPTIC_PHASE_SUSTAIN;
            const tiles_pad_config_t *cfg = board_pad_config(logical_pad);
            if (cfg != NULL) {
                set_motor_level(cfg, sustain_duty_from_aftertouch(s->sustain_target_0_127));
            }
        }
    }
}
