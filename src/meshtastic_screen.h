#pragma once
#include <lvgl.h>

void meshtastic_screen_create();
void meshtastic_screen_show();
bool meshtastic_screen_is_active();
void meshtastic_screen_refresh();  // called by meshtastic.cpp when a new message arrives
