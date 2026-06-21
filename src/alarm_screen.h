#pragma once
#include <lvgl.h>

void alarm_screen_create();
void alarm_screen_show();          // settings UI — hour/minute rollers + enable
void alarm_screen_show_ringing();  // full-screen DISMISS / SNOOZE alert
bool alarm_screen_is_active();
