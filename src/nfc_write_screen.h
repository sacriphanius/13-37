#pragma once
#include <lvgl.h>

void nfc_write_screen_create();
void nfc_write_screen_show();
bool nfc_write_screen_is_active();
void nfc_write_screen_worker(); // call from loop() — no-op when idle
