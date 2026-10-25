#pragma once

// A7682E modem pins (mirrors examples/A7682E/test_AT/utilities.h)
// NOTE: pin 41 is named BOARD_6609_EN in examples/factory/utilities.h (same rail).
#define BOARD_A7682E_POWER_EN 41 // VBAT -> modem rail enable
#define BOARD_A7682E_RST      9
#define BOARD_A7682E_RXD      10 // modem RX -> ESP TX
#define BOARD_A7682E_TXD      11 // modem TX -> ESP RX
#define BOARD_A7682E_PWRKEY   40
