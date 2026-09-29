/* services/mpe_alloc.c -- MPE Member Channel choice (MPE spec section 3.2):
 * same-note reuse, longest-idle-first, and the steal order. */
#include "mpe_alloc.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define N 8

static tiles_mpe_slot_t slots[N];
static bool eligible[N];
static bool held[N];

static void reset(void) {
    memset(slots, 0, sizeof(slots));
    memset(held, 0, sizeof(held));
    for (int i = 0; i < N; i++) eligible[i] = true;
}

static void claim(int i, uint8_t pad, uint8_t note, uint32_t seq) {
    slots[i].in_use = true;
    slots[i].owner_pad = pad;
    slots[i].note = note;
    slots[i].claim_seq = seq;
}

static void release(int i, uint32_t seq) {
    slots[i].in_use = false;
    slots[i].release_seq = seq;
}

int main(void) {
    /* 1. fresh boot: every slot is equally idle -> lowest index. Note 0 must not
     * match a never-used slot's zeroed note field. */
    reset();
    assert(tiles_mpe_alloc_pick_free(slots, eligible, N, 60) == 0);
    assert(tiles_mpe_alloc_pick_free(slots, eligible, N, 0) == 0);

    /* 2. the old bug: play a note on 0 and release it, then a new note must NOT
     * go back onto 0 (its release tail) while never-used slots exist. */
    reset();
    claim(0, 1, 60, 1);
    release(0, 1);
    assert(tiles_mpe_alloc_pick_free(slots, eligible, N, 62) == 1);

    /* 3. once every slot has been used, the one released longest ago wins. */
    reset();
    for (int i = 0; i < N; i++) {
        claim(i, (uint8_t)(i + 1), (uint8_t)(60 + i), (uint32_t)(i + 1));
    }
    release(5, 10);
    release(2, 11);
    release(7, 12);
    assert(tiles_mpe_alloc_pick_free(slots, eligible, N, 40) == 5);

    /* 4. same note re-struck -> its own previous channel, even though another
     * free channel has been idle longer. */
    assert(tiles_mpe_alloc_pick_free(slots, eligible, N, 67) == 7); /* slot 7 last carried note 67 */
    assert(tiles_mpe_alloc_pick_free(slots, eligible, N, 62) == 2); /* slot 2 last carried note 62 */

    /* 5. same note on two free slots -> the most recently released one. */
    reset();
    claim(0, 1, 60, 1);
    release(0, 5);
    claim(1, 2, 60, 2);
    release(1, 6);
    assert(tiles_mpe_alloc_pick_free(slots, eligible, N, 60) == 1);

    /* 6. a matching note on a slot still IN USE doesn't count (that note is
     * sounding; TILES never stacks two notes on one channel). */
    reset();
    claim(3, 4, 60, 1);
    assert(tiles_mpe_alloc_pick_free(slots, eligible, N, 60) == 0);

    /* 7. ineligible slots (outside the declared zone, Song-held, reserved for
     * harmonics) are never picked, free or not. */
    reset();
    for (int i = 0; i < N; i++) eligible[i] = (i == 6);
    assert(tiles_mpe_alloc_pick_free(slots, eligible, N, 60) == 6);
    eligible[6] = false;
    assert(tiles_mpe_alloc_pick_free(slots, eligible, N, 60) == -1);
    assert(tiles_mpe_alloc_pick_free(slots, eligible, 0, 60) == -1); /* empty zone */

    /* 8. all busy -> pick_free finds nothing; steal takes the oldest claim. */
    reset();
    for (int i = 0; i < N; i++) claim(i, (uint8_t)(i + 1), (uint8_t)(60 + i), (uint32_t)(100 - i));
    assert(tiles_mpe_alloc_pick_free(slots, eligible, N, 90) == -1);
    assert(tiles_mpe_alloc_pick_steal(slots, eligible, NULL, N) == 7); /* claim_seq 93, the oldest */

    /* 9. a pedal-held note (no finger on it) goes before any played note, even
     * a newer one; among pedal-held notes the oldest claim goes first. */
    held[2] = true;
    held[4] = true;
    assert(tiles_mpe_alloc_pick_steal(slots, eligible, held, N) == 4); /* 96 is older than 98 */

    /* 10. harmonic plucks (owner_pad 0) and ineligible slots are never stolen. */
    reset();
    claim(0, 0, 72, 1); /* harmonic, oldest of all */
    claim(1, 3, 60, 5);
    claim(2, 4, 62, 2);
    eligible[2] = false;
    assert(tiles_mpe_alloc_pick_steal(slots, eligible, NULL, 3) == 1);
    eligible[1] = false;
    assert(tiles_mpe_alloc_pick_steal(slots, eligible, NULL, 3) == -1);

    printf("mpe_alloc: all tests pass\n");
    return 0;
}
