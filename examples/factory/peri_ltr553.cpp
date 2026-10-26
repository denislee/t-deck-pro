#include <Wire.h>
#include <SPI.h>
#include <Arduino.h>
#include "SensorLTR553.hpp"
#include "utilities.h"
#include "peripheral.h"

static SensorLTR553 als;

bool LTR553_init(void)
{
    I2C0Guard g;

    if (!als.begin(Wire, LTR553_SLAVE_ADDRESS, BOARD_I2C_SDA, BOARD_I2C_SCL)) {
        Serial.println("Failed to find LTR553 - check your wiring!");
        return false;
    }

    Serial.println("Init LTR553 Sensor success!");

    // Nothing reads the sensor any more (the Test screen only needs the
    // presence probe above). Leave both channels in standby: proximity alone
    // pulsed the IR LED at 50 mA every 200 ms.
    als.disableLightSensor();
    als.disableProximity();
    return true;
}
