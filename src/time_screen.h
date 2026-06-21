#pragma once
#include <lvgl.h>

// "TIME" grid screen reached by swiping UP from the clock face. Holds the
// timepiece-flavoured tools that used to live at the bottom of the TOOLS
// grid: Alarm, Stopwatch, Timer, Calendar.

void time_screen_create();
void time_screen_show();
bool time_screen_is_active();
