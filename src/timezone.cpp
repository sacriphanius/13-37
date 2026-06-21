#include "timezone.h"
#include <Arduino.h>
#include <LilyGoLib.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <SD.h>
#include <time.h>
#include <limits.h>
#include "usb_sd.h"

// Implemented in main.cpp (clock screen). The RTC holds UTC; clock_utc_offset
// shifts it to local for display.
void clock_screen_set_utc_offset(int offset_hours);
bool clock_screen_manual_time_active();
void clock_screen_refresh();

#define TZ_PATH "/Settings/timezone.txt"

// ---- persistence -----------------------------------------------------------

static int s_last_written = INT_MIN;   // avoid redundant SD writes

void timezone_note_detected(int offset_hours)
{
    if (offset_hours == s_last_written) return;            // unchanged
    if (offset_hours < -12 || offset_hours > 14) return;   // sanity
    if (!instance.isCardReady() || usb_sd_is_running()) return;

    if (!SD.exists("/Settings")) SD.mkdir("/Settings");
    File f = SD.open(TZ_PATH, FILE_WRITE);   // FILE_WRITE = truncate
    if (!f) return;
    f.printf("offset=%d\n", offset_hours);
    f.close();
    s_last_written = offset_hours;
}

void timezone_load_on_boot()
{
    // Manual Time wins — in that mode the RTC already holds local time directly.
    if (clock_screen_manual_time_active()) return;
    if (!instance.isCardReady() || usb_sd_is_running()) return;
    if (!SD.exists(TZ_PATH)) return;

    File f = SD.open(TZ_PATH, FILE_READ);
    if (!f) return;
    char buf[32] = {0};
    int n = f.readBytesUntil('\n', (uint8_t *)buf, sizeof(buf) - 1);
    f.close();
    if (n <= 0) return;
    buf[n] = '\0';

    const char *p = strstr(buf, "offset=");
    if (!p) return;
    int off = atoi(p + 7);
    if (off < -12 || off > 14) return;     // sanity

    s_last_written = off;                  // already on disk; don't rewrite it
    clock_screen_set_utc_offset(off);
    clock_screen_refresh();
}

// ---- WiFi NTP + IP-geolocation background sync ------------------------------
//
// NTP and HTTP both block for seconds, so the network work runs on a dedicated
// task; the results are applied back on the main loop (timezone_bg_tick) so the
// RTC / clock / SD are only touched from one context.

static volatile bool s_sync_pending = false;   // WiFi GOT_IP -> request a sync
static volatile bool s_result_ready = false;   // worker -> main: results ready
static volatile bool s_got_time     = false;
static volatile bool s_got_offset   = false;
static volatile int  s_offset_h     = 0;
static struct tm     s_utc_tm       = {};
static TaskHandle_t  s_task         = nullptr;

// Query ip-api.com for the current UTC offset (seconds, DST-aware). Free tier
// is plain HTTP. Returns true and fills *out_h on success.
static bool http_get_offset(int *out_h)
{
    HTTPClient http;
    http.setConnectTimeout(5000);
    http.setTimeout(5000);
    if (!http.begin("http://ip-api.com/json/?fields=status,offset")) return false;

    bool ok = false;
    if (http.GET() == 200) {
        String body = http.getString();
        if (body.indexOf("\"status\":\"success\"") >= 0) {
            int i = body.indexOf("\"offset\":");
            if (i >= 0) {
                long secs = atol(body.c_str() + i + 9);
                *out_h = (int)(secs / 3600);
                ok = true;
            }
        }
    }
    http.end();
    return ok;
}

static void tz_worker(void *)
{
    for (;;) {
        if (!s_sync_pending) { vTaskDelay(pdMS_TO_TICKS(250)); continue; }
        s_sync_pending = false;
        if (WiFi.status() != WL_CONNECTED) continue;

        bool      got_time = false, got_off = false;
        int       off_h = 0;
        struct tm tmutc = {};

        // NTP: configTime(0,0,...) keeps system time in UTC.
        configTime(0, 0, "pool.ntp.org", "time.nist.gov");
        if (getLocalTime(&tmutc, 8000)) got_time = true;

        // IP geolocation -> current UTC offset.
        if (http_get_offset(&off_h)) got_off = true;

        if (got_time) s_utc_tm = tmutc;
        s_got_time     = got_time;
        s_offset_h     = off_h;
        s_got_offset   = got_off;
        s_result_ready = true;
    }
}

static void on_wifi_got_ip(arduino_event_id_t, arduino_event_info_t)
{
    s_sync_pending = true;   // worker picks it up
}

void timezone_init()
{
    WiFi.onEvent(on_wifi_got_ip, ARDUINO_EVENT_WIFI_STA_GOT_IP);
    if (!s_task)
        xTaskCreate(tz_worker, "tz_sync", 8192, nullptr, 3, &s_task);
}

void timezone_bg_tick()
{
    if (!s_result_ready) return;
    s_result_ready = false;

    // Manual Time wins — drop the results untouched.
    if (clock_screen_manual_time_active()) {
        s_got_time = s_got_offset = false;
        return;
    }

    if (s_got_time) {
        instance.rtc.setDateTime(s_utc_tm.tm_year + 1900, s_utc_tm.tm_mon + 1,
                                 s_utc_tm.tm_mday, s_utc_tm.tm_hour,
                                 s_utc_tm.tm_min, s_utc_tm.tm_sec);
        instance.rtc.hwClockRead();
        s_got_time = false;
    }
    if (s_got_offset) {
        clock_screen_set_utc_offset(s_offset_h);
        timezone_note_detected(s_offset_h);
        s_got_offset = false;
    }
    clock_screen_refresh();
}
