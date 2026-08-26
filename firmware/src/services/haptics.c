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

/* Brief and strong -- a jolt, not a buzz. Unmeasured: no per-motor
 * spin-up/current data exists yet (see the board map's
 * measured_current_required TODOs). Total duration of the KICK phase,
 * including the overdrive spike below (KICK_OVERDRIVE_MS is a sub-window
 * of this, not additional time). Lengthened from an initial 30ms --
 * real feedback that the kick was "too soft for the touch." */
#define KICK_DURATION_MS 45u

/* Overdrive: a brief spike at MAX_KICK_DUTY regardless of velocity, at
 * the very start of every kick, before settling to the velocity-mapped
 * duty for the rest of KICK_DURATION_MS. This is the real, physically-
 * achievable technique this hardware supports for a snappier *onset* --
 * briefly exceeding the sustained-safe duty to overcome the motor's
 * static friction/inertia faster, so even a soft strike still gets a
 * fast-starting jolt. Distinct from KICK_GAP_MS below, which is about
 * stopping quickly, not starting quickly -- overdrive can't substitute
 * for real braking (still physically impossible here, see haptics.h),
 * but it's a legitimate, standard technique for the attack. Lengthened
 * alongside KICK_DURATION_MS above, same "boost it a lot" feedback. */
#define KICK_OVERDRIVE_MS 10u

/* The hard-zero "brake" gap after KICK -- see haptics.h's file header
 * for why this, not a soft ramp, is the achievable analog to braking on
 * this hardware. */
#define KICK_GAP_MS 8u

/* Even the weakest strike should give a felt kick, not nothing --
 * raised from an initial 0.35 (real feedback: the kick read as "too
 * soft for the touch," boost it a lot) so even a light strike still
 * lands as a real, strong jolt rather than a mild nudge; velocity still
 * has real room to be felt between here and MAX_KICK_DUTY. */
#define MIN_KICK_DUTY 0.65f
#define MAX_KICK_DUTY 1.0f

/* Capped below the kick's peak, and below 1.0, because sustain can be
 * held continuously for seconds (a long-held chord) where a brief
 * kick's inrush/thermal risk doesn't apply the same way -- unmeasured,
 * a conservative starting guess pending real current data. */
#define MAX_SUSTAIN_DUTY 0.6f

/* Continuous sustain after the kick, re-enabled now that both of its
 * real blockers are gone: the magnets are seated (previously not,
 * making the depth/aftertouch signal driving it meaningless) and
 * services/expression.c's aftertouch is now calibrated + smoothed
 * (previously raw and noisy, which is what likely read as "continuous
 * buzzing" rather than a real pressure signal the first time this was
 * tried). Reworked into a deliberate mix rather than aftertouch alone --
 * real feedback: "map haptics to velocity and key travel, this is a
 * mix" -- see sustain_target_duty() below for the blend, and
 * SUSTAIN_ATTACK_PER_MS/_RELEASE_PER_MS for the "feel stronger with
 * more pressure and ease off... slowly" shaping. */
#define TILES_HAPTICS_SUSTAIN_ENABLED 1

/* How much of the strike's own velocity colors the ongoing sustain
 * level, vs. how much comes from current pressure (key travel) --
 * pressure stays the dominant, real-time driver ("feel stronger with
 * more pressure"); velocity just gives a harder-struck note a
 * perceptibly fuller baseline throughout the hold, not only at the
 * instant of the strike. Both terms are scaled into the same
 * [0, MAX_SUSTAIN_DUTY] range before blending (see
 * sustain_base_from_velocity() below) so this weight is a true mix, not
 * one term dominating just because of how it happens to be scaled.
 * Unmeasured -- a starting guess at the ratio. */
#define SUSTAIN_VELOCITY_WEIGHT 0.3f

/* Asymmetric slew on the *applied* sustain motor duty, run every scan
 * tick (not just when aftertouch changes) so release keeps progressing
 * in real time even while held steady: fast attack (reaches a full
 * MAX_SUSTAIN_DUTY swing in ~30ms, so pressing harder is felt almost
 * immediately) but a much slower release (~200ms for a full swing) --
 * real feedback: "should feel stronger with more pressure and ease off
 * when pressure is released slowly." Both unmeasured, first guesses at
 * a feel rather than derived from anything measured. */
#define SUSTAIN_ATTACK_PER_MS 0.020f
#define SUSTAIN_RELEASE_PER_MS 0.003f

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

/* TOUCH_PULSE: a brief, soft acknowledgment fired on capacitive touch
 * alone -- see this file's header for why this exists as a distinct
 * concept from KICK. Real feedback confirmed the trigger path itself
 * was firing correctly every time (a debug capture showed
 * "touch pulse started" on every single touch, correct pad/channel) but
 * "they don't provide the haptic kick... when touched but not pressed" --
 * the real bug was TOUCH_PULSE_DUTY (0.35), the *exact* duty
 * MIN_KICK_DUTY used to be before real feedback proved it "too soft for
 * the touch" and forced it up to 0.65+ (see MIN_KICK_DUTY's own
 * comment) -- reusing the same already-invalidated duty for this new
 * feature was always going to be too weak to feel, on the same
 * hardware, for the same reason. Raised to 0.6 (still meaningfully
 * below the kick range, so it should still read as lighter/shorter than
 * a real strike) and duration extended slightly (15ms -> 25ms, still
 * far short of KICK_DURATION_MS's 45ms) to give the motor a little more
 * time to actually spin up and be felt within the pulse -- unlike KICK,
 * this has no overdrive spike to force a fast start, so a low duty
 * combined with a very short window compounds the "never gets going"
 * problem. Not yet re-verified on real hardware after this change. */
#define TOUCH_PULSE_DURATION_MS 25u
#define TOUCH_PULSE_DUTY 0.6f

typedef enum {
    HAPTIC_PHASE_IDLE = 0,
    HAPTIC_PHASE_PENDING, /* queued, waiting for its staggered start time */
    HAPTIC_PHASE_TOUCH_PULSE,
    HAPTIC_PHASE_KICK,
    HAPTIC_PHASE_GAP,
    HAPTIC_PHASE_SUSTAIN,
} haptic_phase_t;

typedef struct {
    haptic_phase_t phase;
    uint32_t phase_start_ms; /* while PENDING: this pad's scheduled start time, not a phase-entry timestamp */
    uint8_t kick_velocity_0_127; /* this kick's velocity -- reused at the overdrive->normal duty transition within KICK, and as the sustain mix's velocity term */
    bool kick_overdrive_active;
    uint8_t sustain_target_aftertouch_0_127; /* latest aftertouch (key travel/pressure) value */
    float sustain_current_duty;    /* the slewed, actually-applied sustain duty -- see SUSTAIN_ATTACK_PER_MS/_RELEASE_PER_MS */
    uint32_t sustain_last_update_ms; /* for computing real-time-elapsed slew steps, not iteration-count-based ones */
    uint32_t voice_seq; /* assigned when this pad becomes active -- see steal_oldest_voice() */
} haptic_pad_state_t;

static haptic_pad_state_t s_pads[TILES_NUM_PADS];

/* Monotonically increasing -- whichever active pad has the smallest
 * voice_seq became active longest ago, so it's the one
 * steal_oldest_voice() takes from. Never reset mid-session; 32 bits is
 * enormously more triggers than any real session will ever see. */
static uint32_t s_next_voice_seq = 1u;

/* The earliest time a not-yet-scheduled kick may actually start --
 * chains PENDING kicks KICK_STAGGER_MIN_GAP_MS apart even if several
 * trigger calls arrive before any of them actually starts (see
 * tiles_haptics_trigger_kick). Global, not per-pad: staggering is about
 * total simultaneous motor inrush across the whole board. */
static uint32_t s_next_kick_slot_ms;

/* Global intensity scalar, real feedback: "when you hold and press -
 * or + you can adjust intensity of haptics on device." Applied uniformly
 * in set_motor_level() below -- the single low-level write every haptic
 * path (KICK, its overdrive spike, SUSTAIN, TOUCH_PULSE) already funnels
 * through -- so one knob scales everything consistently rather than
 * needing a separate multiplier wired into each effect. Deliberately
 * floored above 0 (see HAPTIC_INTENSITY_MIN below), matching this file's
 * existing "even the weakest strike still gets some feel" stance
 * (MIN_KICK_DUTY, MIN_VELOCITY) -- a dedicated mute would be a separate,
 * clearer feature, not folded into this scalar's minimum. No
 * persistence yet (services/storage/ is still an empty skeleton) --
 * resets to full (1.0) on every boot. */
static float s_haptic_intensity = 1.0f;
#define HAPTIC_INTENSITY_STEP 0.1f
#define HAPTIC_INTENSITY_MIN 0.2f
#define HAPTIC_INTENSITY_MAX 1.0f

void tiles_haptics_adjust_intensity(int8_t direction) {
    s_haptic_intensity += (direction > 0) ? HAPTIC_INTENSITY_STEP : -HAPTIC_INTENSITY_STEP;
    if (s_haptic_intensity < HAPTIC_INTENSITY_MIN) {
        s_haptic_intensity = HAPTIC_INTENSITY_MIN;
    }
    if (s_haptic_intensity > HAPTIC_INTENSITY_MAX) {
        s_haptic_intensity = HAPTIC_INTENSITY_MAX;
    }
    printf("[haptics] intensity now %.2f\n", (double)s_haptic_intensity);
}

void tiles_haptics_set_intensity(float level_0_to_1) {
    if (level_0_to_1 < HAPTIC_INTENSITY_MIN) {
        level_0_to_1 = HAPTIC_INTENSITY_MIN;
    }
    if (level_0_to_1 > HAPTIC_INTENSITY_MAX) {
        level_0_to_1 = HAPTIC_INTENSITY_MAX;
    }
    s_haptic_intensity = level_0_to_1;
    printf("[haptics] intensity set directly to %.2f\n", (double)s_haptic_intensity);
}

float tiles_haptics_get_intensity(void) {
    return s_haptic_intensity;
}

/* Expression mute (services/expression_control.h's circle+square 3s
 * combo) -- a hard kill switch for every haptic effect, separate from
 * s_haptic_intensity above (that's "how strong," this is "on at all").
 * Checked at the top of every trigger/update entry point below rather
 * than folded into set_motor_level()'s scalar, so a currently-decaying
 * SUSTAIN's slew state doesn't keep silently computing toward a target
 * that will never actually reach the motor -- muting hard-stops
 * immediately instead. */
static bool s_haptic_muted;

void tiles_haptics_set_muted(bool muted) {
    s_haptic_muted = muted;
    if (muted) {
        /* Immediately cut every currently-active motor -- real feedback's
         * "hard-stop any currently-active haptics" requirement, not just
         * "stop triggering new ones." tiles_haptics_stop() is already a
         * no-op for an IDLE pad, so this is safe to call unconditionally
         * across all 24. */
        for (uint8_t pad = 1u; pad <= TILES_NUM_PADS; pad++) {
            tiles_haptics_stop(pad);
        }
    }
    printf("[haptics] muted=%d\n", (int)s_haptic_muted);
}

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
    level_0_to_1 *= s_haptic_intensity;
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

/* Velocity's own contribution to the sustain mix, scaled into the same
 * [0, MAX_SUSTAIN_DUTY] range sustain_duty_from_aftertouch() uses --
 * deliberately NOT kick_duty_from_velocity() above, whose
 * [MIN_KICK_DUTY, MAX_KICK_DUTY] range (now boosted, see MIN_KICK_DUTY's
 * own comment) would otherwise impose an inflated floor on every
 * sustain regardless of how gently a pad is actually being held. */
static float sustain_base_from_velocity(uint8_t velocity_0_127) {
    return MAX_SUSTAIN_DUTY * ((float)velocity_0_127 / 127.0f);
}

/* The blended sustain target this pad's slew (in tiles_haptics_scan())
 * chases -- see SUSTAIN_VELOCITY_WEIGHT's own comment for the mix
 * reasoning. */
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

/* TOUCH_PULSE deliberately doesn't count -- see this file's header on
 * why it bypasses the voice ceiling entirely. */
static uint8_t active_voice_count(void) {
    uint8_t count = 0;
    for (uint8_t i = 0; i < TILES_NUM_PADS; i++) {
        if (s_pads[i].phase != HAPTIC_PHASE_IDLE && s_pads[i].phase != HAPTIC_PHASE_TOUCH_PULSE) {
            count++;
        }
    }
    return count;
}

/* Real feedback: "additional notes pressed after the limit of haptic
 * voices steal the first voices pressed so new notes always have
 * priority." Finds the active pad with the smallest voice_seq (the one
 * that became active longest ago -- covers PENDING and SUSTAIN too, not
 * just KICK) and force-stops just its haptic motor, freeing a slot for
 * the new strike. Does NOT touch that pad's MIDI note -- stealing is a
 * haptics-only concept, matching this module's existing "never blocks
 * the MIDI note" stance; the stolen pad keeps sounding, it just loses
 * its motor feedback early. Returns false if there's nothing to steal
 * (only possible when max_haptic_voices is 0, e.g. power.c's FAULT
 * mode -- in that case there is no voice to sacrifice, the new kick
 * still can't be granted one). */
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
    printf("[haptics] stealing pad %u's voice for a new strike (voice ceiling)\n", logical_pad);
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
    s_haptic_muted = false;
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
    s_pads[idx].sustain_target_aftertouch_0_127 = 0u;
    /* Fresh envelope for this strike -- sustain starts from silence and
     * attacks up to its target once SUSTAIN begins, rather than
     * inheriting whatever duty the previous note on this pad ended at. */
    s_pads[idx].sustain_current_duty = 0.0f;
    s_pads[idx].sustain_last_update_ms = now_ms;
    set_motor_level(cfg, MAX_KICK_DUTY);
    /* Temporary bring-up visibility -- real feedback: "we lost the
     * haptic preview a few prompts ago." Review of trigger_kick()'s
     * voice-ceiling path, the shared PCA9685 wiring with buttons.c, and
     * three separate debug captures (zero "[haptics] dropped/stealing"
     * lines, power mode healthy the whole time) turned up no code-level
     * cause -- this confirms the actual motor-drive call is reached at
     * all, so the next test session can tell whether the trigger path is
     * the problem or something downstream of it (wiring, the PCA9685
     * write itself, a specific pad) is. */
    printf("[haptics] pad %u kick started: velocity=%u duty=%.2f\n", (uint8_t)(idx + 1u), velocity_0_127,
           (double)kick_duty_from_velocity(velocity_0_127));
}

void tiles_haptics_trigger_kick(uint8_t logical_pad, uint8_t velocity_0_127) {
    if (s_haptic_muted) {
        return;
    }
    if (logical_pad < 1u || logical_pad > TILES_NUM_PADS) {
        return;
    }
    uint8_t idx = (uint8_t)(logical_pad - 1u);

    /* Ceiling only applies to a genuinely new voice -- re-triggering an
     * already-active pad (shouldn't happen given expression.c's own
     * state machine, but cheap to guard) never gets refused. A PENDING
     * (staggered, not yet started) pad already counts as active here.
     * At the ceiling, steal the oldest active voice rather than drop
     * the new one -- real feedback: "additional notes pressed after the
     * limit of haptic voices steal the first voices pressed so new
     * notes always have priority." See steal_oldest_voice() for why
     * this only affects the stolen pad's *haptic* feedback, never its
     * MIDI note. Real feedback separately: "haptics worked at some
     * point... but they don't activate always" -- the prime suspect for
     * a drop that steal_oldest_voice() *can't* fix is power.c's
     * TILES_POWER_MODE_FAULT, whose max_haptic_voices is a hard 0 (see
     * power.c's state_for_mode()), leaving nothing to steal from
     * either. That GP22-derived mode has never been exercised on real
     * hardware (see power.c's own file header) and could plausibly be
     * flickering into FAULT transiently. The printf below makes that
     * specific case visible in the serial log, correlatable against the
     * periodic "[power] mode=..." print in main.c instead of guessed
     * at. */
    if (s_pads[idx].phase == HAPTIC_PHASE_IDLE &&
        active_voice_count() >= tiles_power_get_state().max_haptic_voices) {
        if (!steal_oldest_voice()) {
            printf("[haptics] dropped pad %u kick -- voice ceiling, nothing to steal (mode=%d max_voices=%u)\n",
                   logical_pad, (int)tiles_power_get_state().mode, tiles_power_get_state().max_haptic_voices);
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

/* A brief, soft acknowledgment on capacitive touch alone -- see this
 * file's header for why this is a separate concept from KICK. No
 * ceiling, no staggering, no voice_seq -- see active_voice_count()/
 * steal_oldest_voice()'s TOUCH_PULSE exclusions. If this pad is already
 * doing anything else (a held note's SUSTAIN, a pending/active KICK from
 * an extremely fast retrigger), leave it alone rather than interrupting
 * real feedback for a touch acknowledgment -- the pulse is a nicety, a
 * real strike's own feedback always takes priority. */
void tiles_haptics_trigger_touch_pulse(uint8_t logical_pad) {
    if (s_haptic_muted) {
        return;
    }
    if (logical_pad < 1u || logical_pad > TILES_NUM_PADS) {
        return;
    }
    uint8_t idx = (uint8_t)(logical_pad - 1u);
    if (s_pads[idx].phase != HAPTIC_PHASE_IDLE) {
        /* Temporary bring-up visibility -- real feedback: "touching the
         * top won't give any haptic pulse." Distinguishes "never even
         * called" from "called but skipped because this pad was already
         * doing something else" from "started but never felt" (wiring/
         * hardware) -- three different problems this print set can tell
         * apart. */
        printf("[haptics] pad %u touch pulse skipped -- already busy (phase=%d)\n", logical_pad,
               (int)s_pads[idx].phase);
        return;
    }
    const tiles_pad_config_t *cfg = board_pad_config(logical_pad);
    if (cfg == NULL) {
        printf("[haptics] pad %u touch pulse skipped -- no board config\n", logical_pad);
        return;
    }
    s_pads[idx].phase = HAPTIC_PHASE_TOUCH_PULSE;
    s_pads[idx].phase_start_ms = to_ms_since_boot(get_absolute_time());
    set_motor_level(cfg, TOUCH_PULSE_DUTY);
    printf("[haptics] pad %u touch pulse started: pca=0x%02x channel=%u duty=%.2f\n", logical_pad,
           cfg->haptic.pca9685_i2c_addr, cfg->haptic.channel, (double)TOUCH_PULSE_DUTY);
}

void tiles_haptics_set_sustain_level(uint8_t logical_pad, uint8_t aftertouch_0_127) {
    if (s_haptic_muted) {
        return;
    }
    if (logical_pad < 1u || logical_pad > TILES_NUM_PADS) {
        return;
    }
    /* Just updates the target -- tiles_haptics_scan() below owns every
     * actual motor write for SUSTAIN now, since the attack/release slew
     * needs to keep progressing in real time every scan tick, not only
     * on the (comparatively rare) ticks where aftertouch itself
     * changes. */
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
            /* Reset here, not just at kick-start -- KICK_DURATION_MS +
             * KICK_GAP_MS have already elapsed since then, and this
             * timestamp is what the very first SUSTAIN slew step below
             * measures its "elapsed" against. Without this reset that
             * first step would see a large elapsed value and jump
             * straight to target instead of attacking smoothly. Motor
             * itself is already at 0 from the KICK->GAP transition, so
             * no write is needed here -- the SUSTAIN branch below
             * handles the attack from there on the next iteration. */
            s->sustain_current_duty = 0.0f;
            s->sustain_last_update_ms = now_ms;
#else
            /* Single click only -- see TILES_HAPTICS_SUSTAIN_ENABLED
             * above. Motor is already at 0 from the KICK->GAP
             * transition, so no further write is needed; just free this
             * pad's voice slot. */
            s->phase = HAPTIC_PHASE_IDLE;
#endif
        } else if (s->phase == HAPTIC_PHASE_SUSTAIN) {
            /* Runs every scan tick (not just when aftertouch changes) so
             * the slow release keeps progressing in real time even
             * while pressure is held steady or updates infrequently --
             * see SUSTAIN_ATTACK_PER_MS/_RELEASE_PER_MS's own comment. */
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

            /* Skip the I2C write once settled -- held-steady pressure
             * (the common case) would otherwise re-send an identical
             * duty on every single main-loop iteration. */
            if (fabsf(s->sustain_current_duty - previous_duty) > 0.001f) {
                const tiles_pad_config_t *cfg = board_pad_config(logical_pad);
                if (cfg != NULL) {
                    set_motor_level(cfg, s->sustain_current_duty);
                }
            }
        }
    }
}
