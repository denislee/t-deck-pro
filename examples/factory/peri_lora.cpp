

#include <RadioLib.h>
#include "utilities.h"
#include "factory.h"
#include "peripheral.h"
#include "ui_deckpro_port.h"

static SX1262 radio = new Module(BOARD_LORA_CS, BOARD_LORA_INT, BOARD_LORA_RST, BOARD_LORA_BUSY);
static int lora_mode = LORA_MODE_SEND;
static String lora_recv_data;
static bool lora_recv_success = false;
static int lora_recv_rssi = 0;

// True only once lora_init() completed. Until then the radio may be
// unpowered (BOARD_LORA_EN low) and must not be touched over SPI.
static bool s_lora_ready = false;

// transmit 
static int transmissionState = RADIOLIB_ERR_NONE;
static volatile bool transmittedFlag = false;

static void set_transmit_flag(void){
    transmittedFlag = true;
}

// receive
static int receivedState = RADIOLIB_ERR_NONE;
static volatile bool receivedFlag = false;

static void set_receive_flag(void){
    receivedFlag = true;
}

// Run one radio config call; on failure log it and fail the whole init
// (the caller's SpiGuard releases the bus).
#define LORA_TRY(expr, what)                                              \
    do {                                                                  \
        int _st = (expr);                                                 \
        if (_st != RADIOLIB_ERR_NONE) {                                   \
            Serial.printf("[SX1262] %s failed, code %d\n", (what), _st);  \
            return false;                                                 \
        }                                                                 \
    } while (0)

// Non-fatal variant: log the same message and carry on. The original code
// ignored every status code except "invalid value" for these calls, so a
// missing TCXO / RF-switch pin must not cost the user a working radio.
#define LORA_WARN(expr, what)                                             \
    do {                                                                  \
        int _st = (expr);                                                 \
        if (_st != RADIOLIB_ERR_NONE) {                                   \
            Serial.printf("[SX1262] %s failed, code %d\n", (what), _st);  \
        }                                                                 \
    } while (0)

static bool lora_apply_user_params(void)
{
    LORA_TRY(radio.setFrequency(ui_lora_get_freq()), "frequency");
    LORA_TRY(radio.setBandwidth(ui_lora_get_bandwidth()), "bandwidth");
    LORA_TRY(radio.setOutputPower(ui_lora_get_power()), "output power");
    return true;
}

bool lora_init(void)
{
    SpiGuard spi;

    // Nothing may talk to the radio until this init has fully succeeded.
    s_lora_ready = false;

    Serial.print(F("[SX1262] Initializing ... "));
    int state = radio.begin(ui_lora_get_freq());
    if (state == RADIOLIB_ERR_NONE) {
        Serial.println(F("success!"));
    } else {
        Serial.print(F("failed, code "));
        Serial.println(state);
        return false;
    }

    radio.setPacketSentAction(set_transmit_flag);

    if (!lora_apply_user_params()) return false;

    LORA_TRY(radio.setSpreadingFactor(10), "spreading factor");
    LORA_TRY(radio.setCodingRate(6), "coding rate");
    LORA_WARN(radio.setSyncWord(0xAB), "sync word");
    LORA_WARN(radio.setCurrentLimit(140), "current limit");
    LORA_WARN(radio.setPreambleLength(15), "preamble length");
    LORA_WARN(radio.setCRC(false), "CRC");
    LORA_WARN(radio.setTCXO(2.4), "TCXO voltage");
    LORA_WARN(radio.setDio2AsRfSwitch(), "DIO2 as RF switch");

    Serial.println(F("All settings succesfully changed!"));

    Serial.println(F("[SX1262] Sending first packet ... "));
    transmissionState = radio.startTransmit("Hello World!");

    s_lora_ready = true;
    return true;
}

void lora_set_mode(int mode) 
{
    if (!s_lora_ready) return;

    SpiGuard spi;

    if(mode == LORA_MODE_SEND){
        radio.setPacketSentAction(set_transmit_flag);
        Serial.println(F("[LORA] Sending first packet ... "));
        transmissionState = radio.startTransmit("Hello World!");
    } else if(mode == LORA_MODE_RECV){
        radio.setPacketReceivedAction(set_receive_flag);
        Serial.println(F("[LORA] Starting to listen ... "));
        receivedState = radio.startReceive();
        if (receivedState == RADIOLIB_ERR_NONE) {
            Serial.println(F("success!"));
        } else {
            Serial.print(F("failed, code "));
            Serial.println(receivedState);
        }
    }
    lora_mode = mode;
}

int lora_get_mode(void)
{
    return lora_mode;
}

void lora_receive_loop(void)
{
    if (!s_lora_ready) return;

    if(receivedFlag){
        receivedFlag = false;

        lora_recv_success = true;

        SpiGuard spi;

        receivedState = radio.readData(lora_recv_data);
        if(receivedState == RADIOLIB_ERR_NONE){
            Serial.println(F("[SX1262] Received packet!"));

            Serial.print(F("[SX1262] Data:\t\t"));
            Serial.println(lora_recv_data.c_str());

            Serial.print(F("[SX1262] RSSI:\t\t"));
            Serial.print(radio.getRSSI());
            lora_recv_rssi = (int) radio.getRSSI();
            Serial.println(F(" dBm"));
        }else{
            Serial.print(F("failed, code "));
            Serial.println(receivedState);
        }
    }
}

void lora_transmit(const char *str)
{
    if (!s_lora_ready) return;

    if(transmittedFlag){
        transmittedFlag = false;

        SpiGuard spi;

        if(transmissionState == RADIOLIB_ERR_NONE){
            Serial.println(F("transmission finished!"));
        } else {
            Serial.print(F("failed, code "));
            Serial.println(transmissionState);
        }

        radio.finishTransmit();
        Serial.print(F("[Lora] Sending another packet ... "));
        transmissionState = radio.startTransmit(str);
    }
}

bool lora_get_recv(const char **str, int *rssi)
{
    *str = lora_recv_data.c_str();
    *rssi = lora_recv_rssi;
    return lora_recv_success;
}

void lora_set_recv_flag(void)
{
    lora_recv_success = false;
}

void lora_sleep(void)
{
    if (!s_lora_ready) return;

    SpiGuard spi;
    radio.sleep();
}

bool lora_param_set(void)
{
    if (!s_lora_ready) return false;

    SpiGuard spi;
    return lora_apply_user_params();
}
