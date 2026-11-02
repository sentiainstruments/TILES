#pragma once
#include <stdint.h>
typedef uint64_t absolute_time_t;
extern uint32_t g_now_ms;
static inline absolute_time_t get_absolute_time(void) { return g_now_ms; }
static inline uint32_t to_ms_since_boot(absolute_time_t t) { return (uint32_t)t; }
static inline uint64_t to_us_since_boot(absolute_time_t t) { return (uint64_t)t * 1000u; }
