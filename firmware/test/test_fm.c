/* The FM engine (audio/fm_voice.c) on the computer: DX7 voice data, pitch,
 * modulation, feedback, envelopes, velocity, pressure, a fast end. */

#include "fm_voice.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int s_failures;

#define CHECK(cond, ...)                                \
    do {                                                \
        if (!(cond)) {                                  \
            printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            printf(__VA_ARGS__);                        \
            printf("\n");                               \
            s_failures++;                               \
        }                                               \
    } while (0)

#define FS 48000u
#define OP(n) ((6 - (n)) * TILES_DX7_OP_BYTES) /* DX7 operator n's bytes */

static float s_l[FS * 4u], s_r[FS * 4u];

/* Every operator a sine at the note's pitch, silent (output 0), flat
 * envelopes; the pitch envelope at rest, no LFO. */
static void plain_voice(tiles_fm_patch_t *p, uint8_t algorithm) {
    memset(p, 0, sizeof(*p));
    uint8_t *v = p->dx7;
    for (int op = 1; op <= 6; op++) {
        uint8_t *o = &v[OP(op)];
        o[0] = o[1] = o[2] = o[3] = 99;
        o[4] = o[5] = o[6] = 99;
        o[7] = 0;
        o[8] = 39;
        o[18] = 1;  /* coarse 1 */
        o[20] = 7;  /* no detune */
    }
    for (int i = 0; i < 4; i++) {
        v[TILES_DX7_PEG_RATE + i] = 99;
        v[TILES_DX7_PEG_LEVEL + i] = 50;
    }
    v[TILES_DX7_ALGORITHM] = (uint8_t)(algorithm - 1u);
    v[TILES_DX7_OSC_SYNC] = 1;
    v[TILES_DX7_LFO_SPEED] = 35;
    v[TILES_DX7_TRANSPOSE] = 24;
    memcpy(&v[TILES_DX7_NAME], "TEST      ", 10);
}

static void render(tiles_fm_voice_t *v, const tiles_fm_input_t *in, uint32_t frames) {
    memset(s_l, 0, frames * sizeof(float));
    memset(s_r, 0, frames * sizeof(float));
    for (uint32_t i = 0; i < frames; i += 16u) {
        tiles_fm_voice_render(v, in, 1.0f, &s_l[i], &s_r[i], 16u);
    }
}

static double hz_of(uint32_t from, uint32_t to) {
    double first = -1.0, last = -1.0;
    uint32_t crossings = 0;
    for (uint32_t i = from + 1u; i < to; i++) {
        if (s_l[i - 1u] < 0.0f && s_l[i] >= 0.0f) {
            double at = (double)i - s_l[i] / (s_l[i] - s_l[i - 1u]);
            if (first < 0.0) {
                first = at;
            } else {
                crossings++;
            }
            last = at;
        }
    }
    return crossings > 0u ? crossings * (double)FS / (last - first) : 0.0;
}

static double tone(uint32_t from, uint32_t n, double hz) {
    double w = 2.0 * M_PI * hz / FS, c = 2.0 * cos(w), s1 = 0.0, s2 = 0.0;
    for (uint32_t i = 0; i < n; i++) {
        double s0 = s_l[from + i] + c * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    return sqrt(s1 * s1 + s2 * s2 - c * s1 * s2) / n;
}

static double rms(uint32_t from, uint32_t to) {
    double e = 0.0;
    for (uint32_t i = from; i < to; i++) {
        e += (double)s_l[i] * s_l[i];
    }
    return sqrt(e / (to - from));
}

static double db(double x) {
    return 20.0 * log10(x > 1e-12 ? x : 1e-12);
}

static void test_data(void) {
    /* A packed voice with every field distinct comes back in place. */
    uint8_t packed[TILES_DX7_PACKED_BYTES], v[TILES_DX7_VOICE_BYTES];
    memset(packed, 0, sizeof(packed));
    uint8_t *op1 = &packed[5 * 17];
    op1[0] = 11;
    op1[11] = (uint8_t)(2u << 2 | 1u);  /* right curve 2, left 1 */
    op1[12] = (uint8_t)(9u << 3 | 5u);  /* detune 9, rate scaling 5 */
    op1[13] = (uint8_t)(6u << 2 | 3u);  /* velocity 6, amp mod 3 */
    op1[14] = 87;
    op1[15] = (uint8_t)(17u << 1 | 1u); /* coarse 17, fixed */
    op1[16] = 42;
    packed[110] = 21;
    packed[111] = (uint8_t)(1u << 3 | 6u);
    packed[116] = (uint8_t)(5u << 4 | 4u << 1 | 1u);
    packed[117] = 30;
    memcpy(&packed[118], "GLASS TINE", 10);
    tiles_dx7_unpack(packed, v);
    const uint8_t *o = &v[OP(1)];
    CHECK(o[0] == 11 && o[11] == 1 && o[12] == 2 && o[13] == 5 && o[14] == 3 && o[15] == 6 && o[16] == 87 &&
              o[17] == 1 && o[18] == 17 && o[19] == 42 && o[20] == 9,
          "an operator's fields unpacked wrong");
    CHECK(v[TILES_DX7_ALGORITHM] == 21 && v[TILES_DX7_FEEDBACK] == 6 && v[TILES_DX7_OSC_SYNC] == 1 &&
              v[TILES_DX7_LFO_SYNC] == 1 && v[TILES_DX7_LFO_WAVE] == 4 && v[TILES_DX7_PITCH_MOD_SENS] == 5 &&
              v[TILES_DX7_TRANSPOSE] == 30,
          "the voice's fields unpacked wrong");
    char name[11];
    tiles_dx7_name(v, name);
    CHECK(strcmp(name, "GLASS TINE") == 0, "name '%s'", name);
    uint8_t again[TILES_DX7_PACKED_BYTES];
    tiles_dx7_pack(v, again);
    CHECK(memcmp(again, packed, sizeof(again)) == 0, "packing the unpacked voice changed it");

    /* A bank: its checksum guards it. */
    static uint8_t bank[TILES_DX7_BANK_SYSEX_BYTES];
    memset(bank, 0, sizeof(bank));
    const uint8_t head[6] = {0xF0, 0x43, 0x00, 0x09, 0x20, 0x00};
    memcpy(bank, head, 6);
    uint32_t sum = 0;
    for (uint32_t i = 0; i < 4096u; i++) {
        bank[6u + i] = (uint8_t)((i * 7u) & 0x7Fu);
        sum += bank[6u + i];
    }
    bank[4102] = (uint8_t)((128u - (sum & 127u)) & 127u);
    bank[4103] = 0xF7;
    CHECK(tiles_dx7_bank_voices(bank, sizeof(bank)) == &bank[6], "a good bank was refused");
    bank[100] ^= 1u;
    CHECK(tiles_dx7_bank_voices(bank, sizeof(bank)) == NULL, "a bank with a bad checksum was taken");
}

static void test_pitch(void) {
    tiles_fm_patch_t p;
    tiles_fm_voice_t v;
    tiles_fm_input_t in = {69.0f, 1.0f, 0.0f, 0.0f};

    plain_voice(&p, 32);
    p.dx7[OP(1) + 16] = 99;
    memset(&v, 0, sizeof(v));
    tiles_fm_voice_start(&v, &p, &in, 0.0f, false);
    render(&v, &in, FS);
    double hz = hz_of(FS / 10u, FS);
    double h2 = db(tone(FS / 10u, FS / 2u, 880.0) / tone(FS / 10u, FS / 2u, 440.0));
    double peak = 0.0;
    for (uint32_t i = 0; i < FS; i++) {
        peak = fabs(s_l[i]) > peak ? fabs(s_l[i]) : peak;
    }
    printf("fm: one carrier %.2f Hz, 2nd harmonic %.1f dB, peak %.2f\n", hz, h2, peak);
    CHECK(fabs(hz - 440.0) < 0.5, "note 69 plays %.2f Hz", hz);
    CHECK(h2 < -50.0, "a lone sine has a 2nd harmonic at %.1f dB", h2);
    CHECK(peak > 0.3 && peak < 0.6, "a full carrier peaks at %.2f", peak);

    /* Coarse 2: an octave up. Fixed 10^2 = 100 Hz whatever the note. */
    p.dx7[OP(1) + 18] = 2;
    memset(&v, 0, sizeof(v));
    tiles_fm_voice_start(&v, &p, &in, 0.0f, false);
    render(&v, &in, FS / 2u);
    hz = hz_of(FS / 10u, FS / 2u);
    CHECK(fabs(hz - 880.0) < 1.0, "coarse 2 plays %.2f Hz", hz);
    p.dx7[OP(1) + 17] = 1;
    p.dx7[OP(1) + 18] = 2;
    p.dx7[OP(1) + 19] = 0;
    double fixed[2];
    for (int k = 0; k < 2; k++) {
        tiles_fm_input_t at = {k == 0 ? 40.0f : 80.0f, 1.0f, 0.0f, 0.0f};
        memset(&v, 0, sizeof(v));
        tiles_fm_voice_start(&v, &p, &at, 0.0f, false);
        render(&v, &at, FS / 2u);
        fixed[k] = hz_of(FS / 10u, FS / 2u);
    }
    CHECK(fabs(fixed[0] - 100.0) < 0.5 && fabs(fixed[1] - 100.0) < 0.5, "fixed 100 Hz plays %.2f / %.2f",
          fixed[0], fixed[1]);
}

static void test_modulation(void) {
    tiles_fm_patch_t p;
    tiles_fm_voice_t v;
    tiles_fm_input_t in = {57.0f, 1.0f, 0.0f, 0.0f}; /* 220 Hz */
    /* Algorithm 1: operator 2 modulates operator 1. */
    plain_voice(&p, 1);
    p.dx7[OP(1) + 16] = 99;
    p.dx7[OP(2) + 16] = 0;
    memset(&v, 0, sizeof(v));
    tiles_fm_voice_start(&v, &p, &in, 0.0f, false);
    render(&v, &in, FS / 2u);
    double plain = db(tone(FS / 10u, FS / 4u, 660.0) / tone(FS / 10u, FS / 4u, 220.0));
    p.dx7[OP(2) + 16] = 85;
    memset(&v, 0, sizeof(v));
    tiles_fm_voice_start(&v, &p, &in, 0.0f, false);
    render(&v, &in, FS / 2u);
    double fm = db(tone(FS / 10u, FS / 4u, 660.0) / tone(FS / 10u, FS / 4u, 220.0));
    printf("fm: 3rd harmonic %.1f dB unmodulated, %.1f dB at modulator level 85\n", plain, fm);
    CHECK(plain < -50.0 && fm > -25.0, "modulation didn't make sidebands");

    /* Pressure raises the modulator: brighter. */
    p.dx7[OP(2) + 16] = 60;
    p.pressure_db = 18.0f;
    double bright[2];
    for (int k = 0; k < 2; k++) {
        tiles_fm_input_t at = {57.0f, 1.0f, k == 0 ? 0.0f : 1.0f, 0.0f};
        memset(&v, 0, sizeof(v));
        tiles_fm_voice_start(&v, &p, &at, 0.0f, false);
        render(&v, &at, FS / 2u);
        bright[k] = db(tone(FS / 10u, FS / 4u, 660.0) / tone(FS / 10u, FS / 4u, 220.0));
    }
    printf("fm: 3rd harmonic %.1f dB without pressure, %.1f dB with\n", bright[0], bright[1]);
    CHECK(bright[1] > bright[0] + 6.0, "pressure didn't brighten");

    /* Feedback on operator 6 alone (algorithm 32): harmonics from nothing. */
    plain_voice(&p, 32);
    p.dx7[OP(6) + 16] = 99;
    double fb[2];
    for (int k = 0; k < 2; k++) {
        p.dx7[TILES_DX7_FEEDBACK] = (uint8_t)(k == 0 ? 0 : 7);
        memset(&v, 0, sizeof(v));
        tiles_fm_voice_start(&v, &p, &in, 0.0f, false);
        render(&v, &in, FS / 2u);
        fb[k] = db(tone(FS / 10u, FS / 4u, 440.0) / tone(FS / 10u, FS / 4u, 220.0));
    }
    printf("fm: feedback 0 -> 2nd harmonic %.1f dB, feedback 7 -> %.1f dB\n", fb[0], fb[1]);
    CHECK(fb[0] < -50.0 && fb[1] > -20.0, "feedback didn't add harmonics");
}

static void test_envelope(void) {
    tiles_fm_patch_t p;
    tiles_fm_voice_t v;
    tiles_fm_input_t in = {60.0f, 1.0f, 0.0f, 0.0f};
    plain_voice(&p, 32);
    uint8_t *o = &p.dx7[OP(1)];
    o[16] = 99;
    o[1] = 40; /* decay 1 to level 2 = 0 */
    o[5] = 0;
    memset(&v, 0, sizeof(v));
    tiles_fm_voice_start(&v, &p, &in, 0.0f, false);
    render(&v, &in, FS * 2u);
    double early = rms(FS / 50u, FS / 25u), mid = rms(FS / 4u, FS / 4u + FS / 50u), late = rms(FS, FS + FS / 50u);
    printf("fm: decay %.1f dB, %.1f dB, %.1f dB at 20 ms, 250 ms, 1 s\n", db(early), db(mid), db(late));
    CHECK(early > mid && mid > late, "rate 40 doesn't decay");

    /* Release: rate 50 is ~1.4 s from full (DX7 timing), 70 well under. */
    double ends[2];
    for (int k = 0; k < 2; k++) {
        plain_voice(&p, 32);
        p.dx7[OP(1) + 16] = 99;
        p.dx7[OP(1) + 3] = (uint8_t)(k == 0 ? 50 : 70);
        memset(&v, 0, sizeof(v));
        tiles_fm_voice_start(&v, &p, &in, 0.0f, false);
        render(&v, &in, FS / 10u);
        tiles_fm_voice_release(&v);
        uint32_t t = 0;
        while (tiles_fm_voice_active(&v) && t < FS * 4u) {
            render(&v, &in, 16u);
            t += 16u;
        }
        ends[k] = (double)t / FS;
    }
    printf("fm: release rate 50 ends after %.2f s, rate 70 after %.3f s\n", ends[0], ends[1]);
    CHECK(ends[0] > 1.0 && ends[0] < 2.0 && ends[1] < 0.3, "release times %.2f / %.2f s", ends[0], ends[1]);

    /* Given away: silent within ~10 ms. */
    plain_voice(&p, 32);
    p.dx7[OP(1) + 16] = 99;
    memset(&v, 0, sizeof(v));
    tiles_fm_voice_start(&v, &p, &in, 0.0f, false);
    render(&v, &in, FS / 10u);
    tiles_fm_voice_kill(&v);
    render(&v, &in, FS / 50u);
    CHECK(!tiles_fm_voice_active(&v), "a killed voice still sounds after 20 ms");
}

static void test_velocity(void) {
    tiles_fm_patch_t p;
    tiles_fm_voice_t v;
    double level[2][2];
    for (int sens = 0; sens < 2; sens++) {
        for (int k = 0; k < 2; k++) {
            plain_voice(&p, 32);
            p.dx7[OP(1) + 16] = 99;
            p.dx7[OP(1) + 15] = (uint8_t)(sens == 0 ? 0 : 7);
            tiles_fm_input_t in = {60.0f, k == 0 ? 0.2f : 1.0f, 0.0f, 0.0f};
            memset(&v, 0, sizeof(v));
            tiles_fm_voice_start(&v, &p, &in, 0.0f, false);
            render(&v, &in, FS / 4u);
            level[sens][k] = db(rms(FS / 10u, FS / 4u));
        }
    }
    printf("fm: velocity 0.2 vs 1: %.1f dB without sensitivity, %.1f dB at 7\n", level[0][0] - level[0][1],
           level[1][0] - level[1][1]);
    CHECK(fabs(level[0][0] - level[0][1]) < 0.5, "velocity changed a voice with no sensitivity");
    CHECK(level[1][0] - level[1][1] < -10.0, "sensitivity 7 barely follows velocity");
}

int main(void) {
    tiles_fm_init();
    test_data();
    test_pitch();
    test_modulation();
    test_envelope();
    test_velocity();
    if (s_failures == 0) {
        printf("test_fm: ok\n");
        return 0;
    }
    printf("test_fm: %d failure(s)\n", s_failures);
    return 1;
}
