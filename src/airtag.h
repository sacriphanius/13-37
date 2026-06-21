#pragma once
#include <stdint.h>
#include <stdbool.h>

// Apple Find My (AirTag-style) BLE beacon sniffer.
//
// Scans all advertising channels passively and filters for AD-type 0xFF
// (manufacturer-specific) with company id 0x004C (Apple) and sub-type 0x12
// (offline finding / Find My). Detections are dedup'd by MAC and written to
// /AirTag/discovered.txt on the SD card when one is mounted, including the
// RTC timestamp and current GPS location if a fix is available.

bool airtag_start();         // returns true if scan started (BT init OK)
void airtag_stop();
bool airtag_is_running();
int  airtag_get_count();     // total Find My adverts accepted since start
void airtag_reset_count();   // zero the count + dedup table (per-session reset)

// Inspect one BLE advertisement for an Apple Find My beacon: dedups, counts,
// and queues a hit for SD logging on a match. Returns true on a match. Lets the
// wardriver feed detections in without the standalone scanner running.
bool airtag_check(const uint8_t *mac6, int8_t rssi, uint8_t addr_type,
                  const uint8_t *adv, int adv_len);

// Drains queued detections and writes them to the SD card. Call from loop().
void airtag_bg_tick();
