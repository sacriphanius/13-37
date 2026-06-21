#pragma once
#include <stdint.h>
#include <stdbool.h>

// Evil-twin (impostor AP) detector that piggybacks on the wardriver's WiFi
// beacon parser. The classic evil-twin attack stands up a rogue AP with the
// same SSID as a legitimate network but a different encryption mode (most
// commonly: clone a WPA2 network as open, hoping victims auto-connect for
// credential harvest or DNS interception). We flag exactly that signal —
// the same SSID broadcast by two BSSIDs whose auth modes don't match.
//
// Same SSID with the same auth mode across multiple BSSIDs is the normal
// pattern for enterprise rollouts and mesh extenders, so that's NOT flagged.

bool evil_twin_check(const uint8_t *bssid, const char *ssid, const char *auth,
                     int8_t rssi, uint8_t channel);

int  evil_twin_get_count();
void evil_twin_reset_count();
void evil_twin_bg_tick();   // drains queued hits to /EvilTwin/discovered.txt

// Standalone tile API -- starts/stops a dedicated WiFi beacon scan so the
// detector runs without the wardriver.
bool evil_twin_start();
void evil_twin_stop();
bool evil_twin_is_running();
