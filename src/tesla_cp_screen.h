#pragma once
#include <lvgl.h>

// Tesla charge-port opener screen — single big "OPEN CHARGE PORT" button
// that transmits the 315 MHz US RKE burst via tesla_cp_transmit_us().
// Deliberately a dedicated screen rather than a fire-on-tap tile so the
// transmit is never accidental.

void tesla_cp_screen_create();
void tesla_cp_screen_show();
bool tesla_cp_screen_is_active();
