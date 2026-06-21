#pragma once
#include <lvgl.h>

void timer_screen_create();
void timer_screen_show();
bool timer_screen_is_active();

// True while a countdown is actively ticking (TS_RUNNING). False when
// paused, expired, or idle. The clock-screen indicator only lights up
// for the actively-running state to match the user's mental model of
// "the timer is running."
bool timer_is_running();
