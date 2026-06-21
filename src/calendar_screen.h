#pragma once
#include <lvgl.h>

// Month-grid calendar viewer reached from the Tools page. Highlights today
// (matched against the RTC) and lets the user page between months. There is
// no event/note storage — this is purely a date reference.

void calendar_screen_create();
void calendar_screen_show();
bool calendar_screen_is_active();
