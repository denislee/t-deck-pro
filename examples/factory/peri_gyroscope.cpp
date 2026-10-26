
// This translation unit contributes 103 KB (bhy2_firmware_image blob + driver
// vtable) to the linked image.  Exclude it entirely when the BHI260AP is not
// fitted; peripheral.h provides inline no-op stubs for the public entry points.
#ifdef BOARD_HAS_BHI260AP

#include <Wire.h>
#include <SPI.h>
#include <Arduino.h>
#include "SensorBHI260AP.hpp"
#include "utilities.h"
#include "peripheral.h"

static SensorBHI260AP bhy;
static struct bhy2_data_xyz accel_data;
static struct bhy2_data_xyz gyro_data;

float accel_factor;
float gyro_factor;

void accel_process_callback(uint8_t sensor_id, uint8_t *data_ptr, uint32_t len, uint64_t *timestamp)
{
    accel_factor = get_sensor_default_scaling(sensor_id);
    bhy2_parse_xyz(data_ptr, &accel_data);
}

void gyro_process_callback(uint8_t sensor_id, uint8_t *data_ptr, uint32_t len, uint64_t *timestamp)
{
    gyro_factor = get_sensor_default_scaling(sensor_id);
    bhy2_parse_xyz(data_ptr, &gyro_data);
}

/**
 * int val_type 
 *      1 --- acceleration
 *      2 --- gyroscope
*/
void BHI260AP_get_val(int val_type, float *x, float *y, float *z)
{
    // bhy.update() pumps callbacks via I2C reads. Same bus as the keypad task.
    i2c0_lock();
    bhy.update();
    i2c0_unlock();

    switch (val_type) {
        case 1:
            *x = accel_data.x * accel_factor;
            *y = accel_data.y * accel_factor;
            *z = accel_data.z * accel_factor;
         break;
        case 2 :
            *x = gyro_data.x * gyro_factor;
            *y = gyro_data.y * gyro_factor;
            *z = gyro_data.z * gyro_factor;
         break;
        default:
            *x = 0;
            *y = 0;
            *z = 0;
            break;
    }
}

// Each BHY2 transfer is a complete I2C transaction, so while this task holds
// i2c0 (BHI260AP_init's I2C0Guard) it can hand the bus to the higher-priority
// keypad task between transfers instead of stalling it for the whole upload.
extern "C" void sensorlib_i2c_transfer_done(void)
{
    if (i2c0_held_by_me()) {
        i2c0_unlock();
        i2c0_lock();
    }
}

bool BHI260AP_init(void)
{
    I2C0Guard g;

    // Set the reset pin and interrupt pin, if any
    bhy.setPins(BOARD_GYROSCOPDE_RST, BOARD_GYROSCOPDE_INT);

    if (!bhy.init(Wire, BOARD_I2C_SDA, BOARD_I2C_SCL, BHI260AP_SLAVE_ADDRESS_L)) {
        Serial.print("Failed to init BHI260AP - ");
        Serial.println(bhy.getError());
        return false;
    }

    // Output all available sensors to Serial
    // bhy.printSensors(Serial);

    float sample_rate = 100.0;      /* Read out hintr_ctrl measured at 100Hz */
    uint32_t report_latency_ms = 0; /* Report immediately */

    // Enable acceleration
    bhy.configure(SENSOR_ID_ACC_PASS, sample_rate, report_latency_ms);
    // Enable gyroscope
    bhy.configure(SENSOR_ID_GYRO_PASS, sample_rate, report_latency_ms);

    // Set the acceleration sensor result callback function
    bhy.onResultEvent(SENSOR_ID_ACC_PASS, accel_process_callback);

    // Set the gyroscope sensor result callback function
    bhy.onResultEvent(SENSOR_ID_GYRO_PASS, gyro_process_callback);

    return true;
}

#endif /* BOARD_HAS_BHI260AP */