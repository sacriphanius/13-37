#pragma once
#include <lvgl.h>

void stopwatch_screen_create();
void stopwatch_screen_show();
bool stopwatch_screen_is_active();

// True while the stopwatch is actively counting up (Start pressed, not
// yet stopped). Paused stopwatches return false. Used by the clock
// screen to show a green stopwatch icon next to the battery.
bool stopwatch_is_running();
