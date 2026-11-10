#ifndef TILES_SHOW_LINK_H
#define TILES_SHOW_LINK_H

/* Show takeover: a computer's light show (Show Control) streams the music
 * it hears as SysEx, and while it does, TILES' LEDs are the show's: the
 * underglow strobes on the beat and the pads play the chosen animation.
 * Message format: services/README.md, "Show link". */

#include <stdbool.h>

void tiles_show_link_init(void); /* after tiles_midi_in_init() */
void tiles_show_link_service(void);
/* The show has the LEDs (on, and a frame within the last 2 s). */
bool tiles_show_link_active(void);

#endif
