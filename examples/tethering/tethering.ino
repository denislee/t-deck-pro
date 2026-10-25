/*
 * Tethering: A7682E (4G) -> ESP32-S3 softAP.
 *
 * Brings up the modem, establishes a PPP session, opens a WiFi access point,
 * and enables NAPT so AP clients route through the cellular link.
 *
 * REQUIRES Arduino-ESP32 3.x, provided by [env:T-Deck-Pro-modem] (see
 * platformio.ini). The PPP class and esp_netif_napt_enable() are not available
 * on the 2.0.x core used by the default factory env. Build/flash with
 * `make build-modem` / `make upload-modem`. See README.md.
 *
 * Battery MUST be connected -- the modem is supplied from the battery rail.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <PPP.h>
#include <esp_netif.h>
#include <lwip/lwip_napt.h>
#include "dhcpserver/dhcpserver.h"   // dhcps_offer_t / OFFER_DNS

#include "utilities.h"

// ---------------------------------------------------------------------------
// User config
// ---------------------------------------------------------------------------
#ifndef TETHER_APN
#define TETHER_APN       "internet"   // carrier APN -- adjust as needed
#endif
#ifndef TETHER_SIM_PIN
#define TETHER_SIM_PIN   NULL         // set to "1234" if your SIM is locked
#endif

#define HOTSPOT_SSID    "T-Deck-Pro"
#define HOTSPOT_PASS    "deckpro1234" // >= 8 chars or AP fails
#define HOTSPOT_CHANNEL 6

// ---------------------------------------------------------------------------
// Modem power-up (PWRKEY pulse, see A7682E hardware design guide ~1.5s)
// ---------------------------------------------------------------------------
static void modem_power_on()
{
    pinMode(BOARD_A7682E_POWER_EN, OUTPUT);
    digitalWrite(BOARD_A7682E_POWER_EN, HIGH);
    delay(100);

    pinMode(BOARD_A7682E_PWRKEY, OUTPUT);
    digitalWrite(BOARD_A7682E_PWRKEY, LOW);
    delay(10);
    digitalWrite(BOARD_A7682E_PWRKEY, HIGH);
    delay(1500);                       // hold > 1.0 s to start
    digitalWrite(BOARD_A7682E_PWRKEY, LOW);
}

// ---------------------------------------------------------------------------
// Hand carrier DNS to AP clients via DHCP option 6
// ---------------------------------------------------------------------------
static void propagate_dns_to_ap(esp_netif_t *ap)
{
    esp_netif_dns_info_t dns{};
    if (esp_netif_get_dns_info(PPP.netif(), ESP_NETIF_DNS_MAIN, &dns) != ESP_OK) {
        return;
    }
    esp_netif_dhcps_stop(ap);
    esp_netif_set_dns_info(ap, ESP_NETIF_DNS_MAIN, &dns);

    dhcps_offer_t opt = OFFER_DNS;
    esp_netif_dhcps_option(ap, ESP_NETIF_OP_SET,
                           ESP_NETIF_DOMAIN_NAME_SERVER, &opt, sizeof(opt));
    esp_netif_dhcps_start(ap);
}

// ---------------------------------------------------------------------------
// Event tap -- useful while debugging PPP bring-up
// ---------------------------------------------------------------------------
static void on_event(arduino_event_id_t event, arduino_event_info_t info)
{
    switch (event) {
        case ARDUINO_EVENT_PPP_START:        Serial.println("[PPP] start"); break;
        case ARDUINO_EVENT_PPP_CONNECTED:    Serial.println("[PPP] connected"); break;
        case ARDUINO_EVENT_PPP_GOT_IP:
            Serial.printf("[PPP] IP=%s GW=%s\n",
                          PPP.localIP().toString().c_str(),
                          PPP.gatewayIP().toString().c_str());
            break;
        case ARDUINO_EVENT_PPP_LOST_IP:      Serial.println("[PPP] lost IP"); break;
        case ARDUINO_EVENT_PPP_DISCONNECTED: Serial.println("[PPP] disconnected"); break;
        case ARDUINO_EVENT_WIFI_AP_START:    Serial.println("[AP] up"); break;
        case ARDUINO_EVENT_WIFI_AP_STACONNECTED:
            Serial.println("[AP] client joined"); break;
        case ARDUINO_EVENT_WIFI_AP_STADISCONNECTED:
            Serial.println("[AP] client left"); break;
        default: break;
    }
}

void setup()
{
    Serial.begin(115200);
    delay(200);
    Serial.println("\n=== T-Deck-Pro tethering ===");

    Network.onEvent(on_event);

    Serial.println("[modem] powering on...");
    modem_power_on();
    delay(3000); // give the modem time to settle before AT traffic

    // PPP transport setup. The Arduino-ESP32 PPP class only exposes the APN
    // string; carriers requiring PAP/CHAP credentials are not supported here
    // (most consumer SIMs don't).
    PPP.setApn(TETHER_APN);
    PPP.setPin(TETHER_SIM_PIN);
    PPP.setResetPin(BOARD_A7682E_RST, LOW, 200);
    // PPP.setPins(tx, rx, rts, cts, flow_ctrl)
    PPP.setPins(BOARD_A7682E_RXD, BOARD_A7682E_TXD);

    Serial.println("[PPP] dialing carrier...");
    // A7682E is SIM7672/A7670 family -> SIM7600-compatible PPP profile
    if (!PPP.begin(PPP_MODEM_SIM7600)) {
        Serial.println("[PPP] begin() failed -- check APN, SIM, signal");
        return;
    }

    uint32_t t0 = millis();
    while (!PPP.connected() && millis() - t0 < 60000) {
        delay(500);
        Serial.print('.');
    }
    Serial.println();
    if (!PPP.connected()) {
        Serial.println("[PPP] no IP after 60s -- giving up");
        return;
    }

    // softAP
    WiFi.mode(WIFI_AP);
    if (!WiFi.softAP(HOTSPOT_SSID, HOTSPOT_PASS, HOTSPOT_CHANNEL)) {
        Serial.println("[AP] softAP() failed");
        return;
    }
    Serial.printf("[AP] SSID='%s' IP=%s\n",
                  HOTSPOT_SSID, WiFi.softAPIP().toString().c_str());

    // route AP -> PPP via NAPT
    esp_netif_t *ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (!ap_netif) {
        Serial.println("[NAPT] no AP netif handle");
        return;
    }
    if (esp_netif_napt_enable(ap_netif) == ESP_OK) {
        Serial.println("[NAPT] enabled");
    } else {
        Serial.println("[NAPT] enable failed -- lwIP not built with NAPT?");
    }

    propagate_dns_to_ap(ap_netif);
    Serial.println("[ready] connect a client to the SSID and try the internet");
}

void loop()
{
    static uint32_t last = 0;
    if (millis() - last >= 10000) {
        last = millis();
        Serial.printf("[stat] PPP=%s clients=%u\n",
                      PPP.connected() ? "up" : "down",
                      WiFi.softAPgetStationNum());
    }
    delay(100);
}
