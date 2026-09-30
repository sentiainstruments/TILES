#pragma once
#include <stdbool.h>
#include <stdint.h>
bool tud_midi_n_packet_read(uint8_t itf, uint8_t packet[4]);
