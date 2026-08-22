#ifdef BOARD_HAS_TOUCH

#include "hyn_core.h"
#ifdef ARDUINO
#include <Arduino.h>
#include <Wire.h>
#include "peripheral.h"
#endif

esp_err_t hyn_i2c_init(u8 pin_sda ,u8 pin_scl)
{
#ifdef ARDUINO
    (void)pin_sda;
    (void)pin_scl;
    return ESP_OK;
#else
    int i2c_master_port = 0;
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = pin_sda,
        .scl_io_num = pin_scl,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 200000,
    };
    i2c_param_config(i2c_master_port, &conf);
    return i2c_driver_install(i2c_master_port, conf.mode, 0, 0, 0);
#endif
}

int hyn_write_data(struct hyn_ts_data *ts_data, u8 *buf, u8 reg_len, u16 len)
{
#ifdef ARDUINO
    (void)reg_len;
    i2c0_lock();
    Wire.beginTransmission(ts_data->salve_addr);
    for (u16 i = 0; i < len; ++i) {
        Wire.write(buf[i]);
    }
    uint8_t err = Wire.endTransmission(true);
    i2c0_unlock();
    return err == 0 ? 0 : -1;
#else
    int ret = i2c_master_write_to_device(0, ts_data->salve_addr, buf, len, 1000 / portTICK_RATE_MS);
    return ret < 0 ? -1 : 0;
#endif
}

int hyn_read_data(struct hyn_ts_data *ts_data,u8 *buf, u16 len)
{
#ifdef ARDUINO
    i2c0_lock();
    int n = Wire.requestFrom((int)ts_data->salve_addr, (int)len);
    if (n != (int)len) { i2c0_unlock(); return -1; }
    for (u16 i = 0; i < len && Wire.available(); ++i) {
        buf[i] = Wire.read();
    }
    i2c0_unlock();
    return 0;
#else
    int ret = i2c_master_read_from_device(0, ts_data->salve_addr, buf, len, 1000 / portTICK_RATE_MS);
    return ret < 0 ? -1 : 0;
#endif
}

int hyn_wr_reg(struct hyn_ts_data *ts_data, u32 reg_addr, u8 reg_len, u8 *rbuf, u16 rlen)
{
#ifdef ARDUINO
    i2c0_lock();
    Wire.beginTransmission(ts_data->salve_addr);
    for (int i = reg_len - 1; i >= 0; --i) {
        uint8_t b = (reg_addr >> (i * 8)) & 0xFF;
        Wire.write(b);
    }
    uint8_t err = Wire.endTransmission(rlen ? false : true);
    if (err != 0) { i2c0_unlock(); return -1; }
    if (rlen) {
        int n = Wire.requestFrom((int)ts_data->salve_addr, (int)rlen);
        if (n != (int)rlen) { i2c0_unlock(); return -1; }
        for (u16 i = 0; i < rlen && Wire.available(); ++i) {
            rbuf[i] = Wire.read();
        }
    }
    i2c0_unlock();
    return 0;
#else
    u8 wbuf[4];
    reg_len = reg_len & 0x0F;
    memset(wbuf, 0, sizeof(wbuf));
    int i = reg_len;
    while(i){
        i--;
        wbuf[i] = reg_addr;
        reg_addr >>= 8;
    }
    int ret = i2c_master_write_to_device(0, ts_data->salve_addr, wbuf, reg_len, 1000 / portTICK_RATE_MS);
    if (rlen) {
        ret |= i2c_master_read_from_device(0, ts_data->salve_addr, rbuf, rlen, 1000 / portTICK_RATE_MS);
    }
    return ret < 0 ? -1 : 0;
#endif
}

void hyn_delay_ms(int cnt)
{
#ifdef ARDUINO
    vTaskDelay(cnt/portTICK_PERIOD_MS);
#else
    vTaskDelay(cnt/portTICK_PERIOD_MS);
#endif
}

int gpio_set_value(uint32_t gpio_id,bool vlue)
{
    gpio_set_level((gpio_num_t)gpio_id, vlue);
    return 0;
}

bool gpio_get_value(uint32_t gpio_id)
{
    return gpio_get_level((gpio_num_t)gpio_id);
}

#endif /* BOARD_HAS_TOUCH */
