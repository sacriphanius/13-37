#pragma once
#include <lvgl.h>

// WiFi channel analyzer — hops the radio across 2.4 GHz channels 1-13 in
// promiscuous mode, measures packets/second per channel, and renders a live
// 13-bar usage chart. Bar colour conveys saturation; bar height conveys
// channel utilisation. Auto-starts on entry and tears down promiscuous mode on
// exit so the radio is freed for other tools.

void analyze_screen_create();
void analyze_screen_show();
bool analyze_screen_is_active();

// Tear down promiscuous-mode capture and restore the WiFi radio to
// whatever mode it was in before the analyzer was opened. Safe to call
// when the analyzer isn't running (no-op). The back-button handler in
// main.cpp invokes this so leaving the analyzer via the boot button
// returns the radio to its prior state instead of leaving it stuck in
// promiscuous-channel-hopping mode.
void analyze_screen_stop();
