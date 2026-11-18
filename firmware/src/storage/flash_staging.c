#include "flash_staging.h"

static uint8_t s_staging[TILES_FLASH_STAGING_BYTES] __attribute__((aligned(4)));

uint8_t *tiles_flash_staging(void) {
    return s_staging;
}
