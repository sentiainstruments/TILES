/*
 * Replays a sensor capture (tools/tiles_control.py capture) through
 * services/bend_core.c in all three bend modes side by side, on the computer:
 *   bend_replay <capture.csv> [max_cosine_deviation] [--trace trace.csv]
 *   bend_replay --pipe [max_cosine_deviation]      (rows on stdin, trace rows out)
 * Checks the replay of the device's own mode matches what it sent, then
 * reports each note's peak bend, time bent and 95th-percentile |delta| per
 * mode. --trace writes every row's result per mode; --pipe does the same
 * live, for tools/studio/.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bend_core.h"

#define FLAG_VALID 0x01u
#define FLAG_STARTED 0x04u
#define FLAG_STEPPED 0x08u
#define FLAG_BENT 0x10u
#define FLAG_STROKE 0x20u
#define FLAG_STROKE_BEGIN 0x40u
#define FLAG_NOTE_ON 0x80u
#define FLAG_PRESSURE 0x100u

#define MODES TILES_BEND_MODE_COUNT
#define NOTE_MAX_STEPS 20000

/* Semitones per wire unit at the declared 12-semitone range. */
#define SEMITONES_PER_UNIT (12.0 / (8191.0 * 12.0 / TILES_BEND_RECEIVER_DEFAULT_RANGE_SEMITONES))

typedef struct {
    unsigned long t;
    int x, y, z;
    unsigned depth;
    float sdepth;
    unsigned bend, flags, state, velocity, aftertouch;
} row_t;

/* One mode's replay of the current note. */
typedef struct {
    tiles_bend_t b;
    bool produced;
    uint16_t bend;
    double peak, bent_ms, note_ms;
    float deltas[NOTE_MAX_STEPS];
    int n_deltas;
} lane_t;

static lane_t s_lanes[MODES];
static tiles_press_path_t s_path;
static int s_device_mode = -1; /* from the "# bend" line; -1 = unknown (Alpha 6.6: legacy) */
static float s_sensitivity = 0.065f;
static bool s_note;
static int s_notes, s_strokes_learned, s_strokes_refused;
static unsigned long s_note_t0, s_last_t;
static unsigned s_peak_depth, s_velocity;
static long s_compared, s_mismatches, s_first_mismatch = -1;

static int cmp_float(const void *a, const void *b) {
    float fa = *(const float *)a, fb = *(const float *)b;
    return (fa > fb) - (fa < fb);
}

static double p95(lane_t *l) {
    if (l->n_deltas == 0) {
        return 0.0;
    }
    qsort(l->deltas, (size_t)l->n_deltas, sizeof(float), cmp_float);
    return l->deltas[(int)(0.95 * (l->n_deltas - 1))];
}

static void print_note(void) {
    if (!s_note) {
        return;
    }
    printf("%4d  %8lu  %6lu  %5u  %3u", s_notes, s_note_t0, s_last_t - s_note_t0, s_peak_depth, s_velocity);
    for (int m = 0; m < MODES; m++) {
        lane_t *l = &s_lanes[m];
        printf("  | %6.2f %4.0f%% %.3f", l->peak, l->note_ms > 0 ? 100.0 * l->bent_ms / l->note_ms : 0.0, p95(l));
    }
    printf("\n");
}

/* "# bend mode=path sensitivity=0.065 bx=.. by=.. strokes=..": the
 * device's setup when the capture started; seeds the learner. */
static void read_setup(const char *line, bool sensitivity_given) {
    const char *p = strstr(line, "mode=");
    if (p != NULL) {
        for (int m = 0; m < MODES; m++) {
            size_t n = strlen(TILES_BEND_MODE_NAMES[m]);
            if (strncmp(p + 5, TILES_BEND_MODE_NAMES[m], n) == 0) {
                s_device_mode = m;
            }
        }
    }
    if (!sensitivity_given && (p = strstr(line, "sensitivity=")) != NULL) {
        s_sensitivity = strtof(p + 12, NULL);
    }
    tiles_press_path_reset(&s_path);
    if ((p = strstr(line, "bx=")) != NULL) {
        s_path.bx = strtof(p + 3, NULL);
    }
    if ((p = strstr(line, "by=")) != NULL) {
        s_path.by = strtof(p + 3, NULL);
    }
    if ((p = strstr(line, "strokes=")) != NULL) {
        s_path.strokes = (uint16_t)strtoul(p + 8, NULL, 10);
    }
}

/* One captured pass, in expression.c's order. */
static void replay_row(const row_t *r, long row_number) {
    if (r->flags & FLAG_STROKE_BEGIN) {
        tiles_press_path_stroke_begin(&s_path);
    }
    if (r->flags & FLAG_STROKE) {
        tiles_press_path_stroke_sample(&s_path, (int16_t)r->x, (int16_t)r->y, (int16_t)r->z);
    }
    if (r->flags & FLAG_NOTE_ON) {
        if (tiles_press_path_stroke_end(&s_path)) {
            s_strokes_learned++;
        } else {
            s_strokes_refused++;
        }
    }
    if (r->flags & (FLAG_NOTE_ON | FLAG_STARTED)) {
        print_note();
        uint16_t center = (r->flags & FLAG_STARTED) ? (uint16_t)r->bend : TILES_BEND_CENTER;
        for (int m = 0; m < MODES; m++) {
            lane_t *l = &s_lanes[m];
            tiles_bend_start(&l->b, (tiles_bend_mode_t)m, &s_path, (int16_t)r->x, (int16_t)r->y, (int16_t)r->z,
                             (float)r->depth, center, (uint32_t)r->t);
            l->produced = false;
            l->peak = l->bent_ms = l->note_ms = 0.0;
            l->n_deltas = 0;
        }
        s_note = true;
        s_notes++;
        s_note_t0 = r->t;
        s_peak_depth = 0;
        s_velocity = r->velocity;
    }
    for (int m = 0; m < MODES; m++) {
        s_lanes[m].produced = false;
    }
    /* Where the device steps: a held note's pass with a valid sample
     * (Alpha 6.6 captures only mark the steps themselves). */
    bool step = (r->flags & FLAG_STEPPED) || ((r->flags & FLAG_PRESSURE) && (r->flags & FLAG_VALID));
    if (s_note && step) {
        double dt = s_last_t ? (double)(r->t - s_last_t) : 0.0;
        for (int m = 0; m < MODES; m++) {
            lane_t *l = &s_lanes[m];
            l->produced = tiles_bend_step(&l->b, &(tiles_bend_config_t){s_sensitivity, 12.0f}, (int16_t)r->x,
                                          (int16_t)r->y, (int16_t)r->z, r->sdepth, (uint32_t)r->t, &l->bend);
            if (!l->produced) {
                continue;
            }
            double semis = ((double)l->bend - (double)l->b.center) * SEMITONES_PER_UNIT;
            l->note_ms += dt;
            if (fabs(semis) > 0.05) {
                l->bent_ms += dt;
            }
            if (fabs(semis) > fabs(l->peak)) {
                l->peak = semis;
            }
            if (l->n_deltas < NOTE_MAX_STEPS) {
                l->deltas[l->n_deltas++] = fabsf(l->b.delta);
            }
        }
        int device = s_device_mode < 0 ? TILES_BEND_MODE_LEGACY : s_device_mode;
        if (r->flags & FLAG_BENT) {
            s_compared++;
            if (!s_lanes[device].produced || s_lanes[device].bend != r->bend) {
                if (s_mismatches++ == 0) {
                    s_first_mismatch = row_number;
                }
            }
        }
        if (r->depth > s_peak_depth) {
            s_peak_depth = r->depth;
        }
    }
    s_last_t = r->t;
}

/* t,depth,smoothed_depth,state,flags, then delta and semitones per mode
 * (empty when that mode gave nothing), then the learned path. */
static void trace_row(FILE *out, const row_t *r) {
    fprintf(out, "%lu,%u,%.1f,%u,%u", r->t, r->depth, (double)r->sdepth, r->state, r->flags);
    for (int m = 0; m < MODES; m++) {
        const lane_t *l = &s_lanes[m];
        if (l->produced) {
            fprintf(out, ",%.5f,%.3f", (double)l->b.delta,
                    ((double)l->bend - (double)l->b.center) * SEMITONES_PER_UNIT);
        } else {
            fprintf(out, ",,");
        }
    }
    fprintf(out, ",%.9g,%.9g,%u\n", (double)s_path.bx, (double)s_path.by, (unsigned)s_path.strokes);
}

static void trace_header(FILE *out) {
    fprintf(out, "t_ms,depth,smoothed_depth,state,flags");
    for (int m = 0; m < MODES; m++) {
        fprintf(out, ",%s_delta,%s_semitones", TILES_BEND_MODE_NAMES[m], TILES_BEND_MODE_NAMES[m]);
    }
    fprintf(out, ",path_bx,path_by,path_strokes\n");
}

static bool parse_row(const char *line, row_t *r) {
    r->velocity = r->aftertouch = 0;
    int n = sscanf(line, "%lu,%d,%d,%d,%u,%f,%u,%u,%u,%u,%u", &r->t, &r->x, &r->y, &r->z, &r->depth, &r->sdepth,
                   &r->bend, &r->flags, &r->state, &r->velocity, &r->aftertouch);
    return n == 9 || n == 11;
}

int main(int argc, char **argv) {
    const char *in_path = NULL, *trace_path = NULL;
    bool pipe = false, sensitivity_given = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--trace") == 0 && i + 1 < argc) {
            trace_path = argv[++i];
        } else if (strcmp(argv[i], "--pipe") == 0) {
            pipe = true;
        } else if (in_path == NULL && !pipe) {
            in_path = argv[i];
        } else {
            s_sensitivity = strtof(argv[i], NULL);
            sensitivity_given = true;
        }
    }
    if (in_path == NULL && !pipe) {
        fprintf(stderr, "usage: bend_replay <capture.csv> [max_cosine_deviation] [--trace trace.csv]\n"
                        "       bend_replay --pipe [max_cosine_deviation]\n");
        return 2;
    }
    FILE *in = pipe ? stdin : fopen(in_path, "r");
    if (in == NULL) {
        perror(in_path);
        return 2;
    }
    FILE *trace = pipe ? stdout : (trace_path ? fopen(trace_path, "w") : NULL);
    if (trace) {
        trace_header(trace);
    }
    if (pipe) {
        fflush(stdout);
    } else {
        printf("                                     ");
        for (int m = 0; m < MODES; m++) {
            printf("  | %-19s", TILES_BEND_MODE_NAMES[m]);
        }
        printf("\nnote  start_ms  length  depth  vel");
        for (int m = 0; m < MODES; m++) {
            printf("  | peak  bent  p95|d|");
        }
        printf("\n");
    }

    char line[256];
    long rows = 0;
    while (fgets(line, sizeof(line), in)) {
        if (strncmp(line, "# bend", 6) == 0) {
            read_setup(line, sensitivity_given);
            continue;
        }
        row_t r;
        if (!parse_row(line, &r)) {
            continue; /* the header */
        }
        rows++;
        replay_row(&r, rows);
        if (trace) {
            trace_row(trace, &r);
            if (pipe) {
                fflush(stdout);
            }
        }
    }
    if (pipe) {
        return 0;
    }
    print_note();
    fclose(in);
    if (trace) {
        fclose(trace);
    }

    printf("%ld samples, %d notes; peak bend in semitones, time bent past 0.05, 95th-percentile |delta| "
           "(deadzone 0.025, full bend %.3f).\n",
           rows, s_notes, (double)s_sensitivity);
    printf("Press path: %d strokes learned, %d refused; now bx=%.4f by=%.4f from %u.\n", s_strokes_learned,
           s_strokes_refused, (double)s_path.bx, (double)s_path.by, (unsigned)s_path.strokes);
    const char *device = TILES_BEND_MODE_NAMES[s_device_mode < 0 ? TILES_BEND_MODE_LEGACY : s_device_mode];
    if (s_compared == 0) {
        printf("No device bend values to compare (pitch bend was off?).\n");
    } else if (s_mismatches == 0) {
        printf("Replay matches the device (%s) on all %ld bend values.\n", device, s_compared);
    } else {
        printf("Replay differs from the device (%s) on %ld of %ld bend values (first at row %ld): a different "
               "sensitivity or firmware?\n",
               device, s_mismatches, s_compared, s_first_mismatch);
    }
    return 0;
}
