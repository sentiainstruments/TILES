#pragma once
/* Host stub: note_map.c places its crash-survivable state in __uninitialized_ram (a Pico linker
 * section that keeps it across a watchdog reset). On the host that's just a plain variable. */
#define __uninitialized_ram(name) name
