#include "fm_voice.h"

/* The factory FM voices, our own, as DX7 voice data (fm_voice.h has the
 * byte order; operator 6 first). */

/* An operator: envelope rates and levels, output level, frequency (coarse
 * 0 = x0.5, fine in %), detune (7 = none), velocity sensitivity, rate
 * scaling; OPK adds keyboard level scaling around C3: left/right depth and
 * curve (0 -LIN, 1 -EXP, 2 +EXP, 3 +LIN). */
#define OPK(r1, r2, r3, r4, l1, l2, l3, l4, out, coarse, fine, detune, vel, rs, ld, rd, lc, rc) \
    r1, r2, r3, r4, l1, l2, l3, l4, 39, ld, rd, lc, rc, rs, 0, vel, out, 0, coarse, fine, detune
#define OP(r1, r2, r3, r4, l1, l2, l3, l4, out, coarse, fine, detune, vel, rs) \
    OPK(r1, r2, r3, r4, l1, l2, l3, l4, out, coarse, fine, detune, vel, rs, 0, 0, 0, 0)
/* The rest: pitch envelope at rest, algorithm (1-32), feedback, oscillator
 * sync on, the LFO still (speed 35), no transpose. */
#define VOICE(alg, fb) 99, 99, 99, 99, 50, 50, 50, 50, (alg) - 1, fb, 1, 35, 0, 0, 0, 1, 0, 0, 24

const tiles_fm_patch_t g_fm_factory[TILES_FM_FACTORY_COUNT] = {
    /* Electric piano: three pairs; the x14 pair is the tine's strike,
     * velocity brightens, pressure opens it further. */
    [TILES_FM_TINE] = {
        .dx7 = {
            OP(95, 60, 40, 70, 99, 40, 0, 0, 52, 1, 0, 7, 4, 2),
            OP(96, 30, 25, 66, 99, 80, 0, 0, 84, 1, 0, 4, 2, 3),
            OPK(95, 72, 50, 75, 99, 0, 0, 0, 56, 14, 0, 7, 6, 3, 0, 50, 0, 0),
            OP(96, 30, 25, 66, 99, 82, 0, 0, 96, 1, 0, 10, 2, 3),
            OPK(95, 48, 32, 72, 99, 55, 0, 0, 72, 1, 0, 7, 7, 3, 0, 30, 0, 0),
            OP(96, 28, 24, 66, 99, 84, 0, 0, 99, 1, 0, 7, 2, 3),
            VOICE(5, 4),
            'T', 'I', 'N', 'E', ' ', ' ', ' ', ' ', ' ', ' ',
        },
        .pressure_db = 9.0f,
        .slide_db = 6.0f,
    },
    /* Bass: a sine and its sub, each with a modulator that bites at the
     * start; pressure and slide growl. */
    [TILES_FM_SOLID] = {
        .dx7 = {
            OP(99, 0, 0, 70, 99, 99, 99, 0, 0, 1, 0, 7, 0, 0),
            OP(99, 0, 0, 70, 99, 99, 99, 0, 0, 1, 0, 7, 0, 0),
            OP(99, 62, 40, 72, 99, 30, 0, 0, 70, 1, 0, 7, 6, 2),
            OP(99, 35, 30, 72, 99, 92, 88, 0, 90, 0, 0, 7, 1, 1),
            OP(99, 52, 38, 72, 99, 62, 52, 0, 80, 1, 0, 8, 5, 2),
            OP(99, 40, 30, 72, 99, 88, 82, 0, 99, 1, 0, 7, 2, 1),
            VOICE(1, 0),
            'S', 'O', 'L', 'I', 'D', ' ', ' ', ' ', ' ', ' ',
        },
        .pressure_db = 12.0f,
        .slide_db = 9.0f,
    },
    /* Bell: inharmonic pairs (x3.5, x7.07, x1.41) ringing out. */
    [TILES_FM_CHIME] = {
        .dx7 = {
            OP(99, 45, 25, 45, 99, 40, 0, 0, 62, 1, 41, 7, 5, 2),
            OP(99, 26, 20, 42, 99, 70, 0, 0, 82, 1, 0, 4, 2, 2),
            OP(99, 58, 30, 50, 99, 0, 0, 0, 64, 7, 7, 7, 6, 3),
            OP(99, 34, 22, 44, 99, 60, 0, 0, 86, 2, 0, 9, 2, 2),
            OP(99, 36, 22, 42, 99, 55, 0, 0, 76, 3, 50, 7, 5, 2),
            OP(99, 28, 20, 42, 99, 72, 0, 0, 99, 1, 0, 7, 2, 2),
            VOICE(5, 2),
            'C', 'H', 'I', 'M', 'E', ' ', ' ', ' ', ' ', ' ',
        },
        .pressure_db = 8.0f,
        .slide_db = 6.0f,
    },
    /* Bass, three pairs: a sine whose x1 modulator snaps shut (the
     * pluck), a sub an octave down, and a fed-back pair for the bite. */
    [TILES_FM_PUNCH] = {
        .dx7 = {
            OP(99, 50, 35, 70, 99, 50, 30, 0, 58, 1, 0, 7, 4, 2),
            OP(99, 35, 25, 70, 99, 70, 55, 0, 72, 1, 0, 9, 2, 1),
            OP(99, 0, 0, 70, 99, 99, 99, 0, 0, 1, 0, 7, 0, 0),
            OP(99, 30, 20, 72, 99, 95, 90, 0, 86, 0, 0, 7, 1, 0),
            OP(99, 58, 32, 72, 99, 42, 10, 0, 80, 1, 0, 7, 5, 2),
            OP(99, 28, 20, 72, 99, 92, 85, 0, 99, 1, 0, 7, 2, 1),
            VOICE(5, 6),
            'P', 'U', 'N', 'C', 'H', ' ', ' ', ' ', ' ', ' ',
        },
        .pressure_db = 12.0f,
        .slide_db = 9.0f,
    },
    /* Brass: four carriers, slightly apart; the modulators swell in a
     * little after the note (the blare), velocity and pressure brighten. */
    [TILES_FM_BRASS] = {
        .dx7 = {
            OP(52, 38, 30, 60, 88, 78, 72, 0, 74, 1, 0, 7, 5, 2),
            OP(72, 40, 30, 62, 99, 95, 90, 0, 72, 2, 0, 7, 1, 1),
            OP(74, 40, 30, 62, 99, 95, 90, 0, 86, 1, 0, 10, 1, 1),
            OP(74, 40, 30, 62, 99, 95, 90, 0, 88, 1, 0, 4, 1, 1),
            OP(56, 40, 32, 60, 94, 82, 76, 0, 80, 1, 0, 7, 5, 2),
            OP(74, 40, 30, 62, 99, 95, 90, 0, 96, 1, 0, 7, 1, 1),
            VOICE(22, 6),
            'B', 'R', 'A', 'S', 'S', ' ', ' ', ' ', ' ', ' ',
        },
        .pressure_db = 9.0f,
        .slide_db = 6.0f,
    },
    /* Mallet: a wooden bar, a sine struck by a x4 modulator that dies at
     * once, and a x10 click on a second, shorter one. */
    [TILES_FM_MALLET] = {
        .dx7 = {
            OP(99, 0, 0, 70, 99, 99, 99, 0, 0, 1, 0, 7, 0, 0),
            OP(99, 0, 0, 70, 99, 99, 99, 0, 0, 1, 0, 7, 0, 0),
            OP(99, 88, 50, 70, 99, 0, 0, 0, 58, 10, 0, 7, 6, 3),
            OP(99, 52, 35, 55, 99, 0, 0, 0, 80, 1, 0, 9, 3, 3),
            OP(99, 80, 50, 70, 99, 0, 0, 0, 70, 4, 0, 7, 6, 3),
            OP(99, 44, 30, 52, 99, 0, 0, 0, 99, 1, 0, 7, 3, 3),
            VOICE(5, 0),
            'M', 'A', 'L', 'L', 'E', 'T', ' ', ' ', ' ', ' ',
        },
        .pressure_db = 8.0f,
        .slide_db = 6.0f,
    },
};
