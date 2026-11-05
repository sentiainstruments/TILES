/*
 * services/tune.c -- the live tuning table: starts at the defaults, refuses
 * unknown keys, bad numbers, out-of-range values and broken orders (key-up
 * depth at or above the strike depth), resets one key or all, and lists
 * every key for TUNE. Run with test/run.sh.
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "tune.h"

int main(void) {
    const tiles_tune_t *t = tiles_tune();
    assert(t->strike_depth == TILES_TUNE_DEFAULT_STRIKE_DEPTH);
    assert(t->key_up_depth == TILES_TUNE_DEFAULT_KEY_UP_DEPTH);
    assert(memcmp(&t->bend, &TILES_BEND_TUNING_DEFAULT, sizeof(t->bend)) == 0);
    assert(tiles_tune_changed() == 0u);

    assert(tiles_tune_set("strike.depth", "180") == TILES_TUNE_OK && t->strike_depth == 180.0f);
    assert(tiles_tune_set("bend.deadzone", "0.03") == TILES_TUNE_OK && t->bend.deadzone == 0.03f);
    assert(tiles_tune_set("bend.settle_ms", "80.4") == TILES_TUNE_OK && t->bend.settle_ms == 80u);
    assert(tiles_tune_changed() == 3u);

    assert(tiles_tune_set("no.such.key", "1") == TILES_TUNE_UNKNOWN_KEY);
    assert(tiles_tune_set("strike.depth", "abc") == TILES_TUNE_BAD_VALUE);
    assert(tiles_tune_set("strike.depth", NULL) == TILES_TUNE_BAD_VALUE);
    assert(tiles_tune_set("strike.depth", "5000") == TILES_TUNE_OUT_OF_RANGE);
    /* key-up must stay under the strike depth; the refused value isn't kept */
    assert(tiles_tune_set("key.up_depth", "180") == TILES_TUNE_CONFLICT && t->key_up_depth == 100u);
    assert(tiles_tune_set("velocity.time_fast_ms", "90") == TILES_TUNE_OK);
    assert(tiles_tune_set("velocity.time_slow_ms", "80") == TILES_TUNE_CONFLICT);

    assert(tiles_tune_reset("strike.depth") && t->strike_depth == TILES_TUNE_DEFAULT_STRIKE_DEPTH);
    assert(!tiles_tune_reset("no.such.key"));
    assert(tiles_tune_reset(NULL) && tiles_tune_changed() == 0u);

    char line[160];
    assert(tiles_tune_describe(0u, line, sizeof(line)));
    assert(strcmp(line, "key=sensor.averaging value=8 default=8 min=1 max=32 group=sensor") == 0);
    /* averaging the sensor can do; settle within its maximum */
    assert(tiles_tune_set("sensor.averaging", "16") == TILES_TUNE_OK && t->sensor_averaging == 16u);
    assert(tiles_tune_set("sensor.averaging", "12") == TILES_TUNE_CONFLICT && t->sensor_averaging == 16u);
    assert(tiles_tune_set("bend.settle_ms", "600") == TILES_TUNE_CONFLICT);
    assert(tiles_tune_reset(NULL));
    for (uint16_t i = 0; i < tiles_tune_count(); i++) {
        assert(tiles_tune_describe(i, line, sizeof(line)) && strstr(line, " group=") != NULL);
    }
    assert(!tiles_tune_describe(tiles_tune_count(), line, sizeof(line)));

    printf("tune: all tests pass\n");
    return 0;
}
