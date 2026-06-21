#pragma once
#include <stdint.h>
#include <stdbool.h>

// LoRa APRS — APRS carried over LoRa on 433.775 MHz, the de-facto LoRa-APRS
// standard (SF12, 125 kHz bandwidth, coding rate 4/5, sync word 0x12).
//
// Why LoRa APRS and not classic 144 MHz APRS: classic APRS is 1200-baud AFSK
// on the 2 m VHF band, which the T-Watch's SX1262 cannot reach (its tuning
// range is ~150–960 MHz). LoRa APRS runs in the 70 cm band the SX1262 covers
// and has an active iGate/digipeater network, so it is the APRS mode this
// hardware can actually do.
//
// On air, every LoRa-APRS frame is the 3-byte header 0x3C 0xFF 0x01 followed
// by an ASCII TNC2 frame: "SOURCE>DEST,PATH:information".
//
// RX: received frames are parsed, shown on the APRS screen, and appended to
// /APRS/received.txt on the SD card. TX: a position beacon is built from the
// current GPS fix and the user's callsign.

#define APRS_FREQ_MHZ      433.775f
#define APRS_PKT_MAX       30        // received-packet ring buffer depth
#define APRS_CALLSIGN_MAX  16

struct AprsPacket {
    char     raw[230];        // full TNC2 frame "SRC>DST,PATH:info"
    char     source[12];      // parsed source callsign (before '>')
    char     info[200];       // parsed information field (after the first ':')
    float    rssi;            // dBm
    float    snr;             // dB
    char     time_str[9];     // "HH:MM:SS"
    uint32_t received_ms;
};

void    aprs_init();          // load the saved callsign; call once in setup()

bool    aprs_start();         // claim the shared SX1262 and start APRS RX
void    aprs_stop();
bool    aprs_is_running();
int16_t aprs_last_error();    // RadioLib code from the last aprs_start()

// Queue a position beacon built from the current GPS fix. The frame is sent by
// aprs_bg_tick(). Returns false if APRS is stopped or there is no GPS fix.
bool    aprs_send_position(const char *comment);
bool    aprs_tx_pending();    // a beacon is queued, waiting for the next tick
bool    aprs_tx_busy();       // true during the (blocking) transmit

void    aprs_bg_tick();       // RX drain, SD logging, queued TX — call from loop()

int               aprs_get_packet_count();
const AprsPacket *aprs_get_packet(int idx);   // 0 = newest
void              aprs_clear_packets();

const char *aprs_get_callsign();
void        aprs_set_callsign(const char *cs);   // uppercased + saved to SD
