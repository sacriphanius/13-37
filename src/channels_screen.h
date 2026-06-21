#pragma once
#include <lvgl.h>

// Settings -> Channels sub-screen for the meshtastic flow.
// Lists the 4 channel slots (slot 0 = LongFast hardcoded, slots 1..3
// user-defined). Each row shows the channel name + a PSK preview,
// plus an enable switch and a "set active" marker. Tap a row to set
// that channel as the active outgoing TX channel; toggle the switch
// to enable / disable RX on that channel. Enabling an empty slot
// auto-generates a random AES-128 PSK so the channel is usable
// without leaving the screen.
//
// Reached from the configuration screen via the new "Channels..."
// link card. Back-swipe (LV_DIR_LEFT) returns to the configuration
// screen.
void channels_screen_create();
void channels_screen_show();
bool channels_screen_is_active();
