#pragma once

#include "../factory/utilities.h"  // single source of truth for the board pins

// Tethering-only aliases for the A7682E pin names the factory sketch spells
// differently (or documents for reference only).
#define BOARD_A7682E_POWER_EN BOARD_6609_EN  // same rail (GPIO 41)
#ifndef BOARD_A7682E_RST
#define BOARD_A7682E_RST 9
#endif
