/*
 * Replays a sensor capture (tools/tiles_control.py capture) through
 * services/bend_core.c, on the computer:
 *   bend_replay <capture.csv> [max_cosine_deviation] [--out replay.csv]
 * Checks the replay matches what the device sent (with its sensitivity),
 * then reports each note's peak depth and bend. --out writes the replayed
 * bend per sample, for plotting or comparing a tuning change.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bend_core.h"

#define FLAG_STARTED 0x04u
#define FLAG_STEPPED 0x08u
#define FLAG_BENT 0x10u

/* Semitones per wire unit at the declared 12-semitone range. */
#define SEMITONES_PER_UNIT (12.0 / (8191.0 * 12.0 / TILES_BEND_RECEIVER_DEFAULT_RANGE_SEMITONES))

typedef struct {
    unsigned long t;
    int x, y, z;
    unsigned depth;
    float sdepth;
    unsigned bend, flags, state;
} row_t;

int main(int argc, char **argv) {
    const char *in_path = NULL, *out_path = NULL;
    float sensitivity = 0.065f;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
            out_path = argv[++i];
        } else if (in_path == NULL) {
            in_path = argv[i];
        } else {
            sensitivity = strtof(argv[i], NULL);
        }
    }
    if (in_path == NULL) {
        fprintf(stderr, "usage: bend_replay <capture.csv> [max_cosine_deviation] [--out replay.csv]\n");
        return 2;
    }
    FILE *in = fopen(in_path, "r");
    if (in == NULL) {
        perror(in_path);
        return 2;
    }
    FILE *out = out_path ? fopen(out_path, "w") : NULL;
    if (out) {
        fprintf(out, "t_ms,depth,bend,semitones\n");
    }

    const tiles_bend_config_t cfg = {sensitivity, 12.0f};
    tiles_bend_t b;
    bool started = false;
    char line[256];
    long rows = 0, compared = 0, mismatches = 0, first_mismatch = -1;
    int notes = 0;
    unsigned peak_depth = 0;
    double peak_bend = 0.0, bent_ms = 0.0, note_ms = 0.0;
    unsigned long note_t0 = 0, last_t = 0;

    printf("note  start_ms  length_ms  peak_depth  peak_bend_semitones  time_bent\n");
    while (fgets(line, sizeof(line), in)) {
        row_t r;
        if (sscanf(line, "%lu,%d,%d,%d,%u,%f,%u,%u,%u", &r.t, &r.x, &r.y, &r.z, &r.depth, &r.sdepth, &r.bend,
                   &r.flags, &r.state) != 9) {
            continue; /* the header */
        }
        rows++;
        if (r.flags & FLAG_STARTED) {
            if (started && notes > 0) {
                printf("%4d  %8lu  %9lu  %10u  %19.2f  %8.0f%%\n", notes, note_t0, last_t - note_t0, peak_depth,
                       peak_bend, note_ms > 0 ? 100.0 * bent_ms / note_ms : 0.0);
            }
            tiles_bend_start(&b, (int16_t)r.x, (int16_t)r.y, (int16_t)r.z, (float)r.depth, (uint16_t)r.bend,
                             (uint32_t)r.t);
            started = true;
            notes++;
            note_t0 = r.t;
            peak_depth = 0;
            peak_bend = 0.0;
            bent_ms = note_ms = 0.0;
        }
        if (started && (r.flags & FLAG_STEPPED)) {
            uint16_t bend;
            bool produced = tiles_bend_step(&b, &cfg, (int16_t)r.x, (int16_t)r.y, (int16_t)r.z, r.sdepth,
                                            (uint32_t)r.t, &bend);
            if (r.flags & FLAG_BENT) {
                compared++;
                if (!produced || bend != r.bend) {
                    if (mismatches++ == 0) {
                        first_mismatch = rows;
                    }
                }
            }
            if (produced) {
                double semis = ((double)bend - (double)b.center) * SEMITONES_PER_UNIT;
                double dt = last_t ? (double)(r.t - last_t) : 0.0;
                note_ms += dt;
                if (fabs(semis) > 0.05) {
                    bent_ms += dt;
                }
                if (fabs(semis) > fabs(peak_bend)) {
                    peak_bend = semis;
                }
                if (out) {
                    fprintf(out, "%lu,%u,%u,%.3f\n", r.t, r.depth, (unsigned)bend, semis);
                }
            }
            if (r.depth > peak_depth) {
                peak_depth = r.depth;
            }
        }
        last_t = r.t;
    }
    if (started && notes > 0) {
        printf("%4d  %8lu  %9lu  %10u  %19.2f  %8.0f%%\n", notes, note_t0, last_t - note_t0, peak_depth, peak_bend,
               note_ms > 0 ? 100.0 * bent_ms / note_ms : 0.0);
    }
    fclose(in);
    if (out) {
        fclose(out);
    }

    printf("%ld samples, %d notes. ", rows, notes);
    if (compared == 0) {
        printf("No device bend values to compare (pitch bend was off?).\n");
    } else if (mismatches == 0) {
        printf("Replay matches the device on all %ld bend values.\n", compared);
    } else {
        printf("Replay differs from the device on %ld of %ld bend values (first at row %ld): a different "
               "sensitivity or firmware?\n",
               mismatches, compared, first_mismatch);
    }
    return 0;
}
