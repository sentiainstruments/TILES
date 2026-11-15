#include "sample_stream.h"

#include "flash_map.h"
#include "sample_store.h"
#include "sampler.h"
#include "storage_flash.h"

#include "hardware/dma.h"
#include "hardware/regs/addressmap.h"
#include "hardware/structs/xip_ctrl.h"
#include "pico/time.h"

#include <string.h>

/* A chunk takes tens of microseconds; this is far past that. */
#define QUIESCE_TIMEOUT_US 2000u

static int s_dma = -1;
static dma_channel_config s_cfg;
static volatile bool s_paused;

static void __not_in_flash_func(stop_stream)(void) {
    xip_ctrl_hw->stream_ctr = 0u;
    while (!(xip_ctrl_hw->stat & XIP_STAT_FIFO_EMPTY)) {
        (void)xip_ctrl_hw->stream_fifo;
    }
}

static bool __not_in_flash_func(fetch_busy)(void) {
    return dma_channel_is_busy((uint)s_dma);
}

static bool __not_in_flash_func(fetch_start)(uint32_t flash_offset, uint32_t *dst, uint32_t words) {
    if (s_paused) {
        return false;
    }
    stop_stream();
    xip_ctrl_hw->stream_addr = XIP_BASE + flash_offset;
    xip_ctrl_hw->stream_ctr = words;
    dma_channel_configure((uint)s_dma, &s_cfg, dst, (const void *)XIP_AUX_BASE, words, true);
    return true;
}

static const tiles_sampler_fetch_t FETCH = {fetch_busy, fetch_start};

/* Core 0, core 1 paused: let the chunk in flight land, then stop. */
static void before_flash_write(void) {
    s_paused = true;
    uint32_t t0 = time_us_32();
    while (dma_channel_is_busy((uint)s_dma) && time_us_32() - t0 < QUIESCE_TIMEOUT_US) {
    }
    if (dma_channel_is_busy((uint)s_dma)) {
        dma_channel_abort((uint)s_dma);
    }
    stop_stream();
}

static void after_flash_write(void) {
    s_paused = false;
}

static bool flash_read(uint32_t offset, uint8_t *buf, uint32_t len) {
    memcpy(buf, (const void *)(XIP_BASE + TILES_FLASH_SAMPLE_OFFSET + offset), len);
    return true;
}

static const tiles_sample_flash_ops_t OPS = {flash_read, tiles_storage_sample_erase, tiles_storage_sample_program};

void tiles_sample_stream_init(void) {
    s_dma = dma_claim_unused_channel(false);
    bool storage = s_dma >= 0 && tiles_storage_sample_region_safe();
    if (s_dma >= 0) {
        s_cfg = dma_channel_get_default_config((uint)s_dma);
        channel_config_set_transfer_data_size(&s_cfg, DMA_SIZE_32);
        channel_config_set_read_increment(&s_cfg, false);
        channel_config_set_write_increment(&s_cfg, true);
        channel_config_set_dreq(&s_cfg, DREQ_XIP_STREAM);
        tiles_flash_set_write_hooks(before_flash_write, after_flash_write);
    }
    tiles_sampler_init(storage ? &FETCH : NULL);
    tiles_sample_store_init(storage ? &OPS : NULL, TILES_FLASH_SAMPLE_OFFSET);
}
