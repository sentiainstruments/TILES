#include "haptics.h"

#include "board_pins.h"
#include "buttons.h"
#include "pad_config.h"
#include "power.h"

#include "pca9685.h"

#include "pico/time.h"

#include <math.h>
#include <stdbool.h>
#include <stdio.h>

/* Whole KICK phase, overdrive included: a jolt, not a buzz. Raised from
 * 30 ms when the kick felt too soft. Unmeasured. */
#define KICK_DURATION_MS 45u

/* Overdrive: full duty at the start of every kick, whatever the velocity,
 * to beat the motor's static friction so even soft strikes start fast.
 * Then the velocity-mapped duty for the rest of KICK_DURATION_MS. About
 * starting fast, not stopping (braking is impossible; see haptics.h). */
#define KICK_OVERDRIVE_MS 10u

/* The hard-zero "brake" after KICK (see haptics.h). */
#define KICK_GAP_MS 8u

/* Even the weakest strike gives a strong, felt kick (raised from 0.35,
 * which felt too soft); velocity still has room above it. */
#define MIN_KICK_DUTY 0.65f
#define MAX_KICK_DUTY 1.0f

/* Below the kick peak: sustain can run for seconds, where inrush and heat
 * matter more. Conservative, unmeasured. */
#define MAX_SUSTAIN_DUTY 0.6f

/* Sustain after the kick (velocity + pressure mix; see
 * sustain_target_duty()). Set to 0 for a single click per strike. */
#define TILES_HAPTICS_SUSTAIN_ENABLED 1

/* Share of the sustain level from strike velocity vs. live pressure.
 * Pressure dominates; velocity gives a harder strike a fuller baseline.
 * Both terms are scaled to [0, MAX_SUSTAIN_DUTY] first, so this is a true
 * mix. Starting guess. */
#define SUSTAIN_VELOCITY_WEIGHT 0.3f

/* Slew on the applied sustain duty, every scan: fast attack (full swing
 * ~30 ms, so pressing harder is felt at once), slow release (~200 ms, so
 * easing off eases the motor). First guesses. */
#define SUSTAIN_ATTACK_PER_MS 0.020f
#define SUSTAIN_RELEASE_PER_MS 0.003f

/* Minimum spacing between kick STARTS (handoff: "stagger motor starts >=
 * 15 ms"): the voice ceiling limits concurrent motors, not simultaneous
 * inrush. Only a second near-simultaneous strike's haptic is delayed;
 * single notes and all MIDI are unaffected. */
#define KICK_STAGGER_MIN_GAP_MS 15u

/* TOUCH_PULSE (see haptics.h). 0.6 duty and 25 ms: at the 0.35 first
 * used, it was too weak to feel, like the original kick (and there's no
 * overdrive to help it start). Not yet re-verified on hardware. */
#define TOUCH_PULSE_DURATION_MS 25u
#define TOUCH_PULSE_DUTY 0.6f

typedef enum {
    HAPTIC_PHASE_IDLE = 0,
    HAPTIC_PHASE_PENDING, /* queued, waiting for its staggered start */
    HAPTIC_PHASE_TOUCH_PULSE,
    HAPTIC_PHASE_KICK,
    HAPTIC_PHASE_GAP,
    HAPTIC_PHASE_SUSTAIN,
} haptic_phase_t;

typedef struct {
    haptic_phase_t phase;
    uint32_t phase_start_ms; /* while PENDING: the scheduled start time */
    uint8_t kick_velocity_0_127; /* this kick's velocity (post-overdrive duty, and the sustain mix) */
    bool kick_overdrive_active;
    uint8_t sustain_target_aftertouch_0_127; /* latest pressure (key travel) */
    float sustain_current_duty;    /* applied (slewed) sustain duty */
    uint32_t sustain_last_update_ms; /* for time-based slew steps */
    uint32_t voice_seq; /* set when the pad becomes active; see steal_oldest_voice() */
} haptic_pad_state_t;

static haptic_pad_state_t s_pads[TILES_NUM_PADS];

/* Increasing; the active pad with the smallest voice_seq is the oldest
 * and is stolen first. */
static uint32_t s_next_voice_seq = 1u;

/* Earliest start for the next kick. Chains PENDING kicks
 * KICK_STAGGER_MIN_GAP_MS apart. Global: staggering is about total inrush. */
static uint32_t s_next_kick_slot_ms;

/* Global intensity, applied in set_motor_level() (every effect goes
 * through it). The floor is 0: column 1 of the expression menu is "haptics
 * off". Only set via tiles_haptics_set_intensity() with the menu's column
 * mapping, so column and value never drift. Not a saved setting: boots at
 * 1.0. */
static float s_haptic_intensity = 1.0f;
#define HAPTIC_INTENSITY_MIN 0.0f
#define HAPTIC_INTENSITY_MAX 1.0f

void tiles_haptics_set_intensity(float level_0_to_1) {
    if (level_0_to_1 < HAPTIC_INTENSITY_MIN) {
        level_0_to_1 = HAPTIC_INTENSITY_MIN;
    }
    if (level_0_to_1 > HAPTIC_INTENSITY_MAX) {
        level_0_to_1 = HAPTIC_INTENSITY_MAX;
    }
    s_haptic_intensity = level_0_to_1;
}

float tiles_haptics_get_intensity(void) {
    return s_haptic_intensity;
}

/* Deep sleep's hard "off" (tiles_haptics_set_sleep_silenced()), separate
 * from intensity ("how strong" vs "on at all"). Checked at every entry
 * point, so a decaying sustain stops at once instead of slewing toward a
 * level that never reaches the motor. */
static bool s_haptic_sleep_silenced;

static bool haptics_should_be_silent(void) {
    return s_haptic_sleep_silenced;
}

void tiles_haptics_set_sleep_silenced(bool silenced) {
    s_haptic_sleep_silenced = silenced;
    if (silenced) {
        for (uint8_t pad = 1u; pad <= TILES_NUM_PADS; pad++) {
            tiles_haptics_stop(pad);
        }
    }
}

/* Like buttons.c's set_button_led_level() but honoring active_level: motor
 * channels are active high (pin high = NMOS on), button LEDs active low. */
/* Drives one motor at level_0_to_1 as given (set_motor_level() applies the
 * haptic strength first). */
static void set_motor_level_raw(const tiles_pad_config_t *cfg, float level_0_to_1) {
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

static void set_motor_level(const tiles_pad_config_t *cfg, float level_0_to_1) {
    set_motor_level_raw(cfg, level_0_to_1 * s_haptic_intensity);
}

/* Bench test state (tiles_haptics_test_motors()): pads 1..s_test_count run
 * until s_test_until_ms. */
static uint8_t s_test_count;
static uint32_t s_test_until_ms;

void tiles_haptics_test_stop(void) {
    for (uint8_t pad = 1u; pad <= s_test_count; pad++) {
        const tiles_pad_config_t *cfg = board_pad_config(pad);
        if (cfg != NULL) {
            set_motor_level_raw(cfg, 0.0f);
        }
    }
    s_test_count = 0u;
}

uint8_t tiles_haptics_test_motors(uint8_t count, float duty_0_to_1) {
    tiles_haptics_test_stop();
    uint8_t ceiling = tiles_power_get_state().max_haptic_voices;
    if (count > ceiling) {
        count = ceiling;
    }
    if (count > TILES_NUM_PADS) {
        count = TILES_NUM_PADS;
    }
    for (uint8_t pad = 1u; pad <= count; pad++) {
        const tiles_pad_config_t *cfg = board_pad_config(pad);
        if (cfg != NULL) {
            set_motor_level_raw(cfg, duty_0_to_1);
        }
    }
    s_test_count = count;
    s_test_until_ms = to_ms_since_boot(get_absolute_time()) + TILES_HAPTICS_TEST_MAX_MS;
    return count;
}

static float kick_duty_from_velocity(uint8_t velocity_0_127) {
    float v = (float)velocity_0_127 / 127.0f;
    return MIN_KICK_DUTY + (MAX_KICK_DUTY - MIN_KICK_DUTY) * v;
}

static float sustain_duty_from_aftertouch(uint8_t aftertouch_0_127) {
    return MAX_SUSTAIN_DUTY * ((float)aftertouch_0_127 / 127.0f);
}

/* Velocity's share of the sustain mix, on the same [0, MAX_SUSTAIN_DUTY]
 * scale as pressure. Not kick_duty_from_velocity(), whose high floor would
 * inflate every sustain. */
static float sustain_base_from_velocity(uint8_t velocity_0_127) {
    return MAX_SUSTAIN_DUTY * ((float)velocity_0_127 / 127.0f);
}

/* The target the sustain slew chases (see SUSTAIN_VELOCITY_WEIGHT). */
static float sustain_target_duty(const haptic_pad_state_t *s) {
    float from_velocity = sustain_base_from_velocity(s->kick_velocity_0_127);
    float from_pressure = sustain_duty_from_aftertouch(s->sustain_target_aftertouch_0_127);
    float mixed = SUSTAIN_VELOCITY_WEIGHT * from_velocity + (1.0f - SUSTAIN_VELOCITY_WEIGHT) * from_pressure;
    if (mixed < 0.0f) {
        mixed = 0.0f;
    }
    if (mixed > MAX_SUSTAIN_DUTY) {
        mixed = MAX_SUSTAIN_DUTY;
    }
    return mixed;
}

/* TOUCH_PULSE doesn't count (it bypasses the ceiling). */
static uint8_t active_voice_count(void) {
    uint8_t count = 0;
    for (uint8_t i = 0; i < TILES_NUM_PADS; i++) {
        if (s_pads[i].phase != HAPTIC_PHASE_IDLE && s_pads[i].phase != HAPTIC_PHASE_TOUCH_PULSE) {
            count++;
        }
    }
    return count;
}

/* Stops the motor of the active pad that became active longest ago
 * (PENDING and SUSTAIN included) to free a voice. Its MIDI note is
 * untouched. False if nothing to steal (only when max_haptic_voices is 0,
 * e.g. power FAULT). */
static bool steal_oldest_voice(void) {
    int8_t oldest_idx = -1;
    uint32_t oldest_seq = 0;
    for (uint8_t i = 0; i < TILES_NUM_PADS; i++) {
        if (s_pads[i].phase == HAPTIC_PHASE_IDLE || s_pads[i].phase == HAPTIC_PHASE_TOUCH_PULSE) {
            continue;
        }
        if (oldest_idx < 0 || s_pads[i].voice_seq < oldest_seq) {
            oldest_idx = (int8_t)i;
            oldest_seq = s_pads[i].voice_seq;
        }
    }
    if (oldest_idx < 0) {
        return false;
    }

    uint8_t logical_pad = (uint8_t)(oldest_idx + 1);
    s_pads[oldest_idx].phase = HAPTIC_PHASE_IDLE;
    const tiles_pad_config_t *cfg = board_pad_config(logical_pad);
    if (cfg != NULL) {
        set_motor_level(cfg, 0.0f);
    }
    return true;
}

void tiles_haptics_init(void) {
    for (uint8_t i = 0; i < TILES_NUM_PADS; i++) {
        s_pads[i] = (haptic_pad_state_t){0};
        s_pads[i].phase = HAPTIC_PHASE_IDLE;
    }
    s_next_kick_slot_ms = 0u;
    s_haptic_sleep_silenced = false;
}

/* Starts driving the motor with the overdrive spike. Called from
 * tiles_haptics_trigger_kick() or, for a staggered kick, from scan. */
static void start_kick_now(uint8_t idx, const tiles_pad_config_t *cfg, uint8_t velocity_0_127,
                            uint32_t now_ms) {
    s_pads[idx].phase = HAPTIC_PHASE_KICK;
    s_pads[idx].phase_start_ms = now_ms;
    s_pads[idx].kick_velocity_0_127 = velocity_0_127;
    s_pads[idx].kick_overdrive_active = true;
    s_pads[idx].sustain_target_aftertouch_0_127 = 0u;
    /* Fresh envelope: sustain starts from 0 and attacks up to its target. */
    s_pads[idx].sustain_current_duty = 0.0f;
    s_pads[idx].sustain_last_update_ms = now_ms;
    set_motor_level(cfg, MAX_KICK_DUTY);
}

void tiles_haptics_trigger_kick(uint8_t logical_pad, uint8_t velocity_0_127) {
    if (haptics_should_be_silent()) {
        return;
    }
    if (logical_pad < 1u || logical_pad > TILES_NUM_PADS) {
        return;
    }
    uint8_t idx = (uint8_t)(logical_pad - 1u);

    /* The ceiling applies only to a new voice (PENDING counts as active). At
     * the ceiling, steal the oldest voice. If haptics ever drop out
     * unexpectedly, check for power FAULT first (0 voices, nothing to steal):
     * compare with main.c's "[power] mode=" print. */
    if (s_pads[idx].phase == HAPTIC_PHASE_IDLE &&
        active_voice_count() >= tiles_power_get_state().max_haptic_voices) {
        if (!steal_oldest_voice()) {
            return;
        }
    }

    const tiles_pad_config_t *cfg = board_pad_config(logical_pad);
    if (cfg == NULL) {
        return;
    }

    s_pads[idx].voice_seq = s_next_voice_seq++;

    uint32_t now_ms = to_ms_since_boot(get_absolute_time());
    uint32_t earliest = (s_next_kick_slot_ms > now_ms) ? s_next_kick_slot_ms : now_ms;

    if (earliest <= now_ms) {
        start_kick_now(idx, cfg, velocity_0_127, now_ms);
        s_next_kick_slot_ms = now_ms + KICK_STAGGER_MIN_GAP_MS;
    } else {
        /* The next stagger window is taken: queue for the one after (chains
         * correctly when several pads trigger before any starts). */
        s_pads[idx].phase = HAPTIC_PHASE_PENDING;
        s_pads[idx].phase_start_ms = earliest;
        s_pads[idx].kick_velocity_0_127 = velocity_0_127;
        s_next_kick_slot_ms = earliest + KICK_STAGGER_MIN_GAP_MS;
    }
}

/* Soft tick on touch alone. No ceiling, stagger or voice_seq. A pad
 * already doing something (sustain, a kick) is left alone: a real strike's
 * feedback wins. */
void tiles_haptics_trigger_touch_pulse(uint8_t logical_pad) {
    if (haptics_should_be_silent()) {
        return;
    }
    if (logical_pad < 1u || logical_pad > TILES_NUM_PADS) {
        return;
    }
    uint8_t idx = (uint8_t)(logical_pad - 1u);
    if (s_pads[idx].phase != HAPTIC_PHASE_IDLE) {
        /* Already giving strike feedback: don't interrupt it. */
        return;
    }
    const tiles_pad_config_t *cfg = board_pad_config(logical_pad);
    if (cfg == NULL) {
        return;
    }
    s_pads[idx].phase = HAPTIC_PHASE_TOUCH_PULSE;
    s_pads[idx].phase_start_ms = to_ms_since_boot(get_absolute_time());
    set_motor_level(cfg, TOUCH_PULSE_DUTY);
}

void tiles_haptics_set_sustain_level(uint8_t logical_pad, uint8_t aftertouch_0_127) {
    if (haptics_should_be_silent()) {
        return;
    }
    if (logical_pad < 1u || logical_pad > TILES_NUM_PADS) {
        return;
    }
    /* Target only; scan does every sustain write so the slew progresses in
     * real time. */
    s_pads[logical_pad - 1u].sustain_target_aftertouch_0_127 = aftertouch_0_127;
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

void tiles_haptics_resync_hardware(void) {
    for (uint8_t i = 0; i < TILES_NUM_PADS; i++) {
        haptic_pad_state_t *s = &s_pads[i];
        if (s->phase == HAPTIC_PHASE_IDLE || s->phase == HAPTIC_PHASE_PENDING) {
            /* PENDING hasn't driven the motor; its start will write a fresh level. */
            continue;
        }
        const tiles_pad_config_t *cfg = board_pad_config((uint8_t)(i + 1u));
        if (cfg == NULL) {
            continue;
        }
        float level;
        switch (s->phase) {
        case HAPTIC_PHASE_TOUCH_PULSE:
            level = TOUCH_PULSE_DUTY;
            break;
        case HAPTIC_PHASE_KICK:
            level = s->kick_overdrive_active ? MAX_KICK_DUTY : kick_duty_from_velocity(s->kick_velocity_0_127);
            break;
        case HAPTIC_PHASE_GAP:
            level = 0.0f;
            break;
        case HAPTIC_PHASE_SUSTAIN:
        default:
            level = s->sustain_current_duty;
            break;
        }
        set_motor_level(cfg, level);
    }
}

void tiles_haptics_scan(void) {
    uint32_t now_ms = to_ms_since_boot(get_absolute_time());

    if (s_test_count != 0u && (int32_t)(now_ms - s_test_until_ms) >= 0) {
        tiles_haptics_test_stop(); /* bench test timed out */
    }

    for (uint8_t i = 0; i < TILES_NUM_PADS; i++) {
        haptic_pad_state_t *s = &s_pads[i];
        uint8_t logical_pad = (uint8_t)(i + 1u);

        if (s->phase == HAPTIC_PHASE_PENDING && now_ms >= s->phase_start_ms) {
            const tiles_pad_config_t *cfg = board_pad_config(logical_pad);
            if (cfg != NULL) {
                start_kick_now(i, cfg, s->kick_velocity_0_127, now_ms);
            }
            continue; /* just started; nothing more this call */
        }

        if (s->phase == HAPTIC_PHASE_TOUCH_PULSE) {
            if ((now_ms - s->phase_start_ms) >= TOUCH_PULSE_DURATION_MS) {
                s->phase = HAPTIC_PHASE_IDLE;
                const tiles_pad_config_t *cfg = board_pad_config(logical_pad);
                if (cfg != NULL) {
                    set_motor_level(cfg, 0.0f);
                }
            }
            continue;
        }

        if (s->phase == HAPTIC_PHASE_KICK) {
            if (s->kick_overdrive_active && (now_ms - s->phase_start_ms) >= KICK_OVERDRIVE_MS) {
                s->kick_overdrive_active = false;
                const tiles_pad_config_t *cfg = board_pad_config(logical_pad);
                if (cfg != NULL) {
                    set_motor_level(cfg, kick_duty_from_velocity(s->kick_velocity_0_127));
                }
            }
            if ((now_ms - s->phase_start_ms) >= KICK_DURATION_MS) {
                s->phase = HAPTIC_PHASE_GAP;
                s->phase_start_ms = now_ms;
                const tiles_pad_config_t *cfg = board_pad_config(logical_pad);
                if (cfg != NULL) {
                    set_motor_level(cfg, 0.0f);
                }
            }
        } else if (s->phase == HAPTIC_PHASE_GAP && (now_ms - s->phase_start_ms) >= KICK_GAP_MS) {
#if TILES_HAPTICS_SUSTAIN_ENABLED
            s->phase = HAPTIC_PHASE_SUSTAIN;
            /* Reset the slew clock here: KICK+GAP time has passed since kick start,
             * and the first sustain step would otherwise jump straight to target
             * instead of attacking. The motor is already 0 from KICK->GAP. */
            s->sustain_current_duty = 0.0f;
            s->sustain_last_update_ms = now_ms;
#else
            /* Single click only (TILES_HAPTICS_SUSTAIN_ENABLED 0): the motor is
             * already 0; free the voice. */
            s->phase = HAPTIC_PHASE_IDLE;
#endif
        } else if (s->phase == HAPTIC_PHASE_SUSTAIN) {
            /* Every scan, so the release keeps moving while pressure is steady. */
            uint32_t elapsed = now_ms - s->sustain_last_update_ms;
            s->sustain_last_update_ms = now_ms;

            float target = sustain_target_duty(s);
            float previous_duty = s->sustain_current_duty;
            if (target > s->sustain_current_duty) {
                s->sustain_current_duty += SUSTAIN_ATTACK_PER_MS * (float)elapsed;
                if (s->sustain_current_duty > target) {
                    s->sustain_current_duty = target;
                }
            } else if (target < s->sustain_current_duty) {
                s->sustain_current_duty -= SUSTAIN_RELEASE_PER_MS * (float)elapsed;
                if (s->sustain_current_duty < target) {
                    s->sustain_current_duty = target;
                }
            }

            /* Skip the I2C write once settled; steady pressure is the common case. */
            if (fabsf(s->sustain_current_duty - previous_duty) > 0.001f) {
                const tiles_pad_config_t *cfg = board_pad_config(logical_pad);
                if (cfg != NULL) {
                    set_motor_level(cfg, s->sustain_current_duty);
                }
            }
        }
    }
}
