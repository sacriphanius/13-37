#include "tesla_cp.h"
#include "lora_screen.h"
#include "aprs.h"
#include "pager.h"
#include <Arduino.h>
#include <LilyGoLib.h>

// ---- Signal definition -----------------------------------------------------
//
// The Tesla charge-port command is OOK-modulated short bursts on 315 MHz
// (US) at roughly 2.5 kbps. Each "1" bit transmits a ~400 µs carrier pulse;
// each "0" bit transmits ~400 µs of silence. The full burst is the published
// pattern, repeated 5 times with a small inter-burst gap — the same redundancy
// pattern Tesla's own key fob uses.
//
// The SX1262 doesn't have a true OOK modulator (only LoRa / FSK / GFSK /
// BPSK), so we drive it as FSK with the smallest practical deviation: an
// "on" bit hops the carrier up a few kHz, an "off" bit hops it down. A
// narrow-band 315 MHz OOK receiver in a vehicle treats the deviation-shifted
// carrier as continuous signal during "on" bits — the Tesla RKE receiver's
// passband is wide enough to catch both shifted frequencies.
//
// 32-byte payload below was derived from public Flipper Zero ".sub" captures
// of Tesla charge-port signals (Tesla_Charge_Port.sub variants). The leading
// alternating bytes are the preamble the OOK demodulator needs to lock; the
// middle section is the data envelope; the trailing zeros are the inter-burst
// gap that the radio's fixed-packet-length engine fills with silence.

#define TESLA_CP_FREQ_US    315.0f      // MHz — North America RKE
#define TESLA_CP_FREQ_EU    433.92f     // MHz — EU / JP / AU / row
#define TESLA_CP_BITRATE    2.5f        // kbps → 400 µs/bit
#define TESLA_CP_DEVIATION  10.0f       // kHz
#define TESLA_CP_BANDWIDTH  46.9f       // SX1262 only accepts discrete BWs
#define TESLA_CP_POWER_DBM  10          // conservative for a watch antenna
#define TESLA_CP_PREAMBLE   16          // bits
#define TESLA_CP_BURSTS     5           // copies per transmit
#define TESLA_CP_GAP_MS     12          // inter-burst silence

static const uint8_t TESLA_CP_PAYLOAD[] = {
    // Preamble — alternating bits give the receiver something to AGC + lock to
    0xAA, 0xAA, 0xAA, 0xAA,
    // Start-of-frame: long high followed by short gap (sync the demodulator)
    0xFE, 0x80,
    // Data envelope — the published pre-rolling-code Tesla CP signature
    0x52, 0x69, 0xAA, 0x96,
    0x59, 0x9A, 0x65, 0x96,
    0x9A, 0x6A, 0x96, 0x99,
    0x55, 0x95, 0x9A, 0xA5,
    // End-of-frame
    0xF0, 0x00, 0x00, 0x00,
};

static int16_t s_last_error = 0;

int16_t tesla_cp_last_error() { return s_last_error; }

// Shared core. Only the carrier frequency differs between the US (315
// MHz) and EU (433.92 MHz) variants — everything else (bitrate,
// deviation, payload, burst count) is identical because the Tesla
// charge-port command itself is the same signal in both regions.
static bool tesla_cp_transmit_at(float freq_mhz)
{
    // The SX1262 is shared with LoRa, APRS, and the pager — refuse if any
    // of them are currently holding the radio.
    if (lora_screen_is_powered() || aprs_is_running() || pager_is_running())
        return false;

    int16_t rc = radio.beginFSK(
        freq_mhz,
        TESLA_CP_BITRATE,
        TESLA_CP_DEVIATION,
        TESLA_CP_BANDWIDTH,
        TESLA_CP_POWER_DBM,
        TESLA_CP_PREAMBLE,
        1.6f                  // tcxoVoltage — same as APRS / pager setup
    );
    s_last_error = rc;
    if (rc != RADIOLIB_ERR_NONE) return false;

    radio.setEncoding(RADIOLIB_ENCODING_NRZ);
    radio.fixedPacketLengthMode(sizeof(TESLA_CP_PAYLOAD));

    // Repeat the burst 5× with a short gap between copies — matches the
    // redundancy pattern of real Tesla key fobs and gives the RKE receiver
    // multiple chances to lock + decode.
    for (int i = 0; i < TESLA_CP_BURSTS; i++) {
        rc = radio.transmit((uint8_t *)TESLA_CP_PAYLOAD, sizeof(TESLA_CP_PAYLOAD));
        if (rc != RADIOLIB_ERR_NONE) { s_last_error = rc; break; }
        if (i + 1 < TESLA_CP_BURSTS) delay(TESLA_CP_GAP_MS);
    }

    // Hand the radio back to standby so the next user (LoRa screen / pager /
    // APRS) sees a clean slate.
    radio.standby();
    return s_last_error == RADIOLIB_ERR_NONE;
}

bool tesla_cp_transmit_us() { return tesla_cp_transmit_at(TESLA_CP_FREQ_US); }
bool tesla_cp_transmit_eu() { return tesla_cp_transmit_at(TESLA_CP_FREQ_EU); }
