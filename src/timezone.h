#pragma once

// Persisted timezone offset.
//
// The RTC holds UTC; clock_utc_offset (in main.cpp / the clock screen) shifts it
// to local time for display. That offset used to be RAM-only and reset to 0 on
// every boot, so after a reboot the watch showed UTC until the next GPS fix.
//
// This module persists the detected offset to the SD card so the correct local
// time survives reboots (the RTC is battery-backed and keeps UTC), and refreshes
// it automatically in the background whenever WiFi connects — via NTP for the
// time and an IP-geolocation lookup for the offset, mirroring how a GPS lock
// sets both. Manual Time always wins: while it's on, none of this touches the
// clock.

void timezone_init();                            // register WiFi hook + worker
void timezone_load_on_boot();                    // restore saved offset (unless manual)
void timezone_note_detected(int offset_hours);   // persist a freshly-detected offset
void timezone_bg_tick();                         // main loop: apply background WiFi results
