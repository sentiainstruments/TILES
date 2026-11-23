/* The effects (audio/fx.c, audio/fx_memory.c) on the computer: the plate's
 * decay and stereo, the delay's echoes, the ensemble's movement, the
 * overdrive's anti-aliasing, bit-crush, the pump, lending the memory. */

#include "fx.h"
#include "fx_memory.h"
#include "synth_dsp.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int s_failures;

#define CHECK(cond, ...)                                    \
    do {                                                    \
        if (!(cond)) {                                      \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);     \
            printf(__VA_ARGS__);                            \
            printf("\n");                                   \
            s_failures++;                                   \
        }                                                   \
    } while (0)

#define FS 48000u
#define N 16u
#define SECONDS(s) ((uint32_t)((s) * FS))

static float s_l[FS * 6u], s_r[FS * 6u];
static const float k_zero[N];

/* Runs the shared effects over `frames` with one input on one send. */
enum { SEND_CHORUS, SEND_DELAY, SEND_REVERB };
static void run_busses(int send, const float *input, uint32_t frames) {
    memset(s_l, 0, frames * sizeof(float));
    memset(s_r, 0, frames * sizeof(float));
    for (uint32_t i = 0; i < frames; i += N) {
        const float *in = input != NULL ? &input[i] : k_zero;
        tiles_fx_busses(send == SEND_CHORUS ? in : k_zero, send == SEND_DELAY ? in : k_zero,
                        send == SEND_REVERB ? in : k_zero, &s_l[i], &s_r[i], N, 2.0f);
    }
}

static float s_in[FS * 6u];

static void impulse(uint32_t frames) {
    memset(s_in, 0, frames * sizeof(float));
    s_in[0] = 1.0f;
}

static double energy(const float *x, uint32_t from, uint32_t to) {
    double e = 0.0;
    for (uint32_t i = from; i < to; i++) {
        e += (double)x[i] * x[i];
    }
    return e;
}

static double db(double ratio) {
    return 10.0 * log10(ratio > 1e-30 ? ratio : 1e-30);
}

static void fresh(void) {
    tiles_fx_set_bus(&g_tiles_fx_bus_default);
    tiles_fx_init(tiles_fx_memory());
}

/* Seconds for a 20 ms window's energy to fall 60 dB below the first
 * 50-70 ms (the tail after the build-up). */
static double rt60(uint32_t frames) {
    uint32_t w = FS / 50u;
    double ref = energy(s_l, FS / 20u, FS / 20u + w) + energy(s_r, FS / 20u, FS / 20u + w);
    for (uint32_t i = FS / 20u; i + w < frames; i += w) {
        if (db((energy(s_l, i, i + w) + energy(s_r, i, i + w)) / ref) < -60.0) {
            return (double)i / FS;
        }
    }
    return (double)frames / FS;
}

static void test_reverb(void) {
    const uint32_t frames = SECONDS(6.0);
    double times[3];
    const float decays[3] = {0.2f, 0.62f, 0.9f};
    for (uint32_t k = 0; k < 3u; k++) {
        fresh();
        tiles_fx_bus_t bus = g_tiles_fx_bus_default;
        bus.reverb_decay = decays[k];
        tiles_fx_set_bus(&bus);
        impulse(frames);
        run_busses(SEND_REVERB, s_in, frames);
        times[k] = rt60(frames);
        double peak = 0.0;
        for (uint32_t i = 0; i < frames; i++) {
            CHECK(isfinite(s_l[i]) && isfinite(s_r[i]), "reverb not finite at %u", i);
            peak = fabs(s_l[i]) > peak ? fabs(s_l[i]) : peak;
        }
        CHECK(peak < 1.0, "an impulse's reverb peaks at %.2f", peak);
    }
    printf("reverb RT60: %.2f s, %.2f s, %.2f s (decay 0.2, 0.62, 0.9)\n", times[0], times[1], times[2]);
    CHECK(times[0] < times[1] && times[1] < times[2], "longer decay isn't longer");
    CHECK(times[1] > 1.0 && times[1] < 4.5, "default RT60 %.2f s out of 1-4.5 s", times[1]);

    /* The tail: wide (left and right barely correlated) and dense: the
     * normalized echo density (Abel & Huang 2006), the share of 20 ms
     * windows' samples beyond one standard deviation over a Gaussian's
     * 31.7 %, near 1 once diffuse; a few separate echoes score far lower. */
    fresh();
    impulse(SECONDS(1.0));
    run_busses(SEND_REVERB, s_in, SECONDS(1.0));
    double lr = 0.0, ll = 0.0, rr = 0.0, ned = 0.0;
    uint32_t windows = 0, from = FS / 5u, to = FS, w = FS / 50u;
    for (uint32_t i = from; i < to; i++) {
        lr += (double)s_l[i] * s_r[i];
        ll += (double)s_l[i] * s_l[i];
        rr += (double)s_r[i] * s_r[i];
    }
    for (uint32_t i = from; i + w <= to; i += w, windows++) {
        double sd = sqrt(energy(s_l, i, i + w) / w);
        uint32_t beyond = 0;
        for (uint32_t k = i; k < i + w; k++) {
            beyond += fabs(s_l[k]) > sd ? 1u : 0u;
        }
        ned += ((double)beyond / w) / 0.3173;
    }
    ned /= windows;
    double corr = lr / sqrt(ll * rr);
    printf("reverb L/R correlation %.2f, echo density %.2f\n", corr, ned);
    CHECK(fabs(corr) < 0.3, "reverb L/R correlation %.2f", corr);
    CHECK(ned > 0.85, "reverb tail sparse: echo density %.2f", ned);

    /* It falls silent and stays so. */
    run_busses(SEND_REVERB, NULL, SECONDS(5.0));
    CHECK(energy(s_l, SECONDS(4.5), SECONDS(5.0)) < 1e-8, "reverb never quiet");
}

/* Peak sample index in [from, to) of x. */
static uint32_t peak_at(const float *x, uint32_t from, uint32_t to, float *value) {
    uint32_t at = from;
    for (uint32_t i = from; i < to; i++) {
        if (fabsf(x[i]) > fabsf(x[at])) {
            at = i;
        }
    }
    *value = x[at];
    return at;
}

static void test_delay(void) {
    fresh();
    tiles_fx_bus_t bus = g_tiles_fx_bus_default;
    bus.delay_to_reverb = 0.0f;
    tiles_fx_set_bus(&bus);
    /* A short click at a send's usual level (a single sample is mostly
     * above the loop filter; full scale would meet the loop's soft clip). */
    memset(s_in, 0, SECONDS(2.0) * sizeof(float));
    for (uint32_t i = 0; i < 48u; i++) {
        s_in[i] = 0.3f * sinf((float)i * 3.14159f / 48.0f);
    }
    run_busses(SEND_DELAY, s_in, SECONDS(2.0));
    const uint32_t t = (uint32_t)(3.0f * 0.25f / 2.0f * FS); /* dotted 1/8 at 120 BPM: 18,000 */
    float a1, b1, a2, b2;
    uint32_t e1 = peak_at(s_l, t - 200u, t + 200u, &a1);
    float r_at_1 = 0.0f;
    (void)peak_at(s_r, t - 200u, t + 200u, &r_at_1);
    uint32_t e2 = peak_at(s_r, 2u * t - 200u, 2u * t + 200u, &a2);
    (void)peak_at(s_l, 2u * t - 200u, 2u * t + 200u, &b2);
    (void)b1;
    printf("delay echoes: left at %u (%.3f), right at %u (%.3f)\n", e1, (double)a1, e2, (double)a2);
    CHECK(abs((int)e1 - (int)(t + 24u)) < 8, "first echo at %u, not ~%u", e1, t + 24u);
    CHECK(fabsf(a1) > 0.28f, "first echo %.2f of 0.3", (double)a1);
    CHECK(fabsf(r_at_1) < 0.01f, "the first echo is on the right too (%.3f)", (double)r_at_1);
    CHECK(abs((int)e2 - (int)(2u * t + 24u)) < 12, "second echo at %u, not ~%u", e2, 2u * t + 24u);
    CHECK(fabsf(a2 / a1) > 0.28f && fabsf(a2 / a1) < 0.4f, "second echo %.2f of the first, feedback 0.38",
          (double)(a2 / a1));
    CHECK(fabsf(b2) < 0.01f, "the second echo is on the left too (%.3f)", (double)b2);
}

static void test_chorus(void) {
    fresh();
    const uint32_t frames = SECONDS(2.0);
    for (uint32_t i = 0; i < frames; i++) {
        s_in[i] = 0.5f * sinf(2.0f * 3.14159265f * 1000.0f * (float)i / FS);
    }
    run_busses(SEND_CHORUS, s_in, frames);
    double lr = 0.0, ll = 0.0, rr = 0.0;
    for (uint32_t i = FS / 10u; i < frames; i++) {
        lr += (double)s_l[i] * s_r[i];
        ll += (double)s_l[i] * s_l[i];
        rr += (double)s_r[i] * s_r[i];
    }
    double corr = lr / sqrt(ll * rr);
    /* Zero crossings of the left: the moving delay bends the 1 kHz pitch. */
    double min_period = 1e9, max_period = 0.0;
    double last = -1.0;
    for (uint32_t i = FS / 10u; i < frames; i++) {
        if (s_l[i - 1u] < 0.0f && s_l[i] >= 0.0f) {
            double at = (double)i - s_l[i] / (s_l[i] - s_l[i - 1u]);
            if (last > 0.0) {
                double p = at - last;
                min_period = p < min_period ? p : min_period;
                max_period = p > max_period ? p : max_period;
            }
            last = at;
        }
    }
    double cents = 1200.0 * log2(max_period / min_period);
    printf("chorus: L/R correlation %.2f, pitch movement %.1f cents\n", corr, cents);
    CHECK(corr < 0.9, "ensemble not wide: correlation %.2f", corr);
    CHECK(cents > 3.0 && cents < 60.0, "ensemble pitch movement %.1f cents", cents);
}

/* Goertzel: the magnitude of frequency `hz` over x[0..n). */
static double tone(const float *x, uint32_t n, double hz) {
    double w = 2.0 * M_PI * hz / FS, c = 2.0 * cos(w), s1 = 0.0, s2 = 0.0;
    for (uint32_t i = 0; i < n; i++) {
        double s0 = x[i] + c * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    return sqrt(s1 * s1 + s2 * s2 - c * s1 * s2) / n;
}

static void test_overdrive(void) {
    /* A 4.7 kHz sine driven hard: its odd harmonics above 24 kHz fold back
     * between the real ones. ADAA must lower them against plain clipping. */
    const uint32_t frames = FS / 2u;
    const double hz = 4700.0;
    tiles_fx_amounts_t fx = {.drive = 1.0f};
    tiles_fx_insert_t s;
    tiles_fx_insert_reset(&s);
    static float l[FS], r[FS], naive[FS];
    for (uint32_t i = 0; i < frames; i++) {
        l[i] = r[i] = 0.3f * sinf(2.0f * 3.14159265f * (float)hz * (float)i / FS);
        naive[i] = synth_sat(l[i] * 25.0f + 0.15f) / 5.0f;
    }
    for (uint32_t i = 0; i < frames; i += N) {
        tiles_fx_insert(&s, &fx, &l[i], &r[i], N);
    }
    const uint32_t skip = FS / 10u;
    /* Folded: 5th at 23.5 kHz -> 24.5 kHz (no: 23.5 is in band), 7th
     * 32.9 kHz -> 15.1 kHz, 9th 42.3 kHz -> 5.7 kHz, 11th 51.7 kHz -> 3.7 kHz. */
    const double aliases[3] = {15100.0, 5700.0, 3700.0};
    double adaa = 0.0, plain = 0.0;
    for (uint32_t k = 0; k < 3u; k++) {
        adaa += tone(&l[skip], frames - skip, aliases[k]) / tone(&l[skip], frames - skip, hz);
        plain += tone(&naive[skip], frames - skip, aliases[k]) / tone(&naive[skip], frames - skip, hz);
    }
    printf("overdrive aliases: %.1f dB with ADAA, %.1f dB clipped plainly\n", 20.0 * log10(adaa),
           20.0 * log10(plain));
    CHECK(20.0 * log10(adaa) < 20.0 * log10(plain) - 6.0, "ADAA doesn't lower the aliases");
    double mean = 0.0, peak = 0.0;
    for (uint32_t i = skip; i < frames; i++) {
        mean += l[i];
        peak = fabs(l[i]) > peak ? fabs(l[i]) : peak;
    }
    mean /= (double)(frames - skip);
    CHECK(fabs(mean) < 0.005, "overdrive leaves DC %.4f", mean);
    CHECK(peak < 0.5, "overdrive peaks at %.2f", peak);
}

static void test_crush(void) {
    tiles_fx_amounts_t fx = {.crush = 1.0f};
    tiles_fx_insert_t s;
    tiles_fx_insert_reset(&s);
    static float l[FS / 10u], r[FS / 10u];
    for (uint32_t i = 0; i < FS / 10u; i++) {
        l[i] = r[i] = 0.9f * sinf(2.0f * 3.14159265f * 100.0f * (float)i / FS);
    }
    for (uint32_t i = 0; i < FS / 10u; i += N) {
        tiles_fx_insert(&s, &fx, &l[i], &r[i], N);
    }
    /* 5 bits: steps of 1/16; held for ~23 samples at ~2.1 kHz. */
    uint32_t changes = 0, off_grid = 0;
    for (uint32_t i = 1; i < FS / 10u; i++) {
        changes += l[i] != l[i - 1u] ? 1u : 0u;
        float steps = l[i] * 16.0f;
        off_grid += fabsf(steps - roundf(steps)) > 1e-4f ? 1u : 0u;
    }
    printf("crush: %u changes in 0.1 s, %u off the 5-bit grid\n", changes, off_grid);
    CHECK(off_grid == 0u, "%u samples off the 5-bit grid", off_grid);
    CHECK(changes < 220u, "crush held %u changes in 0.1 s (2.1 kHz: ~210 at most)", changes);
}

static void test_pump(void) {
    fresh();
    tiles_fx_amounts_t fx = {.pump = 0.8f};
    tiles_fx_insert_t s;
    tiles_fx_insert_reset(&s);
    float l[N], r[N];
    tiles_fx_pump_kick();
    float at_2ms = 0.0f, at_end = 0.0f;
    const uint32_t blocks = SECONDS(0.6) / N;
    for (uint32_t b = 0; b < blocks; b++) {
        for (uint32_t i = 0; i < N; i++) {
            l[i] = r[i] = 1.0f;
        }
        tiles_fx_block_begin(N, 2.0f);
        tiles_fx_insert(&s, &fx, l, r, N);
        if (b == SECONDS(0.002) / N) {
            at_2ms = l[N - 1u];
        }
        at_end = l[N - 1u];
    }
    printf("pump: %.2f at 2 ms, %.2f at 0.6 s\n", (double)at_2ms, (double)at_end);
    CHECK(fabsf(at_2ms - 0.2f) < 0.05f, "pump at 2 ms %.2f, not 0.2", (double)at_2ms);
    CHECK(at_end > 0.999f, "pump didn't recover (%.3f)", (double)at_end);
}

static void test_layout(void) {
    CHECK(tiles_fx_reverb_extent(false) <= 4096u, "the plate's front overflows (%u)", tiles_fx_reverb_extent(false));
    CHECK(tiles_fx_reverb_extent(true) <= 16384u, "the plate's tank overflows (%u)", tiles_fx_reverb_extent(true));
}

static void test_lending(void) {
    fresh();
    void *m = tiles_fx_memory_borrow();
    CHECK(m == tiles_fx_memory(), "not lent while the effects don't run");
    tiles_fx_set_bypass(true); /* what core 1 does when it lets go */
    impulse(SECONDS(0.5));
    run_busses(SEND_REVERB, s_in, SECONDS(0.5));
    CHECK(energy(s_l, 0u, SECONDS(0.5)) == 0.0, "the effects sound while their memory is lent");
    memset(m, 0x55, 4096u); /* the capture's records */
    tiles_fx_memory_return();
    tiles_fx_memory_service(); /* core 1 takes it back, cleared */
    CHECK(!tiles_fx_memory_lent(), "still lent");
    run_busses(SEND_REVERB, NULL, SECONDS(0.5));
    CHECK(energy(s_l, 0u, SECONDS(0.5)) == 0.0, "the capture's records came out of the reverb");
}

int main(void) {
    test_reverb();
    test_delay();
    test_chorus();
    test_overdrive();
    test_crush();
    test_pump();
    test_layout();
    test_lending();
    if (s_failures == 0) {
        printf("test_fx: ok\n");
        return 0;
    }
    printf("test_fx: %d failure(s)\n", s_failures);
    return 1;
}
