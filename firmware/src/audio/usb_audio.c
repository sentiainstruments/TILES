#include "usb_audio.h"

#include <string.h>

#include "audio_engine.h"
#include "audio_ring.h"
#include "midi_out.h"
#include "sampler.h"
#include "usb_device.h"

#include "hardware/clocks.h"
#include "hardware/structs/usb.h"
#include "pico/multicore.h"
#include "pico/time.h"
#include "tusb.h"

#define FRAMES_PER_MS (TILES_AUDIO_RATE_HZ / 1000u)
/* Core 1 keeps this much rendered ahead: a catch-up packet plus margin.
 * Sets the latency it adds (3 ms). */
#define TARGET_FRAMES (3u * FRAMES_PER_MS)
#define RENDER_FRAMES 16u
/* Packets right after a stream starts, before core 1 has caught up; their
 * padding isn't an underrun. */
#define WARMUP_PACKETS 8u
#define AUDIO_RING_WORDS 1024u /* 512 stereo frames */
#define MIDI_RING_WORDS 256u
#define DRUM_RING_WORDS 64u

TU_VERIFY_STATIC(2u * TARGET_FRAMES < AUDIO_RING_WORDS, "ring holds the target");
TU_VERIFY_STATIC(TILES_AUDIO_EP_SIZE <= 1023u, "full-speed isochronous packet limit");
TU_VERIFY_STATIC(TILES_AUDIO_TRACKS == 4u, "tiles_usb_audio_info_t.cost has 4 tracks");

static uint32_t s_audio_words[AUDIO_RING_WORDS];
static uint32_t s_midi_words[MIDI_RING_WORDS];
static tiles_spsc_t s_audio_ring; /* core 1 -> core 0 */
static tiles_spsc_t s_midi_ring;  /* core 0 -> core 1 */
static uint32_t s_drum_words[DRUM_RING_WORDS];
static tiles_spsc_t s_drum_ring; /* core 0 -> core 1: slot | velocity << 8 */
static bool s_started;
static bool s_usb_on_core1;
static volatile bool s_usb_up;

/* Core 0 (USB) side. */
static volatile bool s_streaming;
static bool s_have_sof;
static uint16_t s_last_sof;
static uint32_t s_warmup;
static uint32_t s_packets;
static uint32_t s_late_frames;
static uint32_t s_underruns;
static uint32_t s_midi_drops;

/* Core 1 side. */
static volatile uint8_t s_voices;
static volatile uint8_t s_drum_voices;
static volatile uint8_t s_load_pct;

#if TILES_DSP_BENCH
static volatile bool s_bench_request;
static volatile bool s_bench_done;
static tiles_dsp_bench_t s_bench;
static volatile uint32_t s_soak_ms;
static volatile bool s_soak_cached;

static uint32_t bench_now_us(void) {
    return time_us_32();
}

void tiles_usb_audio_bench_start(void) {
    s_bench_done = false;
    s_bench_request = true;
}

const tiles_dsp_bench_t *tiles_usb_audio_bench_result(void) {
    return s_bench_done ? &s_bench : NULL;
}

void tiles_usb_audio_bench_soak(uint32_t ms, bool cached) {
    s_soak_cached = cached;
    s_soak_ms = ms;
}
#endif

static uint32_t engine_now_us(void) {
    return time_us_32();
}

static void core1_main(void) {
    /* Flash writes on core 0 pause this core (storage_flash.h). */
    multicore_lockout_victim_init();
    if (s_usb_on_core1) {
        tiles_usb_device_init(true); /* its interrupt on this core too */
    }
    s_usb_up = true;
    int32_t block[2u * RENDER_FRAMES];
    uint32_t window_start = time_us_32();
    uint32_t busy_us = 0;
    for (;;) {
#if TILES_DSP_BENCH
        if (s_bench_request) {
            s_bench_request = false;
            tiles_dsp_bench_run(&s_bench, bench_now_us, clock_get_hz(clk_sys) / 1000000u);
            s_bench_done = true;
        }
        if (s_soak_ms != 0u) {
            tiles_dsp_bench_soak(bench_now_us, s_soak_ms, s_soak_cached);
            s_soak_ms = 0u;
        }
#endif
        if (s_usb_on_core1) {
            tud_task();
        }
        uint32_t word;
        while (tiles_spsc_pop(&s_midi_ring, &word, 1u) == 1u) {
            tiles_audio_engine_midi((uint8_t)word, (uint8_t)(word >> 8), (uint8_t)(word >> 16));
        }
        while (tiles_spsc_pop(&s_drum_ring, &word, 1u) == 1u) {
            tiles_sampler_hit((uint8_t)word, (uint8_t)(word >> 8));
        }
        bool streaming = s_streaming;
        if (streaming) {
            uint32_t t0 = time_us_32();
            while (AUDIO_RING_WORDS - tiles_spsc_space(&s_audio_ring) < 2u * TARGET_FRAMES) {
                tiles_audio_engine_render(block, RENDER_FRAMES);
                (void)tiles_spsc_push(&s_audio_ring, (const uint32_t *)block, 2u * RENDER_FRAMES);
                if (s_usb_on_core1) {
                    tud_task(); /* a block takes up to ~0.3 ms: never a frame without */
                }
            }
            busy_us += time_us_32() - t0;
        }
        s_drum_voices = tiles_sampler_voices();
        s_voices = (uint8_t)(tiles_audio_engine_active_voices() + s_drum_voices);
        uint32_t now = time_us_32();
        if (now - window_start >= 1000000u) {
            s_load_pct = (uint8_t)(busy_us / 10000u);
            busy_us = 0;
            window_start = now;
        }
        sleep_us(streaming || s_usb_on_core1 ? 200u : 1000u);
    }
}

void tiles_usb_audio_start(void) {
    if (s_started) {
        return;
    }
    tiles_spsc_init(&s_audio_ring, s_audio_words, AUDIO_RING_WORDS);
    tiles_spsc_init(&s_midi_ring, s_midi_words, MIDI_RING_WORDS);
    tiles_spsc_init(&s_drum_ring, s_drum_words, DRUM_RING_WORDS);
    tiles_audio_engine_init((uint8_t)TILES_MIDI_MPE_PITCH_BEND_RANGE_SEMITONES, clock_get_hz(clk_sys) / 1000000u,
                            engine_now_us);
    /* The stress test holds core 1 for seconds, so its builds keep USB on
     * core 0. */
#if TILES_DSP_BENCH
    s_usb_on_core1 = false;
#else
    s_usb_on_core1 = true;
#endif
    if (s_usb_on_core1) {
        tiles_usb_task_on_core1();
    } else {
        tiles_usb_device_init(true);
    }
    multicore_launch_core1(core1_main);
    uint32_t t0 = time_us_32();
    while (!s_usb_up && time_us_32() - t0 < 100000u) {
        tight_loop_contents();
    }
    s_started = true;
}

void tiles_usb_audio_drum_hit(uint8_t slot, uint8_t velocity) {
    if (!s_started) {
        return;
    }
    uint32_t word = (uint32_t)slot | ((uint32_t)velocity << 8);
    if (!tiles_spsc_push(&s_drum_ring, &word, 1u)) {
        s_midi_drops++;
    }
}

void tiles_usb_audio_midi(uint8_t status, uint8_t data1, uint8_t data2) {
    if (!s_started) {
        return;
    }
    uint32_t word = (uint32_t)status | ((uint32_t)data1 << 8) | ((uint32_t)data2 << 16);
    if (!tiles_spsc_push(&s_midi_ring, &word, 1u)) {
        s_midi_drops++;
    }
}

bool tiles_usb_audio_is_quiet(void) {
    return !s_streaming || s_voices == 0u;
}

tiles_usb_audio_info_t tiles_usb_audio_get_info(void) {
    tiles_usb_audio_info_t info = {
        .streaming = s_streaming,
        .packets = s_packets,
        .late_frames = s_late_frames,
        .underruns = s_underruns,
        .midi_drops = s_midi_drops,
        .voices = s_voices,
        .load_pct = s_load_pct,
        .shed = tiles_audio_engine_shed_count(),
        .drum_voices = s_drum_voices,
        .drum_underruns = tiles_sampler_underruns(),
    };
    for (uint8_t t = 0; t < TILES_AUDIO_TRACKS; t++) {
        info.cost[t] = (uint16_t)tiles_audio_engine_track_cost(t);
    }
    return info;
}

/* ---- TinyUSB audio callbacks (core 0, from tud_task()) ---- */

/* Called when the last packet went out; what's written here goes in the
 * next: always 48 frames, what the 48 kHz clock gives per 1 ms frame
 * (macOS stopped streaming on packets of 96 to make up a missed frame).
 * A frame without a packet loses its audio; audio.late_frames counts
 * them. The first call of a stream comes before tud_audio_set_itf_cb(). */
bool tud_audio_tx_done_pre_load_cb(uint8_t rhport, uint8_t func_id, uint8_t ep_in, uint8_t cur_alt_setting) {
    (void)rhport;
    (void)func_id;
    (void)ep_in;
    (void)cur_alt_setting;
    if (!s_streaming) {
        uint32_t stale[64];
        while (tiles_spsc_pop(&s_audio_ring, stale, 64u) > 0u) {
        }
        s_have_sof = false;
        s_warmup = WARMUP_PACKETS;
        s_streaming = true;
    }
    uint16_t sof = (uint16_t)(usb_hw->sof_rd & USB_SOF_RD_BITS);
    if (s_have_sof) {
        uint32_t elapsed = (uint32_t)((uint16_t)(sof - s_last_sof) & USB_SOF_RD_BITS);
        if (elapsed > 1u) {
            s_late_frames += elapsed - 1u;
        }
    }
    s_have_sof = true;
    s_last_sof = sof;
    static int32_t packet[2u * FRAMES_PER_MS];
    uint32_t got = tiles_spsc_pop(&s_audio_ring, (uint32_t *)packet, 2u * FRAMES_PER_MS) / 2u;
    if (got < FRAMES_PER_MS) {
        memset(&packet[2u * got], 0, (FRAMES_PER_MS - got) * 2u * sizeof(int32_t));
        if (s_warmup == 0u) {
            s_underruns++;
        }
    }
    if (s_warmup > 0u) {
        s_warmup--;
    }
    (void)tud_audio_write(packet, (uint16_t)(FRAMES_PER_MS * 2u * sizeof(int32_t)));
    s_packets++;
    return true;
}

bool tud_audio_set_itf_close_EP_cb(uint8_t rhport, tusb_control_request_t const *p_request) {
    (void)rhport;
    (void)p_request;
    s_streaming = false;
    return true;
}

bool tud_audio_set_itf_cb(uint8_t rhport, tusb_control_request_t const *p_request) {
    (void)rhport;
    if (TU_U16_LOW(p_request->wValue) == 0u) {
        s_streaming = false;
    }
    return true;
}

/* The clock: 48 kHz, fixed and always valid. */
bool tud_audio_get_req_entity_cb(uint8_t rhport, tusb_control_request_t const *p_request) {
    uint8_t control = TU_U16_HIGH(p_request->wValue);
    uint8_t entity = TU_U16_HIGH(p_request->wIndex);
    if (entity != TILES_AUDIO_ENTITY_CLOCK) {
        return false;
    }
    if (control == AUDIO_CS_CTRL_SAM_FREQ) {
        if (p_request->bRequest == AUDIO_CS_REQ_CUR) {
            static uint32_t rate = TILES_AUDIO_RATE_HZ;
            return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &rate, sizeof(rate));
        }
        if (p_request->bRequest == AUDIO_CS_REQ_RANGE) {
            static audio_control_range_4_n_t(1) range = {
                .wNumSubRanges = 1,
                .subrange[0] = {.bMin = (int32_t)TILES_AUDIO_RATE_HZ, .bMax = (int32_t)TILES_AUDIO_RATE_HZ, .bRes = 0},
            };
            return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &range, sizeof(range));
        }
    } else if (control == AUDIO_CS_CTRL_CLK_VALID && p_request->bRequest == AUDIO_CS_REQ_CUR) {
        static uint8_t valid = 1;
        return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &valid, sizeof(valid));
    }
    return false;
}

/* A host may set the rate it just read; anything else is refused. */
bool tud_audio_set_req_entity_cb(uint8_t rhport, tusb_control_request_t const *p_request, uint8_t *buf) {
    (void)rhport;
    uint8_t control = TU_U16_HIGH(p_request->wValue);
    uint8_t entity = TU_U16_HIGH(p_request->wIndex);
    if (entity == TILES_AUDIO_ENTITY_CLOCK && control == AUDIO_CS_CTRL_SAM_FREQ &&
        p_request->bRequest == AUDIO_CS_REQ_CUR && p_request->wLength == 4u) {
        uint32_t rate;
        memcpy(&rate, buf, sizeof(rate));
        return rate == TILES_AUDIO_RATE_HZ;
    }
    return false;
}
