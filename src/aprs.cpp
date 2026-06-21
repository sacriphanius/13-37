#include "aprs.h"

void clock_screen_get_local_time(struct tm *out);
#include "lora_screen.h"
#include "pager.h"
#include "tpms.h"
#include "gps_screen.h"
#include "usb_sd.h"
#include <LilyGoLib.h>
#include <SD.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

// TNC2 destination ("tocall") and digipeater path used by transmitted beacons.
// APZxxx is the registered prefix for experimental / homebrew APRS software;
// APZTWU = "T-Watch Ultra". WIDE1-1 is the standard generic single-hop path.
#define APRS_TOCALL  "APZTWU"
#define APRS_PATH    "WIDE1-1"

#define APRS_CALLSIGN_FILE "/APRS/callsign.txt"

// ---- state -----------------------------------------------------------------

static bool      s_running   = false;
static int16_t   s_last_error = 0;
static char      s_callsign[APRS_CALLSIGN_MAX] = "N0CALL";

// Received-packet ring buffer. s_head is the index of the newest packet.
static AprsPacket s_pkts[APRS_PKT_MAX];
static int        s_head  = -1;
static int        s_count = 0;

// TX is queued by aprs_send_position() and performed by aprs_bg_tick() so the
// (blocking, ~2 s at SF12) transmit never runs inside an LVGL event callback.
static char           s_tx_frame[230];
static volatile bool  s_tx_pending = false;
static volatile bool  s_tx_busy    = false;
static int16_t        s_last_tx_error = 0;

// Set from the SX1262 DIO1 interrupt when a packet finishes arriving.
static volatile bool s_pkt_flag = false;
static void IRAM_ATTR on_pkt_isr() { s_pkt_flag = true; }

// ---- callsign persistence --------------------------------------------------

void aprs_init()
{
    if (!instance.isCardReady()) return;
    if (!SD.exists(APRS_CALLSIGN_FILE)) return;
    File f = SD.open(APRS_CALLSIGN_FILE, FILE_READ);
    if (!f) return;
    String cs = f.readStringUntil('\n');
    f.close();
    cs.trim();
    if (cs.length() > 0 && cs.length() < APRS_CALLSIGN_MAX) {
        strncpy(s_callsign, cs.c_str(), APRS_CALLSIGN_MAX - 1);
        s_callsign[APRS_CALLSIGN_MAX - 1] = '\0';
    }
}

const char *aprs_get_callsign() { return s_callsign; }

void aprs_set_callsign(const char *cs)
{
    if (!cs) return;
    // Copy, upper-casing — APRS callsigns are upper-case alphanumerics + "-SSID".
    int j = 0;
    for (int i = 0; cs[i] && j < APRS_CALLSIGN_MAX - 1; i++) {
        char c = cs[i];
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-')
            s_callsign[j++] = c;
    }
    s_callsign[j] = '\0';
    if (j == 0) { strcpy(s_callsign, "N0CALL"); }

    // Persist to the SD card (skipped while the card is mounted over USB).
    if (usb_sd_is_running() || !instance.isCardReady()) return;
    if (!SD.exists("/APRS")) SD.mkdir("/APRS");
    File f = SD.open(APRS_CALLSIGN_FILE, FILE_WRITE);   // truncate + rewrite
    if (!f) return;
    f.println(s_callsign);
    f.close();
}

// ---- radio lifecycle -------------------------------------------------------

bool aprs_start()
{
    if (s_running) return true;

    // The SX1262 is shared and can only be in one mode. Refuse if Meshtastic
    // holds it; stop the FSK scanners so they can't steal IRQs.
    if (lora_screen_is_powered()) { s_last_error = 0; return false; }
    pager_stop();
    tpms_stop();

    instance.powerControl(POWER_RADIO, true);

    // LoRa APRS link settings: 433.775 MHz, SF12, 125 kHz BW, CR 4/5,
    // sync word 0x12, 20 dBm, 8-symbol preamble, 1.6 V TCXO.
    int16_t rc = radio.begin(APRS_FREQ_MHZ, 125.0, 12, 5, 0x12, 20, 8, 1.6f);
    s_last_error = rc;
    if (rc != RADIOLIB_ERR_NONE) return false;

    radio.setPacketReceivedAction(on_pkt_isr);
    radio.startReceive();
    s_pkt_flag = false;
    s_running  = true;
    return true;
}

void aprs_stop()
{
    if (!s_running) return;
    s_running = false;
    radio.clearPacketReceivedAction();
    radio.standby();
}

bool    aprs_is_running()  { return s_running;  }
int16_t aprs_last_error()  { return s_last_error; }
bool    aprs_tx_pending()  { return s_tx_pending; }
bool    aprs_tx_busy()     { return s_tx_busy;    }

// ---- received-packet store -------------------------------------------------

int aprs_get_packet_count() { return s_count; }

const AprsPacket *aprs_get_packet(int idx)
{
    if (idx < 0 || idx >= s_count) return nullptr;
    int i = (s_head - idx) % APRS_PKT_MAX;
    if (i < 0) i += APRS_PKT_MAX;
    return &s_pkts[i];
}

void aprs_clear_packets()
{
    s_head  = -1;
    s_count = 0;
}

static void store_packet(const AprsPacket *p)
{
    s_head = (s_head + 1) % APRS_PKT_MAX;
    s_pkts[s_head] = *p;
    if (s_count < APRS_PKT_MAX) s_count++;
}

// ---- RX --------------------------------------------------------------------

// Splits a TNC2 frame "SRC>DST,PATH:info" into its source call and info field.
static void parse_tnc2(AprsPacket *p)
{
    p->source[0] = '\0';
    p->info[0]   = '\0';
    const char *gt = strchr(p->raw, '>');
    if (gt) {
        size_t n = (size_t)(gt - p->raw);
        if (n >= sizeof(p->source)) n = sizeof(p->source) - 1;
        memcpy(p->source, p->raw, n);
        p->source[n] = '\0';
    }
    const char *colon = strchr(p->raw, ':');
    if (colon) {
        strncpy(p->info, colon + 1, sizeof(p->info) - 1);
        p->info[sizeof(p->info) - 1] = '\0';
    }
}

static void log_packet(const AprsPacket *p)
{
    if (usb_sd_is_running()) return;          // host owns the SD card
    if (!instance.isCardReady()) return;
    if (!SD.exists("/APRS")) SD.mkdir("/APRS");

    File f = SD.open("/APRS/received.txt", FILE_APPEND);
    if (!f) return;

    struct tm t;
    clock_screen_get_local_time(&t);
    f.printf("%04d-%02d-%02d %02d:%02d:%02d\tRSSI %.0f\tSNR %.1f\t%s\n",
        t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
        t.tm_hour, t.tm_min, t.tm_sec,
        p->rssi, p->snr, p->raw);
    f.close();
}

// Reads the just-received frame off the radio into *p. Returns false on a read
// error or an empty / oversized packet.
static bool read_packet(AprsPacket *p)
{
    uint8_t buf[256];
    size_t len = radio.getPacketLength();
    if (len == 0 || len > sizeof(buf)) {
        radio.readData(buf, sizeof(buf));     // flush to unblock the radio
        return false;
    }
    if (radio.readData(buf, len) != RADIOLIB_ERR_NONE) return false;

    // Strip the 3-byte LoRa-APRS header (0x3C 0xFF 0x01) if present.
    size_t off = 0;
    if (len >= 3 && buf[0] == 0x3C && buf[1] == 0xFF && buf[2] == 0x01)
        off = 3;
    size_t text_len = len - off;
    if (text_len == 0) return false;
    if (text_len >= sizeof(p->raw)) text_len = sizeof(p->raw) - 1;

    // Copy as text: drop CR/LF outright, replace any other non-printable byte
    // with '.' so the log and UI stay clean.
    size_t w = 0;
    for (size_t i = 0; i < text_len; i++) {
        uint8_t c = buf[off + i];
        if (c == '\r' || c == '\n') continue;
        p->raw[w++] = (c >= 0x20 && c <= 0x7E) ? (char)c : '.';
    }
    p->raw[w] = '\0';
    if (w == 0) return false;

    p->rssi = radio.getRSSI();
    p->snr  = radio.getSNR();
    parse_tnc2(p);

    struct tm t;
    clock_screen_get_local_time(&t);
    snprintf(p->time_str, sizeof(p->time_str), "%02d:%02d:%02d",
             t.tm_hour, t.tm_min, t.tm_sec);
    p->received_ms = millis();
    return true;
}

// ---- TX --------------------------------------------------------------------

// Formats a latitude as APRS "DDMM.mmN". Integer hundredths-of-minute maths
// keeps a 59.999' rounding from ever producing an invalid "60.00".
static void format_aprs_lat(char *buf, size_t n, double lat)
{
    char hemi = (lat < 0) ? 'S' : 'N';
    if (lat < 0) lat = -lat;
    if (lat > 89.99999) lat = 89.99999;
    int deg  = (int)lat;
    int hmin = (int)lround((lat - deg) * 6000.0);
    if (hmin >= 6000) { hmin -= 6000; deg += 1; }
    snprintf(buf, n, "%02d%02d.%02d%c", deg, hmin / 100, hmin % 100, hemi);
}

static void format_aprs_lon(char *buf, size_t n, double lon)
{
    char hemi = (lon < 0) ? 'W' : 'E';
    if (lon < 0) lon = -lon;
    if (lon > 179.99999) lon = 179.99999;
    int deg  = (int)lon;
    int hmin = (int)lround((lon - deg) * 6000.0);
    if (hmin >= 6000) { hmin -= 6000; deg += 1; }
    snprintf(buf, n, "%03d%02d.%02d%c", deg, hmin / 100, hmin % 100, hemi);
}

bool aprs_send_position(const char *comment)
{
    if (!s_running) return false;
    if (!gps_screen_has_lock() || !instance.gps.location.isValid()) return false;

    char latbuf[12], lonbuf[12];
    format_aprs_lat(latbuf, sizeof(latbuf), instance.gps.location.lat());
    format_aprs_lon(lonbuf, sizeof(lonbuf), instance.gps.location.lng());

    // Uncompressed position report, no timestamp:
    //   !<lat><symtable>/<lon><symcode>[<comment>
    // Symbol "/[" is a person (jogger) — appropriate for a wearable.
    snprintf(s_tx_frame, sizeof(s_tx_frame),
             "%s>%s,%s:!%s/%s[%s",
             s_callsign, APRS_TOCALL, APRS_PATH,
             latbuf, lonbuf, comment ? comment : "");
    s_tx_pending = true;
    return true;
}

// Prepends the LoRa-APRS header to s_tx_frame and transmits it. Blocking.
static void do_transmit()
{
    s_tx_busy = true;

    uint8_t out[256];
    out[0] = 0x3C; out[1] = 0xFF; out[2] = 0x01;
    size_t flen = strlen(s_tx_frame);
    if (flen > sizeof(out) - 3) flen = sizeof(out) - 3;
    memcpy(out + 3, s_tx_frame, flen);

    s_last_tx_error = radio.transmit(out, flen + 3);
    s_pkt_flag = false;            // discard the TX-done IRQ on DIO1
    radio.startReceive();

    s_tx_busy = false;
}

// ---- background tick -------------------------------------------------------

void aprs_bg_tick()
{
    if (!s_running) return;

    // Drain a received packet first, before any radio mode change.
    if (s_pkt_flag) {
        s_pkt_flag = false;
        AprsPacket pkt;
        if (read_packet(&pkt)) {
            store_packet(&pkt);
            log_packet(&pkt);
        }
        radio.startReceive();
    }

    // Send a queued beacon (blocking — leaves the radio back in RX).
    if (s_tx_pending) {
        s_tx_pending = false;
        do_transmit();
    }
}
