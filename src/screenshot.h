#pragma once
#include <stdbool.h>
#include <stdint.h>

// Long-press screenshot — when the Settings → "Screenshot long press" toggle
// is on, holding any point on the screen for ≥ SCREENSHOT_LONG_PRESS_MS
// (3 s) fires a single capture of the currently-active screen and writes
// it as a 16-bit RGB565 BMP to /Screenshots/<timestamp>.bmp on the SD card.
//
// The capture path is gated by SD state (no card / USB-SD claimed → no-op)
// and by the settings switch itself, so polling is cheap and safe to call
// from the main loop unconditionally.

#define SCREENSHOT_LONG_PRESS_MS  3000

// Capture the active screen and write the BMP. Synchronous, blocks for
// ~100–300 ms (snapshot + SD write). Returns true on success; false if SD
// isn't ready, the snapshot allocation failed, or the write failed.
bool screenshot_capture();

// Polled from the main loop. Tracks how long the touchscreen has been
// held; once the user crosses SCREENSHOT_LONG_PRESS_MS and the setting
// is enabled, fires exactly one capture (no auto-repeat until release).
void screenshot_poll();
