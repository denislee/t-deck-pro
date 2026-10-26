
#include <Adafruit_TCA8418.h>
#include <Adafruit_TCA8418_registers.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "utilities.h"
#include "peripheral.h"
#include <ctype.h>

extern TaskHandle_t loopTaskHandle;  // Arduino core (main.cpp): the task running loop() / LVGL
static volatile uint32_t keypad_last_key_ms = 0;

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
    // shared KEY_BACKSPACE code (0x08) so text consumers delete a character
    // instead of inserting the digit '0'.
    {'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', KEY_BACKSPACE},
    // Index 0 of row 2 is the physical key left of Z. Acts as a sym modifier
    // (swapped with the bottom-row left-shift slot for thumb ergonomics).
    {KEY_SYM_SENTINEL, 'z', 'x', 'c', 'v', 'b', 'n', 'm', '$', KEY_ENTER},
    // Bottom row has a left-shift, mic, wide spacebar, sym, and right-shift.
    // Left-shift (index 5) is a held modifier that upper-cases letters at
    // lookup time (shift takes precedence over sym when both are held). The
    // mic key (index 6) emits KEY_ESC so it doubles as Escape. The wide
    // spacebar (index 7, col 2 after the col reversal) emits a literal
    // space. Sym at index 8 is a held modifier; the top-of-z-row slot also
    // acts as sym so either thumb can reach it.
    {' ', ' ', ' ', ' ', ' ', KEY_SHIFT_SENTINEL, KEY_ESC, ' ', KEY_SYM_SENTINEL, ' '},
};

// Symbol overlay applied while the sym key is held. Reads off each key's
// upper-left glyph on the physical keyboard. Non-printable slots (modifiers,
// backspace, enter) are kept identical to keymap so they still work with sym
// held.
const char sym_keymap[KEYPAD_ROWS][KEYPAD_COLS] = {
    {'#', '1', '2', '3', '(', ')', '_', '-', '+', '@'},
    {'*', '4', '5', '6', '/', ':', ';', '\'', '"', KEY_BACKSPACE},
    {KEY_SYM_SENTINEL, '7', '8', '9', '?', '!', ',', '.', '$', KEY_ENTER},
    {' ', ' ', ' ', ' ', ' ', KEY_SHIFT_SENTINEL, KEY_ESC, ' ', KEY_SYM_SENTINEL, ' '},
};

Adafruit_TCA8418 keypad;

// Binary semaphore given by the INT ISR and taken by keypad_task. Created in
// keypad_task_create(); safe to check for NULL before any xSemaphore call.
static SemaphoreHandle_t keypad_irq_sem = NULL;

// Re-arm the drain task after light sleep, when INT is already low and the
// ISR will therefore see no edge.
void keypad_kick(void)
{
    if (keypad_irq_sem) xSemaphoreGive(keypad_irq_sem);
}

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

static void i2c0_lock_init(void)
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

bool i2c0_held_by_me(void)
{
    return i2c0_mutex && xSemaphoreGetMutexHolder(i2c0_mutex) == xTaskGetCurrentTaskHandle();
}

// Ring buffer of pressed-key characters. The producer (keypad_loop) runs from
// the IRQ-driven keypad task; consumers (UI polls) often run at 10-50 Hz
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

// Tracks whether the sym/shift modifiers are currently held. Updated from
// keypad_loop on press/release of the corresponding modifier key.
static volatile bool sym_held = false;
static volatile bool shift_held = false;

bool keypad_init(int address)
{
    i2c0_lock_init();

    i2c0_lock();
    if (!keypad.begin(address, &Wire)) {
        i2c0_unlock();
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

static void keypad_loop(void)
{
    bool pushed = false;

    // Drain every event the TCA8418 has queued. Held under the I2C bus mutex so
    // a concurrent main-loop Wire user (PMU, gauge, touch) cannot interleave
    // with our reads and corrupt the TwoWire txBuffer state.
    i2c0_lock();

    for (int pass = 0; pass < 4; pass++) {
        // Clear K_INT (and GPI_INT) BEFORE draining: a key that lands mid-drain
        // re-asserts INT and produces a fresh falling edge instead of being
        // lost until the safety-net timeout; the re-check of the INT pin below
        // covers a chip that refuses to release INT while its FIFO is
        // non-empty. Must be done while i2c0_lock is still held — before
        // i2c0_unlock() below.
        keypad.writeRegister(TCA8418_REG_INT_STAT,
                             TCA8418_REG_STAT_K_INT | TCA8418_REG_STAT_GPI_INT);

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
            if (shift_held)    c = toupper((unsigned char)keymap[row][col]);
            else if (sym_held) c = sym_keymap[row][col];
            else               c = keymap[row][col];
            keypad_buf_push(c);
            pushed = true;
        }

        // INT released after this pass → nothing left queued, stop here.
        if (digitalRead(BOARD_KEYBOARD_INT) == HIGH) {
            break;
        }
    }

    i2c0_unlock();

    // Wake the LVGL loop straight away instead of letting it poll the ring
    // buffer.
    if (pushed) {
        keypad_last_key_ms = millis();
        if (loopTaskHandle) xTaskNotifyGive(loopTaskHandle);
    }
}

// millis() of the last key that reached the ring buffer; 0 if none yet.
uint32_t keypad_last_activity_ms(void)
{
    return keypad_last_key_ms;
}

// Dedicated drain task. Blocks on keypad_irq_sem, which is given by the
// falling-edge ISR on BOARD_KEYBOARD_INT. When the TCA8418 asserts INT the
// task wakes, drains the full FIFO in one pass (keypad_loop), and clears
// INT_STAT so the line de-asserts. The 5 s timeout is a safety net: if the
// ISR was missed (e.g. INT was already low at attachInterrupt time, or a
// glitch prevented the edge) the task re-syncs by draining and clearing.
// Because keypad_loop() clears INT_STAT unconditionally, the timeout path is
// a genuine recovery — not just a no-op poll.
static TaskHandle_t keypad_task_handle = NULL;

static void keypad_task(void *param)
{
    (void)param;
    uint8_t fast_retries = 0;
    for (;;) {
        // Block until INT fires or the safety-net timeout expires. If the last
        // drain gave up with INT still low (keys kept arriving through every
        // pass), no new falling edge will come, so re-drain soon rather than
        // after the 5 s safety net. Capped so a line stuck low (faulty or
        // absent TCA8418) does not poll I2C at 50 Hz forever.
        bool int_low = (digitalRead(BOARD_KEYBOARD_INT) == LOW);
        if (!int_low) fast_retries = 0;
        else if (fast_retries <= 10) fast_retries++;
        const TickType_t timeout = (int_low && fast_retries <= 10)
                                       ? pdMS_TO_TICKS(20) : pdMS_TO_TICKS(5000);
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

    // Priority above loopTask (1) so we preempt the e-paper SPI refresh, and
    // above the GPS task (KEYPAD_PRIORITY 19 vs GPS 17). 4 KB stack: nominally
    // a TCA8418 drain is tiny, but when the I2C bus is unhealthy (Error 263
    // timeouts on shared peripherals) the Wire error-handling path eats
    // hundreds of additional bytes — 2 KB tripped the stack canary in practice.
    xTaskCreate(keypad_task, "keypad", 4096, NULL, KEYPAD_PRIORITY, &keypad_task_handle);
}
