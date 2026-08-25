#include "calibration.h"

#include "board_pins.h"
#include "hall.h"

#include "pico/stdio.h"

#include <stdio.h>

static void print_help(void) {
    printf("[calibration] commands: 'r' recapture rest baseline (hands off all pads first), "
           "'f' snapshot regular full-press depth, 'm' snapshot max-press depth, 'h' this help\n");
}

static void print_depth_snapshot(const char *label) {
    printf("[calibration] %s depth per pad (raw |Z - baseline|):\n", label);
    uint32_t sum = 0;
    uint8_t counted = 0;
    for (uint8_t pad = 1u; pad <= TILES_NUM_PADS; pad++) {
        if (!tiles_hall_last_init_ok(pad)) {
            printf("  pad %2u: (sensor never initialized, skipped)\n", pad);
            continue;
        }
        uint16_t depth = tiles_hall_get_depth(pad);
        printf("  pad %2u: depth=%u\n", pad, depth);
        sum += depth;
        counted++;
    }
    if (counted > 0) {
        printf("[calibration] %s average across %u initialized pads: %lu\n", label, counted,
               (unsigned long)(sum / counted));
    }
}

static void print_rest_baseline(void) {
    printf("[calibration] rest baseline (raw Z) per pad:\n");
    for (uint8_t pad = 1u; pad <= TILES_NUM_PADS; pad++) {
        if (!tiles_hall_last_init_ok(pad)) {
            printf("  pad %2u: (sensor never initialized, skipped)\n", pad);
            continue;
        }
        tiles_hall_sample_t s = tiles_hall_get_sample(pad);
        printf("  pad %2u: z=%d valid=%d\n", pad, s.z, s.valid);
    }
}

void tiles_calibration_init(void) {
    print_help();
}

void tiles_calibration_scan(void) {
    int c = getchar_timeout_us(0);
    if (c == PICO_ERROR_TIMEOUT || c < 0) {
        return;
    }

    switch (c) {
    case 'r':
    case 'R':
        printf("[calibration] recapturing rest baseline -- make sure nothing is touching any "
               "pad right now...\n");
        if (!tiles_hall_recapture_baseline()) {
            printf("[calibration] warning: at least one initialized pad's read failed -- its "
                   "baseline was left unchanged, see per-pad output below\n");
        }
        print_rest_baseline();
        break;
    case 'f':
    case 'F':
        print_depth_snapshot("full-press");
        break;
    case 'm':
    case 'M':
    case 'x':
    case 'X':
        print_depth_snapshot("max-press");
        break;
    case 'h':
    case 'H':
    case '?':
        print_help();
        break;
    default:
        break;
    }
}
