#pragma once
#include <Arduino.h>
#include "board.h"

// Small always-on status screen for boards with an ST7789 240x240 LCD.
// Shows IP, time, AirSense state, mask pressure (during therapy) and SpO2.
// The backlight idles at AB_LCD_DIM_DUTY and goes to full brightness for
// AB_LCD_WAKE_MS after any wake button press.

namespace StatusScreen {
#if AB_LCD_ENABLED
void init();            // call early in setup(); shows a "starting" message
void tick();            // call from loop()
void status(String &out);   // $LCD: backlight, wake timer, button levels
#else
inline void init() {}
inline void tick() {}
inline void status(String &out) { out = "lcd: not enabled in this build\n"; }
#endif
}
