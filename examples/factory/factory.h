#ifndef __TEST_FACTORY_H__
#define __TEST_FACTORY_H__

/*********************************************************************************
 *                                  INCLUDES
 * *******************************************************************************/
#include "peripheral.h"
#include "utilities.h"
#define XPOWERS_CHIP_BQ25896
#include <XPowersLib.h>
#include "bq27220.h"
#ifdef BOARD_HAS_PCM5102A
#include "Audio.h"
#endif
#include "FS.h"
#include "SD.h"
#include "SPI.h"
#include "SPIFFS.h"

/*********************************************************************************
 *                                   DEFINES
 * *******************************************************************************/

#define DISP_REFR_MODE_FULL 0
#define DISP_REFR_MODE_PART 1
#define EPD_BITMAP_STRIDE(width) (((width) + 7) / 8)
#define EPD_BITMAP_BUF_SIZE (EPD_BITMAP_STRIDE(LCD_HOR_SIZE) * LCD_VER_SIZE)

#ifndef TINY_GSM_MODEM_SIM7672
#define TINY_GSM_MODEM_SIM7672
#endif
#define TINY_GSM_RX_BUFFER 1024 // Set RX buffer to 1Kb

#include <TinyGsmClient.h>

extern TinyGsm modem;
extern TaskHandle_t a7682_handle;
/*********************************************************************************
 *                                   MACROS
 * *******************************************************************************/
extern bool peri_init_st[E_PERI_NUM_MAX];
extern XPowersPPM PPM;
extern BQ27220 bq27220;
#ifdef BOARD_HAS_PCM5102A
extern Audio audio;
#endif

/*********************************************************************************
 *                                  TYPEDEFS
 * *******************************************************************************/

/*********************************************************************************
 *                              GLOBAL PROTOTYPES
 * *******************************************************************************/
#ifdef __cplusplus
extern "C" {
#endif

void board_rails_hold(bool hold);
void disp_full_refr(void); // Next global refresh
void disp_hard_refresh(void); // Thorough B/W cycle refresh
void disp_white_clear(void); // Single full-screen white frame
void factory_set_landscape(bool landscape); // Reader-view rotation hook
void ink_screen_prepare_shutdown(void);
void shared_spi_bus_init(void);
void shared_spi_lock(void);
void shared_spi_unlock(void);

#ifdef __cplusplus
}

/* Holds the shared SPI bus (LoRa / SD / EPD) for the enclosing scope. */
struct SpiGuard {
    SpiGuard()  { shared_spi_lock(); }
    ~SpiGuard() { shared_spi_unlock(); }
    SpiGuard(const SpiGuard &) = delete;
    SpiGuard &operator=(const SpiGuard &) = delete;
};
#endif

#endif
