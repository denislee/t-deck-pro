#ifndef __PERIPHERAL_H__
#define __PERIPHERAL_H__

#define GPS_PRIORITY     (configMAX_PRIORITIES - 8)
#define A7682E_PRIORITY  (configMAX_PRIORITIES - 5)
// Above the Arduino loopTask (priority 1) so the keypad drains even while the
// main loop is blocked inside an e-paper SPI refresh.
#define KEYPAD_PRIORITY  (configMAX_PRIORITIES - 6)

// Mutex around the primary I2C bus (Wire on BOARD_I2C_SDA/SCL). Any code that
// touches Wire while the keypad task is running must take this around its
// full transaction (beginTransmission / write* / endTransmission, or any lib
// call that wraps those). Without it, the keypad task can preempt mid-call
// and corrupt TwoWire's internal txBuffer state.
void i2c0_lock(void);
void i2c0_unlock(void);
bool i2c0_held_by_me(void);

#ifdef __cplusplus
/* Holds the I2C-0 bus mutex for the enclosing scope. */
struct I2C0Guard {
    I2C0Guard()  { i2c0_lock(); }
    ~I2C0Guard() { i2c0_unlock(); }
    I2C0Guard(const I2C0Guard &) = delete;
    I2C0Guard &operator=(const I2C0Guard &) = delete;
};
#endif

enum {
    E_PERI_LORA = 0,
    E_PERI_KYEPAD,
    E_PERI_BQ25896,
    E_PERI_BQ27220,
    E_PERI_SD,
    E_PERI_GPS,
    E_PERI_BHI260AP,
    E_PERI_LTR_553ALS,
    E_PERI_A7682E,
    E_PERI_PCM5102A,
    E_PERI_INK_SCREEN,
    E_PERI_NUM_MAX,
};

// lora sx1262
#define LORA_MODE_SEND 0
#define LORA_MODE_RECV 1

bool lora_init(void);
void lora_set_mode(int mode);
int lora_get_mode(void);
void lora_receive_loop(void);
void lora_transmit(const char *str);
bool lora_get_recv(const char **str, int *rssi);
void lora_set_recv_flag(void);
void lora_sleep(void);
void lora_power_off(void);
bool lora_param_set(void);
// keypad
#define KEYPAD_PRESS   1
#define KEYPAD_RELEASE 0

// Key codes the keypad driver pushes for the non-printable keys.
#define KEY_ENTER     '\r'
#define KEY_ESC       0x1B
#define KEY_BACKSPACE 0x08

bool keypad_init(int address);
int keypad_get_val(char *c);
void keypad_set_flag(void);
void keypad_task_create(void);
// millis() of the last key pushed into the ring buffer (0 before the first).
uint32_t keypad_last_activity_ms(void);
// Re-arm the drain task when INT is already low (after light sleep).
void keypad_kick(void);

// gyro — compiled only when the BHI260AP hardware is present.
// Without BOARD_HAS_BHI260AP (the default 4G build) the 103 KB firmware blob
// is excluded from the link and these entry points become inline no-ops.
#ifdef BOARD_HAS_BHI260AP
bool BHI260AP_init(void);
void BHI260AP_get_val(int val_type, float *x, float *y, float *z);
#else
static inline bool BHI260AP_init(void) { return false; }
static inline void BHI260AP_get_val(int val_type, float *x, float *y, float *z)
{
    (void)val_type;
    if (x) *x = 0.0f;
    if (y) *y = 0.0f;
    if (z) *z = 0.0f;
}
#endif /* BOARD_HAS_BHI260AP */

// LTR553
bool LTR553_init(void);

// gps u-blox m10q
bool gps_init(void);
void gps_task_suspend(void);
void gps_task_resume(void);
void gps_set_continuous(bool on);
// True while the module is powered and its UART is being drained.
bool gps_module_active(void);
void gps_get_coord(double *lat, double *lng);
void gps_get_data(uint16_t *year, uint8_t *month, uint8_t *day);
void gps_get_time(uint8_t *hour, uint8_t *minute, uint8_t *second);
void gps_get_satellites(uint32_t *vsat);
void gps_get_speed(double *speed);

#endif