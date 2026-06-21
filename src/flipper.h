#pragma once
#include <stdint.h>
#include <stdbool.h>

// Flipper Zero BLE detector.
//
// Flipper Zero devices advertise a complete local name that always starts
// with "Flipper " (e.g. "Flipper Bryan", "Flipper Roberton") and expose a
// vendor service UUID 0x3082. Detection here matches the name prefix — it's
// the most reliable cue across firmware variants. Detections are dedup'd by
// MAC and written to /Flipper/discovered.txt on the SD card when one is
// mounted, including the RTC timestamp, name, RSSI, and current GPS fix.

bool flipper_start();         // returns true if scan started (BT init OK)
void flipper_stop();
bool flipper_is_running();
int  flipper_get_count();     // total Flipper Zero adverts accepted since start
void flipper_reset_count();   // zero the count + dedup table (per-session reset)

// Inspect one BLE advertisement for a Flipper Zero. Returns true on a match
// (and dedups against recent hits). Used by both the standalone scanner and
// the wardriver — both call this from the BT task, so the dedup state needs
// no locking.
bool flipper_check(const uint8_t *mac6, int8_t rssi, uint8_t addr_type,
                   const uint8_t *adv, int adv_len);

// Drains queued detections and writes them to the SD card. Call from loop().
void flipper_bg_tick();
