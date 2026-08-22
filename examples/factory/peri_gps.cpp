
#include "utilities.h"
#include "peripheral.h"
#include <TinyGPS++.h>
#include <sys/time.h>
#include <time.h>
#include <stdlib.h>

/* clang-format off */

TinyGPSPlus gps;
static bool GPS_Recovery();
static bool gps_try_baud(uint32_t baud);
static void gps_power_cycle();
static void gps_try_apply_time_sync();
bool setupGPS();
void displayInfo();

static TaskHandle_t gps_handle = nullptr;
static double gps_lat=0, gps_lng=0, gps_altitude=0, gps_speed=0;
static uint16_t gps_year=0;
static uint8_t gps_month=0, gps_day=0;
static uint8_t gps_hour=0, gps_minute=0, gps_second=0;
static uint32_t gps_vsat=0;
static uint32_t gps_active_baud = 0;

uint8_t buffer[256];

static constexpr uint32_t GPS_BAUD_CANDIDATES[] = {
    9600,
    38400,
    115200,
    57600,
    19200,
    4800,
};

static void gps_power_cycle()
{
    SerialGPS.end();
    digitalWrite(BOARD_GPS_EN, LOW);
    delay(100);
    digitalWrite(BOARD_GPS_EN, HIGH);
    delay(200);
}

static bool gps_try_baud(uint32_t baud)
{
    SerialGPS.begin(baud, SERIAL_8N1, BOARD_GPS_RXD, BOARD_GPS_TXD);
    delay(50);

    while (SerialGPS.available()) {
        SerialGPS.read();
    }

    if (!GPS_Recovery()) {
        return false;
    }

    gps_active_baud = baud;
    Serial.printf("GPS connected at %lu baud\n", static_cast<unsigned long>(baud));
    return true;
}

// The MIA-M10Q's PPS (TIMEPULSE) output and the back-of-device red LED share
// the same net (utilities.h defines both BOARD_GPS_PPS and BOARD_RED_LED as
// GPIO 1). With the GPS powered, the module drives ~1 Hz pulses onto that
// line and overrides whatever digitalWrite() we issued for the LED setting.
// Nothing in the firmware consumes PPS as an input, so we disable TP1 at the
// GPS via UBX-CFG-VALSET (key CFG-TP-TP1_ENA = 0x10050007, RAM layer — we
// resend on every boot because gps_power_cycle() clears RAM config).
static void gps_disable_time_pulse(void)
{
    static const uint8_t cfg_tp1_disable[] = {
        0xB5, 0x62,             // sync
        0x06, 0x8A,             // class=CFG, id=VALSET
        0x09, 0x00,             // length = 9
        0x00,                   // version
        0x01,                   // layers = RAM
        0x00, 0x00,             // reserved
        0x07, 0x00, 0x05, 0x10, // key: CFG-TP-TP1_ENA (LE)
        0x00,                   // value: disabled
        0xB6, 0x83              // CK_A, CK_B
    };
    SerialGPS.write(cfg_tp1_disable, sizeof(cfg_tp1_disable));
    SerialGPS.flush();
}

bool gps_init(void)
{
    bool result = false;
    gps_active_baud = 0;

    gps_power_cycle();
    // L76K GPS USE 9600 BAUDRATE
    // result = setupGPS();
    if (!result) {
        for (size_t i = 0; i < (sizeof(GPS_BAUD_CANDIDATES) / sizeof(GPS_BAUD_CANDIDATES[0])); ++i) {
            result = gps_try_baud(GPS_BAUD_CANDIDATES[i]);
            if (result) {
                break;
            }
        }
    }
    if (!result) {
        Serial.println("GPS Connect failed~!");
    }
    if(result) {
        gps_disable_time_pulse();
        Serial.println("GPS Task Create...!");
        gps_task_create();
    }
    return result;
}

void gps_task(void *param)
{
    // Suspend ourselves immediately. gps_handle is still NULL here because
    // this task runs before xTaskCreate() has returned and written the global.
    // vTaskSuspend(NULL) always means "calling task", which is what we want;
    // vTaskSuspend(gps_handle) would dereference a NULL handle — undefined
    // behaviour if the scheduler ever changes. An explicit NULL makes the
    // intent unambiguous and removes the latent bug. gps_task_resume() will
    // wake us via the now-valid gps_handle once init is complete.
    vTaskSuspend(NULL);

    while(1)
    {
        // NOTE: the former 'while (Serial.available()) SerialGPS.write(Serial.read())'
        // USB-console passthrough was removed. It races with a7682_task which
        // drains the same SerialMon UART, splitting console input
        // non-deterministically between the GPS and the modem — a correctness
        // bug. The modem's own drain loop must remain the sole consumer.

        while (SerialGPS.available()) {
            int c = SerialGPS.read();
            // Serial.write(c);
            if (gps.encode(c)) {
                displayInfo();
                gps_try_apply_time_sync();
            }
        }

        if (millis() > 30000 && gps.charsProcessed() < 10) {
            Serial.println(F("No GPS detected: check wiring."));
            delay(1000);
        }

        // The u-blox MIA-M10Q runs at 1 Hz; waking 1000×/s to drain a UART
        // that has data ~1×/s wastes CPU and bus bandwidth. 20 ms covers the
        // full NMEA burst that arrives each second without adding perceptible
        // latency to the fix display.
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void gps_task_create(void)
{
    xTaskCreate(gps_task, "gps_task", 1024 * 3, NULL, GPS_PRIORITY, &gps_handle);
    // vTaskSuspend(gps_handle);
}

void gps_task_suspend(void)
{
    if (gps_handle != nullptr) {
        vTaskSuspend(gps_handle);
    }
}

void gps_task_resume(void)
{
    if (gps_handle != nullptr) {
        vTaskResume(gps_handle);
    }
}

void gps_get_coord(double *lat, double *lng)
{
    *lat = gps_lat;
    *lng = gps_lng;
}

void gps_get_data(uint16_t *year, uint8_t *month, uint8_t *day)
{
    *year = gps_year;
    *month = gps_month;
    *day = gps_day;
}

void gps_get_time(uint8_t *hour, uint8_t *minute, uint8_t *second)
{
    *hour = gps_hour;
    *minute = gps_minute;
    *second = gps_second;
}

void gps_get_satellites(uint32_t *vsat)
{
    *vsat = gps_vsat;   // Visible Satellites
}

void gps_get_speed(double *speed)
{
    *speed = gps_speed;
}

// GPS gives us UTC date+time once it has a fix. We push it into the system
// clock via settimeofday() so the existing ui_time_get_local() path picks it
// up automatically — no UI changes needed. This is the offline fallback for
// NTP: without WiFi the clock would otherwise be stuck at "--:--".
//
// We re-apply every hour to absorb any ESP32 internal RTC drift, and we
// rate-limit attempts so we're not toggling TZ on every NMEA sentence.
#define GPS_TIME_RESYNC_INTERVAL_MS (60UL * 60UL * 1000UL)
#define GPS_TIME_ATTEMPT_INTERVAL_MS 1000UL

static uint32_t gps_time_last_set_ms = 0;
static uint32_t gps_time_last_attempt_ms = 0;
static bool gps_time_ever_set = false;

static void gps_try_apply_time_sync(void)
{
    uint32_t now_ms = millis();
    if (gps_time_last_attempt_ms != 0 &&
        (now_ms - gps_time_last_attempt_ms) < GPS_TIME_ATTEMPT_INTERVAL_MS) {
        return;
    }
    gps_time_last_attempt_ms = now_ms;

    if (gps_time_ever_set &&
        (now_ms - gps_time_last_set_ms) < GPS_TIME_RESYNC_INTERVAL_MS) {
        return;
    }

    // Require both fields to be valid AND fresh (< 1.5s since last NMEA
    // update). Stale fields can linger from a previous fix after signal loss.
    if (!gps.date.isValid() || !gps.time.isValid()) return;
    if (gps.date.age() > 1500 || gps.time.age() > 1500) return;
    if (gps.date.year() < 2024) return;

    struct tm tm_utc = {};
    tm_utc.tm_year = (int)gps.date.year() - 1900;
    tm_utc.tm_mon  = (int)gps.date.month() - 1;
    tm_utc.tm_mday = (int)gps.date.day();
    tm_utc.tm_hour = (int)gps.time.hour();
    tm_utc.tm_min  = (int)gps.time.minute();
    tm_utc.tm_sec  = (int)gps.time.second();
    tm_utc.tm_isdst = 0;

    // mktime() interprets the struct in the current TZ. GPS gives UTC, so
    // pin TZ to UTC for the conversion, then lock TZ to America/Sao_Paulo
    // (UTC-3, no DST since 2019) so all local time reads render in BRT.
    setenv("TZ", "UTC0", 1);
    tzset();
    time_t t = mktime(&tm_utc);

    setenv("TZ", "<-03>3", 1);
    tzset();

    if (t <= 0) return;

    struct timeval tv = { .tv_sec = t, .tv_usec = 0 };
    if (settimeofday(&tv, NULL) != 0) return;

    gps_time_last_set_ms = now_ms;
    if (!gps_time_ever_set) {
        gps_time_ever_set = true;
        Serial.printf("[gps] system time set from GPS: %04u-%02u-%02u %02u:%02u:%02u UTC\n",
                      gps.date.year(), gps.date.month(), gps.date.day(),
                      gps.time.hour(), gps.time.minute(), gps.time.second());
    }
}

/* clang-format on */
void displayInfo()
{
    Serial.print(F("Location: "));
    if (gps.location.isValid())
    {
        gps_lat = gps.location.lat();
        gps_lng = gps.location.lng();
        Serial.print(gps_lat, 6);
        Serial.print(F(","));
        Serial.print(gps_lng, 6);
    }
    else
    {
        Serial.print(F("INVALID"));
    }

    Serial.print(F("  Date/Time: "));
    if (gps.date.isValid())
    {
        gps_year = gps.date.year();
        gps_month = gps.date.month();
        gps_day = gps.date.day();
        Serial.print(gps_month);
        Serial.print(F("/"));
        Serial.print(gps_day);
        Serial.print(F("/"));
        Serial.print(gps_year);
    }
    else
    {
        Serial.print(F("INVALID"));
    }

    Serial.print(F(" "));
    if (gps.time.isValid())
    {
        gps_hour = gps.time.hour();
        gps_minute = gps.time.minute();
        gps_second = gps.time.second();

        if (gps_hour < 10)
            Serial.print(F("0"));
        Serial.print(gps_hour);
        Serial.print(F(":"));
        if (gps_minute < 10)
            Serial.print(F("0"));
        Serial.print(gps_minute);
        Serial.print(F(":"));
        if (gps_second < 10)
            Serial.print(F("0"));
        Serial.print(gps_second);
        Serial.print(F("."));
    }
    else
    {
        Serial.print(F("INVALID"));
    }

    Serial.print(F("  Satellites: "));
    if(gps.satellites.isValid())
    {
        gps_vsat = gps.satellites.value();
        Serial.print(gps_vsat);
        Serial.print(F(" "));
    }

    Serial.print(F("  Speed: "));
    if(gps.speed.isValid())
    {
        gps_speed = gps.speed.kmph();
        Serial.print(gps_speed);
        Serial.print(F(" "));
    }

    Serial.println();
}
/* clang-format off */

bool setupGPS()
{
    // L76K GPS USE 9600 BAUDRATE
    SerialGPS.begin(9600, SERIAL_8N1, BOARD_GPS_RXD, BOARD_GPS_TXD);
    bool result = false;
    uint32_t startTimeout ;
    for (int i = 0; i < 3; ++i) {
        SerialGPS.write("$PCAS03,0,0,0,0,0,0,0,0,0,0,,,0,0*02\r\n");
        delay(5);
        // Get version information
        startTimeout = millis() + 3000;
        Serial.print("Try to init L76K . Wait stop .");
        while (SerialGPS.available()) {
            Serial.print(".");
            SerialGPS.readString();
            if (millis() > startTimeout) {
                Serial.println("Wait L76K stop NMEA timeout!");
                return false;
            }
        };
        Serial.println();
        SerialGPS.flush();
        delay(200);

        SerialGPS.write("$PCAS06,0*1B\r\n");
        startTimeout = millis() + 500;
        String ver = "";
        while (!SerialGPS.available()) {
            if (millis() > startTimeout) {
                Serial.println("Get L76K timeout!");
                return false;
            }
        }
        SerialGPS.setTimeout(10);
        ver = SerialGPS.readStringUntil('\n');
        if (ver.startsWith("$GPTXT,01,01,02")) {
            Serial.println("L76K GNSS init succeeded, using L76K GNSS Module\n");
            result = true;
            break;
        }
        delay(500);
    }
    // Initialize the L76K Chip, use GPS + GLONASS
    SerialGPS.write("$PCAS04,5*1C\r\n");
    delay(250);
    SerialGPS.write("$PCAS03,1,1,1,1,1,1,1,1,1,1,,,0,0*26\r\n");
    delay(250);
    // Switch to Vehicle Mode, since SoftRF enables Aviation < 2g
    SerialGPS.write("$PCAS11,3*1E\r\n");
    return result;
}


static int getAck(uint8_t *buffer, uint16_t size, uint8_t requestedClass, uint8_t requestedID)
{
    uint16_t    ubxFrameCounter = 0;
    bool        ubxFrame = 0;
    uint32_t    startTime = millis();
    uint16_t    needRead;

    while (millis() - startTime < 800) {
        while (SerialGPS.available()) {
            int c = SerialGPS.read();
            switch (ubxFrameCounter) {
            case 0:
                if (c == 0xB5) {
                    ubxFrameCounter++;
                }
                break;
            case 1:
                if (c == 0x62) {
                    ubxFrameCounter++;
                } else {
                    ubxFrameCounter = 0;
                }
                break;
            case 2:
                if (c == requestedClass) {
                    ubxFrameCounter++;
                } else {
                    ubxFrameCounter = 0;
                }
                break;
            case 3:
                if (c == requestedID) {
                    ubxFrameCounter++;
                } else {
                    ubxFrameCounter = 0;
                }
                break;
            case 4:
                needRead = c;
                ubxFrameCounter++;
                break;
            case 5:
                needRead |=  (c << 8);
                ubxFrameCounter++;
                break;
            case 6:
                if (needRead >= size) {
                    ubxFrameCounter = 0;
                    break;
                }
                if (SerialGPS.readBytes(buffer, needRead) != needRead) {
                    ubxFrameCounter = 0;
                } else {
                    return needRead;
                }
                break;

            default:
                break;
            }
        }
    }
    return 0;
}

static bool GPS_Recovery()
{
    uint8_t cfg_clear1[] = {0xB5, 0x62, 0x06, 0x09, 0x0D, 0x00, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x1C, 0xA2};
    uint8_t cfg_clear2[] = {0xB5, 0x62, 0x06, 0x09, 0x0D, 0x00, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x1B, 0xA1};
    uint8_t cfg_clear3[] = {0xB5, 0x62, 0x06, 0x09, 0x0D, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0x00, 0x00, 0x03, 0x1D, 0xB3};
    SerialGPS.write(cfg_clear1, sizeof(cfg_clear1));

    if (getAck(buffer, 256, 0x05, 0x01)) {
        Serial.println("Get ack successes!");
    }
    SerialGPS.write(cfg_clear2, sizeof(cfg_clear2));
    if (getAck(buffer, 256, 0x05, 0x01)) {
        Serial.println("Get ack successes!");
    }
    SerialGPS.write(cfg_clear3, sizeof(cfg_clear3));
    if (getAck(buffer, 256, 0x05, 0x01)) {
        Serial.println("Get ack successes!");
    }

    // UBX-CFG-RATE, Size 8, 'Navigation/measurement rate settings'
    uint8_t cfg_rate[] = {0xB5, 0x62, 0x06, 0x08, 0x00, 0x00, 0x0E, 0x30};
    SerialGPS.write(cfg_rate, sizeof(cfg_rate));
    if (getAck(buffer, 256, 0x06, 0x08)) {
        Serial.println("Get ack successes!");
    } else {
        return false;
    }
    return true;
}
