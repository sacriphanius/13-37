#pragma once
#include <lvgl.h>
#include <stdint.h>

// Port scanner UI reached by tapping a host in the ping-sweep result list.
// Three buttons across the top row pick the technique (TCP Connect / UDP /
// Banner); three buttons below pick the preset (Top 20 / Top 100 / Range).
// When Range is selected, a lo/hi pair of text fields appears for custom
// entry. START kicks off a scan against the host the screen was opened with.

void portscan_screen_create();
// Pre-fills the target with the given IP, and optionally a resolved
// hostname for the title bar ("printer.local — TCP+UDP" vs the bare IP).
// Pass `name = nullptr` (or omit) when no hostname is available.
void portscan_screen_show(uint32_t ip_host_order, const char *name = nullptr);
bool portscan_screen_is_active();
