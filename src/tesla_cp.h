#pragma once
#include <stdint.h>
#include <stdbool.h>

// Tesla charge-port (a.k.a. "fuel door") opener — single static OOK burst
// at 315 MHz (US RKE band). Public-research signal documented in the
// Flipper Zero community as "Tesla Charge Port"; it only opens vehicles
// running pre-2022.20 firmware that didn't add cryptographic authentication
// to the charge-port command. Use against your own vehicle / authorized
// testing only.

// Transmit the burst on 315 MHz. Returns true if the transmit completed,
// false if the radio was busy (LoRa / APRS / pager active) or the SX1262
// failed to enter FSK mode. Synchronous — blocks ~150 ms for the full
// 5-burst sequence and restores the radio to standby afterwards.
bool tesla_cp_transmit_us();

// Same signal pattern as the US variant but on 433.92 MHz — the carrier
// used by EU, JP, AU, and most "rest-of-world" Tesla deliveries. The
// SX1262's matching network on the T-Watch Ultra is closer to 868/915
// than to 433 so radiated power at 433.92 will be down a few dB, but
// the chip is in-spec for 433 MHz and the same payload reaches a real
// 433 MHz EU RKE receiver.
bool tesla_cp_transmit_eu();

// Last RadioLib error code from either tesla_cp_transmit_us() or
// tesla_cp_transmit_eu() (so the UI can surface "Failed (err -2)" type
// messages without needing to know which variant ran).
int16_t tesla_cp_last_error();
