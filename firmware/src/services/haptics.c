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
 * measured_current_required TODOs). Total duration of the KICK phase,
 * including the overdrive spike below (KICK_OVERDRIVE_MS is a sub-window
 * of this, not additional time). */
#define KICK_DURATION_MS 30u

/* Overdrive: a brief spike at MAX_KICK_DUTY regardless of velocity, at
 * the very start of every kick, before settling to the velocity-mapped
 * duty for the rest of KICK_DURATION_MS. This is the real, physically-
 * achievable technique this hardware supports for a snappier *onset* --
 * briefly exceeding the sustained-safe duty to overcome the motor's
 * static friction/inertia faster, so even a soft strike still gets a
 * fast-starting jolt. Distinct from KICK_GAP_MS below, which is about
 * stopping quickly, not starting quickly -- overdrive can't substitute
 * for real braking (still physically impossible here, see haptics.h),
 * but it's a legitimate, standard technique for the attack. */
#define KICK_OVERDRIVE_MS 6u

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

/* Minimum spacing enforced between actual kick starts (not trigger
 * calls), per the hardware handoff's "stagger motor starts >= 15ms"
 * guidance -- multiple motors inrushing at the exact same instant is a
 * real current-budget concern the max_haptic_voices ceiling alone
 * doesn't address (that caps how many can be concurrently active, not
 * how many can *start* in the same instant). Only affects the rare case
 * of several pads struck within the same ~15ms window: a single note's
 * own kick always starts immediately (see tiles_haptics_trigger_kick),
 * so normal single-note play has zero added latency -- only a second
 * (or third...) near-simultaneous strike's *haptic* pulse gets pushed
 * back slightly, never its MIDI note-on. */
#define KICK_STAGGER_MIN_GAP_MS 15u

typedef enum {
    HAPTIC_PHASE_IDLE = 0,
    HAPTIC_PHASE_PENDING, /* queued, waiting for its staggered start time */
    HAPTIC_PHASE_KICK,
    HAPTIC_PHASE_GAP,
    HAPTIC_PHASE_SUSTAIN,
} haptic_phase_t;

typedef struct {
    haptic_phase_t phase;
    uint32_t phase_start_ms; /* while PENDING: this pad's scheduled start time, not a phase-entry timestamp */
    uint8_t kick_velocity_0_127; /* this kick's velocity -- reused at the overdrive->normal duty transition within KICK */
    bool kick_overdrive_active;
    uint8_t sustain_target_0_127; /* latest aftertouch value, applied once SUSTAIN begins */
} haptic_pad_state_t;

static haptic_pad_state_t s_pads[TILES_NUM_PADS];

/* The earliest time a not-yet-scheduled kick may actually start --
 * chains PENDING kicks KICK_STAGGER_MIN_GAP_MS apart even if several
 * trigger calls arrive before any of them actually starts (see
 * tiles_haptics_trigger_kick). Global, not per-pad: staggering is about
 * total simultaneous motor inrush across the whole board. */
static uint32_t s_next_kick_slot_ms;

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
    s_next_kick_slot_ms = 0u;
}

/* Actually begins driving the motor: the overdrive spike (see
 * KICK_OVERDRIVE_MS above), regardless of velocity. Called either
 * immediately from tiles_haptics_trigger_kick() or later from
 * tiles_haptics_scan() once a staggered PENDING pad's scheduled time
 * arrives. */
static void start_kick_now(uint8_t idx, const tiles_pad_config_t *cfg, uint8_t velocity_0_127,
                            uint32_t now_ms) {
    s_pads[idx].phase = HAPTIC_PHASE_KICK;
    s_pads[idx].phase_start_ms = now_ms;
    s_pads[idx].kick_velocity_0_127 = velocity_0_127;
    s_pads[idx].kick_overdrive_active = true;
    s_pads[idx].sustain_target_0_127 = 0u;
    set_motor_level(cfg, MAX_KICK_DUTY);
}

void tiles_haptics_trigger_kick(uint8_t logical_pad, uint8_t velocity_0_127) {
    if (logical_pad < 1u || logical_pad > TILES_NUM_PADS) {
        return;
    }
    uint8_t idx = (uint8_t)(logical_pad - 1u);

    /* Ceiling only applies to a genuinely new voice -- re-triggering an
     * already-active pad (shouldn't happen given expression.c's own
     * state machine, but cheap to guard) never gets refused. A PENDING
     * (staggered, not yet started) pad already counts as active here. */
    if (s_pads[idx].phase == HAPTIC_PHASE_IDLE &&
        active_voice_count() >= tiles_power_get_state().max_haptic_voices) {
        return;
    }

    const tiles_pad_config_t *cfg = board_pad_config(logical_pad);
    if (cfg == NULL) {
        return;
    }

    uint32_t now_ms = to_ms_since_boot(get_absolute_time());
    uint32_t earliest = (s_next_kick_slot_ms > now_ms) ? s_next_kick_slot_ms : now_ms;

    if (earliest <= now_ms) {
        start_kick_now(idx, cfg, velocity_0_127, now_ms);
        s_next_kick_slot_ms = now_ms + KICK_STAGGER_MIN_GAP_MS;
    } else {
        /* Something else already claimed the next KICK_STAGGER_MIN_GAP_MS
         * window -- queue this one for the slot after that, chaining
         * correctly even if several pads trigger before any of them
         * actually starts. */
        s_pads[idx].phase = HAPTIC_PHASE_PENDING;
        s_pads[idx].phase_start_ms = earliest;
        s_pads[idx].kick_velocity_0_127 = velocity_0_127;
        s_next_kick_slot_ms = earliest + KICK_STAGGER_MIN_GAP_MS;
    }
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

        if (s->phase == HAPTIC_PHASE_PENDING && now_ms >= s->phase_start_ms) {
            const tiles_pad_config_t *cfg = board_pad_config(logical_pad);
            if (cfg != NULL) {
                start_kick_now(i, cfg, s->kick_velocity_0_127, now_ms);
            }
            continue; /* just started -- nothing further to do this pad this call */
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
            s->phase = HAPTIC_PHASE_SUSTAIN;
            const tiles_pad_config_t *cfg = board_pad_config(logical_pad);
            if (cfg != NULL) {
                set_motor_level(cfg, sustain_duty_from_aftertouch(s->sustain_target_0_127));
            }
        }
    }
}
