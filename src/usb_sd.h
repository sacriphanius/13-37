#pragma once
#include <stdbool.h>
#include <stdint.h>

// Exposes the microSD card to a host computer as a USB Mass Storage (MSC)
// device over the USB-C port — the watch shows up as a removable drive that
// the host can read and write directly.
//
// The MSC interface has to be registered with the TinyUSB stack before that
// stack starts, so a USBMSC object is constructed at static-init time and
// usb_sd_init() finishes wiring it up during setup(). At runtime the card is
// presented to / hidden from the host by toggling "media present" — exactly
// how a USB card reader behaves when a card is inserted or removed, so no USB
// re-enumeration is needed.
//
// Requires the build to be in USB-OTG mode (ARDUINO_USB_MODE=0); this is set
// in boards/lilygo-t-watch-ultra.json.

void usb_sd_init();           // call once in setup(), after the SD card mounts

bool usb_sd_start();          // present the card to the host; false if no card
void usb_sd_stop();           // hide the card from the host again

bool usb_sd_is_running();     // true while the card is exposed to the host
bool usb_sd_host_active();    // true if the host read/wrote within the last ~2 s

uint64_t usb_sd_card_bytes(); // SD card capacity in bytes (0 if no card)
