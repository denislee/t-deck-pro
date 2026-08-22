
#include <Adafruit_TCA8418.h>
#include <Adafruit_TCA8418_registers.h>
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

// Modifier sentinels. Picked outside printable ASCII (32-126) so text
// consumers skip them. Both sym and shift are held modifiers tracked across
// press/release events; the sentinels themselves never reach the input buffer.
#define KEY_SYM_SENTINEL    0x1A
#define KEY_SHIFT_SENTINEL  0x19

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
    // Left-shift (index 5) is a held modifier that selects shift_keymap. The
    // mic key (index 6) emits ASCII ESC (0x1B) so it doubles as Escape. The
    // wide spacebar (index 7, col 2 after the col reversal) emits a literal
    // space. Sym at index 8 is a held modifier; the top-of-z-row slot also
    // acts as sym so either thumb can reach it.
    {' ', ' ', ' ', ' ', ' ', KEY_SHIFT_SENTINEL, 0x1B, ' ', KEY_SYM_SENTINEL, ' '},
};

// Symbol overlay applied while the sym key is held. Reads off each key's
// upper-left glyph on the physical keyboard. Non-printable slots (modifiers,
// backspace, enter) are kept identical to keymap so they still work with sym
// held.
const char sym_keymap[KEYPAD_ROWS][KEYPAD_COLS] = {
    {'#', '1', '2', '3', '(', ')', '_', '-', '+', '@'},
    {'*', '4', '5', '6', '/', ':', ';', '\'', '"', 0x08},
    {KEY_SYM_SENTINEL, '7', '8', '9', '?', '!', ',', '.', '$', 'E'},
    {' ', ' ', ' ', ' ', ' ', KEY_SHIFT_SENTINEL, 0x1B, ' ', KEY_SYM_SENTINEL, ' '},
};

// Uppercase overlay applied while the shift key is held. Non-letter slots
// match keymap so backspace, enter, ESC, and modifiers behave normally with
// shift held. Shift takes precedence over sym when both are held.
const char shift_keymap[KEYPAD_ROWS][KEYPAD_COLS] = {
    {'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P'},
    {'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', 0x08},
    {KEY_SYM_SENTINEL, 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '$', 'E'},
    {' ', ' ', ' ', ' ', ' ', KEY_SHIFT_SENTINEL, 0x1B, ' ', KEY_SYM_SENTINEL, ' '},
};

Adafruit_TCA8418 keypad;
keypad_cb keypad_listener = NULL;

// Binary semaphore given by the INT ISR and taken by keypad_task. Created in
// keypad_task_create(); safe to check for NULL before any xSemaphore call.
static SemaphoreHandle_t keypad_irq_sem = NULL;

// ISR fired on the falling edge of BOARD_KEYBOARD_INT (GPIO 15). The TCA8418
// holds the line low until INT_STAT is cleared, so a single falling edge per
// event burst is all we get. ISR gives the semaphore and yields; I2C access,
// logging, and mutex operations are all forbidden here.
static void IRAM_ATTR keypad_irq_handler(void)
{
    BaseType_t higher_woken = pdFALSE;
    xSemaphoreGiveFromISR(keypad_irq_sem, &higher_woken);
    portYIELD_FROM_ISR(higher_woken);
}

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

// Tracks whether the sym/shift modifiers are currently held. Updated from
// keypad_loop on press/release of the corresponding modifier key.
static volatile bool sym_held = false;
static volatile bool shift_held = false;

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

    // flush the internal buffer and clear any stale interrupt flags before the
    // ISR is attached, so we start from a known-clean state.
    keypad.flush();

    // Enable key-event interrupt (KE_IEN) so TCA8418 asserts INT on key state
    // changes. GPI_IEN is also set by enableInterrupts() — harmless here since
    // all GPIOs are configured as matrix rows/columns, not GPI inputs.
    keypad.enableInterrupts();

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

        // Sym/shift are held modifiers — track their state on both press and
        // release, and never push the sentinel to the buffer.
        if (keymap[row][col] == KEY_SYM_SENTINEL) {
            sym_held = (state == KEYPAD_PRESS);
            continue;
        }
        if (keymap[row][col] == KEY_SHIFT_SENTINEL) {
            shift_held = (state == KEYPAD_PRESS);
            continue;
        }

        if (state != KEYPAD_PRESS) {
            continue;
        }

        char c;
        if (shift_held)    c = shift_keymap[row][col];
        else if (sym_held) c = sym_keymap[row][col];
        else               c = keymap[row][col];
        keypad_state = state;
        keypad_buf_push(c);
    }

    // Clear K_INT (and GPI_INT) in INT_STAT so the open-drain INT line
    // de-asserts. The TCA8418 holds INT low until this register is written
    // with 1-to-clear bits, regardless of FIFO state. Must be done while
    // i2c0_lock is still held — before i2c0_unlock() below.
    keypad.writeRegister(TCA8418_REG_INT_STAT,
                         TCA8418_REG_STAT_K_INT | TCA8418_REG_STAT_GPI_INT);

    i2c0_unlock();
}

// Dedicated drain task. Blocks on keypad_irq_sem, which is given by the
// falling-edge ISR on BOARD_KEYBOARD_INT. When the TCA8418 asserts INT the
// task wakes, drains the full FIFO in one pass (keypad_loop), and clears
// INT_STAT so the line de-asserts. The 500 ms timeout is a safety net: if the
// ISR was missed (e.g. INT was already low at attachInterrupt time, or a
// glitch prevented the edge) the task re-syncs by draining and clearing.
// Because keypad_loop() clears INT_STAT unconditionally, the timeout path is
// a genuine recovery — not just a no-op poll.
static TaskHandle_t keypad_task_handle = NULL;

static void keypad_task(void *param)
{
    (void)param;
    const TickType_t timeout = pdMS_TO_TICKS(500);
    for (;;) {
        // Block until INT fires or the safety-net timeout expires.
        xSemaphoreTake(keypad_irq_sem, timeout);
        // Drain the full TCA8418 FIFO and clear INT_STAT in one locked burst.
        keypad_loop();
    }
}

void keypad_task_create(void)
{
    if (keypad_task_handle) {
        return;
    }

    // Create the binary semaphore that the INT ISR uses to wake the task.
    // xSemaphoreCreateBinary() starts unsignalled, so the task will block
    // immediately on its first xSemaphoreTake until INT fires or timeout.
    keypad_irq_sem = xSemaphoreCreateBinary();

    // Configure the INT pin with a pull-up so it idles HIGH. The TCA8418
    // open-drains the line LOW when an event is queued and holds it there
    // until INT_STAT is cleared; a falling edge therefore marks each new burst.
    pinMode(BOARD_KEYBOARD_INT, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(BOARD_KEYBOARD_INT),
                    keypad_irq_handler, FALLING);

    // Priority above loopTask (1) so we preempt the e-paper SPI refresh, but
    // below the time-critical GPS/LoRa tasks. 4 KB stack: nominally a TCA8418
    // drain is tiny, but when the I2C bus is unhealthy (Error 263 timeouts on
    // shared peripherals) the Wire error-handling path eats hundreds of
    // additional bytes — 2 KB tripped the stack canary in practice.
    xTaskCreate(keypad_task, "keypad", 4096, NULL, KEYPAD_PRIORITY, &keypad_task_handle);
}

void keypad_regetser_cb(keypad_cb cb)
{
    keypad_listener = cb;
}