#pragma once
#include <stdint.h>

enum PagerMode {
    PAGER_POCSAG_512  = 0,
    PAGER_POCSAG_1200 = 1,
    PAGER_POCSAG_2400 = 2,
    PAGER_FLEX_1600   = 3,
};

// Function codes (POCSAG) / message types (FLEX)
enum PagerFunc {
    PFUNC_TONE    = 0,   // POCSAG: tone-only alert
    PFUNC_NUMERIC = 1,   // POCSAG: numeric message
    PFUNC_ALPHA   = 2,   // POCSAG: alphanumeric message
    PFUNC_VOICE   = 3,   // POCSAG: voice alert
    PFUNC_FLEX    = 4,   // FLEX message (any sub-type)
};

struct PagerMsg {
    uint32_t capcode;
    uint8_t  func;           // PagerFunc
    char     text[128];
    char     time_str[9];    // "HH:MM:SS"
    uint32_t received_ms;
    float    freq_mhz;       // frequency the message landed on (scanner)
};

#define PAGER_MSG_MAX 30

bool            pager_start(float freq_mhz, PagerMode mode);
void            pager_stop();
bool            pager_is_running();
float           pager_get_freq();
PagerMode       pager_get_mode();
void            pager_set_mode(PagerMode mode);   // store mode while stopped
int16_t         pager_last_error();               // RadioLib code from last pager_start()
void            pager_bg_tick();
int             pager_get_msg_count();
const PagerMsg* pager_get_msg(int idx);   // 0 = newest
void            pager_clear_msgs();

// ---- Multi-frequency scanner ----
//
// Hops through a curated list of common POCSAG/FLEX channels at the given
// mode, dwelling ~300 ms per channel and extending the dwell automatically
// when the radio reports sync activity. Use this when you don't know which
// frequency local pager traffic is on — it'll surface activity within a
// full cycle (~7 s) of any channel in the list.

bool   pager_start_scanner(PagerMode mode);
bool   pager_is_scanning_all();   // true while in multi-freq scan
int    pager_scan_freq_count();
float  pager_scan_freq_at(int idx);
int    pager_scan_current_idx();  // 0..count-1; index currently dwelt

// Transmit a page on `freq_mhz`. Synchronous: blocks until the airframe
// has gone out (worst case ~1 s for a long 512-bps batch). If the pager
// is currently in RX, it is stopped before transmit and resumed afterwards
// on the RX frequency it was using (not on `freq_mhz`). Returns true on
// success, false if the radio couldn't be tuned, the encoder rejected the
// input, or `freq_mhz` is outside the SX1262's 150–960 MHz tuning range.
//
// POCSAG: `mode` selects bitrate (PAGER_POCSAG_512/_1200/_2400). `func` is
// any of PFUNC_TONE / PFUNC_NUMERIC / PFUNC_ALPHA. `text` is ignored for
// PFUNC_TONE.
bool pager_transmit_pocsag(uint32_t capcode, PagerFunc func,
                           const char *text, PagerMode mode, float freq_mhz);

// FLEX 1600 bps alphanumeric. Best-effort encoder matching the inverse of
// pager.cpp's RX decoder — interoperability with arbitrary FLEX receivers
// is not guaranteed (the FLEX cycle/frame structure is not reconstructed).
// `freq_mhz` semantics same as the POCSAG variant.
bool pager_transmit_flex(uint32_t capcode, const char *text, float freq_mhz);
