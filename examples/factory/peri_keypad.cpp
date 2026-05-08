
#include <Adafruit_TCA8418.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "utilities.h"
#include "peripheral.h"

#define KEYPAD_ROWS 4
#define KEYPAD_COLS 10
#define KEYPAD_PRESS_VAL_MIN   129
#define KEYPAD_PRESS_VAL_MAX   163
#define KEYPAD_RELEASE_VAL_MIN 1
#define KEYPAD_RELEASE_VAL_MAX 35

// Modifier-key sentinels. Picked outside printable ASCII (32-126) so text
// consumers skip them, but distinct so modifier-aware code can tell them
// apart. Only sym is currently treated as a held modifier.
#define KEY_ALT_SENTINEL  0x1B
#define KEY_SYM_SENTINEL  0x1A

const char keymap[KEYPAD_ROWS][KEYPAD_COLS] = {
    {'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p'},
    // Index 9 of row 1 is the physical backspace key (right of L). Use the
    // standard ASCII BS code (0x08) so text consumers delete a character
    // instead of inserting the digit '0'.
    {'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', 0x08},
    // Index 0 of row 2 is the physical key left of Z. Acts as a sym modifier
    // (swapped with the bottom-row left-shift slot for thumb ergonomics).
    {KEY_SYM_SENTINEL, 'z', 'x', 'c', 'v', 'b', 'n', 'm', '$', 'E'},
    // Bottom row has a left-shift, mic, wide spacebar, sym, and right-shift.
    // The left-shift slot (index 5) emits the alt sentinel, while the
    // top-of-z-row slot acts as sym — the two were swapped from the silkscreen
    // labels for thumb ergonomics. Sym at index 8 still works as a sym
    // modifier so either thumb can reach it. The right-shift (index 9)
    // doubles as a secondary spacebar. The wide spacebar lands on native
    // col 2 (array index 7 after the col reversal), which used to be 'S' —
    // pressing space registered as the letter S.
    {' ', ' ', ' ', ' ', ' ', KEY_ALT_SENTINEL, '*', ' ', KEY_SYM_SENTINEL, ' '},
};

// Symbol overlay applied while the sym key is held. Reads off each key's
// upper-left glyph on the physical keyboard. Non-printable slots (modifiers,
// backspace, enter) are kept identical to keymap so they still work with sym
// held.
const char sym_keymap[KEYPAD_ROWS][KEYPAD_COLS] = {
    {'#', '1', '2', '3', '(', ')', '_', '-', '+', '@'},
    {'*', '4', '5', '6', '/', ':', ';', '\'', '"', 0x08},
    {KEY_SYM_SENTINEL, '7', '8', '9', '?', '!', ',', '.', '$', 'E'},
    {' ', ' ', ' ', ' ', ' ', KEY_ALT_SENTINEL, '*', ' ', KEY_SYM_SENTINEL, ' '},
};

Adafruit_TCA8418 keypad;
keypad_cb keypad_listener = NULL;

// Mutex serializing access to the primary I2C bus (Wire on BOARD_I2C_SDA/SCL).
// The keypad task drains the TCA8418 from a higher-priority context that can
// preempt the main loop mid-Wire-transaction; without this lock, txBuffer state
// inside the TwoWire object would corrupt across threads.
static SemaphoreHandle_t i2c0_mutex = NULL;

void i2c0_lock_init(void)
{
    if (!i2c0_mutex) {
        i2c0_mutex = xSemaphoreCreateMutex();
    }
}

void i2c0_lock(void)
{
    if (i2c0_mutex) {
        xSemaphoreTake(i2c0_mutex, portMAX_DELAY);
    }
}

void i2c0_unlock(void)
{
    if (i2c0_mutex) {
        xSemaphoreGive(i2c0_mutex);
    }
}

// Ring buffer of pressed-key characters. The producer (keypad_loop) runs at
// roughly 1 kHz from the main loop; consumers (UI polls) often run at 10-50 Hz
// so without a buffer fast typing drops keys. Keep this a power-of-two for
// cheap masking, and large enough to absorb a quick burst (~32 chars).
#define KEYPAD_BUF_SIZE 32
#define KEYPAD_BUF_MASK (KEYPAD_BUF_SIZE - 1)
static volatile char keypad_buf[KEYPAD_BUF_SIZE];
static volatile uint8_t keypad_buf_head = 0; // write index
static volatile uint8_t keypad_buf_tail = 0; // read index

static inline bool keypad_buf_empty(void) { return keypad_buf_head == keypad_buf_tail; }

static inline void keypad_buf_push(char c)
{
    uint8_t next = (keypad_buf_head + 1) & KEYPAD_BUF_MASK;
    if (next == keypad_buf_tail) {
        // Buffer full: drop the oldest to keep the most recent keystrokes.
        keypad_buf_tail = (keypad_buf_tail + 1) & KEYPAD_BUF_MASK;
    }
    keypad_buf[keypad_buf_head] = c;
    keypad_buf_head = next;
}

int keypad_state = KEYPAD_RELEASE;

// Tracks whether the sym modifier is currently held. Updated from
// keypad_loop on press/release of the sym key.
static volatile bool sym_held = false;

bool keypad_init(int address)
{
    i2c0_lock_init();

    i2c0_lock();
    if(!i2cIsInit(0)){
        Wire.begin(BOARD_KEYBOARD_SDA, BOARD_KEYBOARD_SCL);
        Wire.beginTransmission(address);
        Wire.endTransmission(true);
    }

    if (!keypad.begin(address, &Wire)) {
        i2c0_unlock();
        // Serial.println("keypad not found, check wiring & pullups!");
        log_e("keypad not found, check wiring & pullups!");
        return false;
    }

    // configure the size of the keypad matrix.
    // all other pins will be inputs
    keypad.matrix(KEYPAD_ROWS, KEYPAD_COLS);

    // flush the internal buffer
    keypad.flush();
    i2c0_unlock();

    return true;
}

// Peek the oldest buffered key without removing it. Returns 1 if a key is
// available, 0 otherwise. Callers consume by calling keypad_set_flag().
int keypad_get_val(char *c)
{
    if (keypad_buf_empty()) {
        return 0;
    }
    if (c) {
        *c = keypad_buf[keypad_buf_tail];
    }
    return 1;
}

// Consume the oldest buffered key. Pairs with keypad_get_val().
void keypad_set_flag(void)
{
    if (!keypad_buf_empty()) {
        keypad_buf_tail = (keypad_buf_tail + 1) & KEYPAD_BUF_MASK;
    }
}

void keypad_loop(void)
{
    // Drain every event the TCA8418 has queued. Held under the I2C bus mutex so
    // a concurrent main-loop Wire user (PMU, gauge, touch) cannot interleave
    // with our reads and corrupt the TwoWire txBuffer state.
    i2c0_lock();
    while (true) {
        int k = keypad.getEvent();
        if (k <= 0) {
            break;
        }
        int state;
        if (k >= KEYPAD_RELEASE_VAL_MIN && k <= KEYPAD_RELEASE_VAL_MAX) {
            k -= KEYPAD_RELEASE_VAL_MIN;
            state = KEYPAD_RELEASE;
        } else if (k >= KEYPAD_PRESS_VAL_MIN && k <= KEYPAD_PRESS_VAL_MAX) {
            k -= KEYPAD_PRESS_VAL_MIN;
            state = KEYPAD_PRESS;
        } else {
            continue;
        }

        int row = k / KEYPAD_COLS;
        int col = (KEYPAD_COLS - 1) - k % KEYPAD_COLS;

        // Sym is a held modifier — track its state on both press and release,
        // and never push its sentinel to the buffer.
        if (keymap[row][col] == KEY_SYM_SENTINEL) {
            sym_held = (state == KEYPAD_PRESS);
            continue;
        }

        if (state != KEYPAD_PRESS) {
            continue;
        }

        char c = sym_held ? sym_keymap[row][col] : keymap[row][col];
        keypad_state = state;
        keypad_buf_push(c);
    }
    i2c0_unlock();
}

// Dedicated drain task. Runs above the Arduino loopTask so the TCA8418's
// 10-deep on-chip FIFO gets serviced even while the main loop is blocked
// inside lv_task_handler -> e-paper SPI refresh (which can take >300ms).
// At ~5ms cadence the FIFO can absorb >2000 events/sec — well above any
// realistic typing rate — so presses no longer drop on the chip side.
static TaskHandle_t keypad_task_handle = NULL;

static void keypad_task(void *param)
{
    (void)param;
    const TickType_t period = pdMS_TO_TICKS(5);
    TickType_t last = xTaskGetTickCount();
    for (;;) {
        keypad_loop();
        vTaskDelayUntil(&last, period);
    }
}

void keypad_task_create(void)
{
    if (keypad_task_handle) {
        return;
    }
    // Priority above loopTask (1) so we preempt the e-paper SPI refresh, but
    // below the time-critical GPS/LoRa tasks. Stack 2KB is plenty for the
    // small Wire+TCA8418 transactions we do here.
    xTaskCreate(keypad_task, "keypad", 2048, NULL, KEYPAD_PRIORITY, &keypad_task_handle);
}

void keypad_regetser_cb(keypad_cb cb)
{
    keypad_listener = cb;
}