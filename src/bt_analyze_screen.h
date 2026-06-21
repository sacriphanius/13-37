#pragma once
#include <lvgl.h>

// Bluetooth spectrum analyzer — same visual style as the WiFi analyzer but
// summarising BLE saturation by RSSI band. Reached by swiping LEFT on the
// WiFi analyze screen; a RIGHT swipe returns to it.
//
// Why RSSI buckets rather than channels: the BLE controller picks among the
// three advertising channels (37/38/39) internally and the Bluedroid scan
// callback does not report which one received the packet, so a literal
// "channel" chart is not observable. Bucketing unique devices by signal
// strength gives the analogous "how crowded is the BT spectrum around me"
// view: each bar = number of devices currently advertising at that RSSI band.

void bt_analyze_screen_create();
void bt_analyze_screen_show();
bool bt_analyze_screen_is_active();

// Remove this screen's BLE scan consumer; if no other module is still
// scanning the ble_scan_manager tears the controller down. Safe to call
// when the screen isn't active (no-op). Invoked from main.cpp's back-
// button handler so leaving the analyzer via the boot button doesn't
// leave BLE running for no one.
void bt_analyze_screen_stop();
