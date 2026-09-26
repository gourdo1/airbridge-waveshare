#pragma once


// UART1 -> AirSense USART3
#ifndef AB_AS10_TX_GPIO
#define AB_AS10_TX_GPIO 26
#endif

#ifndef AB_AS10_RX_GPIO
#define AB_AS10_RX_GPIO 36      // input-only on M5Stamp Pico
#endif

// Drive the AirSense TX pin open-drain (relies on an external pull-up). For
// wiring where TX shares a line with other open-drain devices, e.g. I2C.
#ifndef AB_AS10_TX_OPEN_DRAIN
#define AB_AS10_TX_OPEN_DRAIN 0
#endif

// Touch controller reset; held low at boot to keep it off a shared I2C bus
// (-1 = leave alone)
#ifndef AB_TOUCH_RST_GPIO
#define AB_TOUCH_RST_GPIO -1
#endif

#ifndef AB_LED_GPIO
#define AB_LED_GPIO 27
#endif

// LCD backlight; driven low at boot until the status screen takes over (-1 = none)
#ifndef AB_LCD_BL_GPIO
#define AB_LCD_BL_GPIO -1
#endif

// ST7789 240x240 SPI status screen (src/status_screen.cpp). 0 = no screen.
#ifndef AB_LCD_ENABLED
#define AB_LCD_ENABLED 0
#endif

#ifndef AB_LCD_MOSI_GPIO
#define AB_LCD_MOSI_GPIO -1
#endif

#ifndef AB_LCD_SCLK_GPIO
#define AB_LCD_SCLK_GPIO -1
#endif

#ifndef AB_LCD_CS_GPIO
#define AB_LCD_CS_GPIO -1
#endif

#ifndef AB_LCD_DC_GPIO
#define AB_LCD_DC_GPIO -1
#endif

#ifndef AB_LCD_RST_GPIO
#define AB_LCD_RST_GPIO -1
#endif

// Backlight PWM duty (0-1023) when idle. Tune on hardware to the lowest
// level that is still readable in a dark room.
#ifndef AB_LCD_DIM_DUTY
#define AB_LCD_DIM_DUTY 8
#endif

// How long a button press keeps the backlight at full brightness
#ifndef AB_LCD_WAKE_MS
#define AB_LCD_WAKE_MS 10000
#endif

// Active-low buttons that wake the screen (-1 = unused)
#ifndef AB_BTN1_GPIO
#define AB_BTN1_GPIO -1
#endif

#ifndef AB_BTN2_GPIO
#define AB_BTN2_GPIO -1
#endif

#ifndef AB_BTN3_GPIO
#define AB_BTN3_GPIO -1
#endif

// Battery power latch; driven high at boot so the board stays on (-1 = none)
#ifndef AB_POWER_HOLD_GPIO
#define AB_POWER_HOLD_GPIO -1
#endif

#ifndef AB_STORAGE_SDMMC_ENABLED
#define AB_STORAGE_SDMMC_ENABLED 0
#endif

#ifndef AB_SDMMC_WIDTH
#define AB_SDMMC_WIDTH 4
#endif

#ifndef AB_SDMMC_FREQ_KHZ
#define AB_SDMMC_FREQ_KHZ 20000
#endif

#ifndef AB_SDMMC_CLK_GPIO
#define AB_SDMMC_CLK_GPIO -1
#endif

#ifndef AB_SDMMC_CMD_GPIO
#define AB_SDMMC_CMD_GPIO -1
#endif

#ifndef AB_SDMMC_D0_GPIO
#define AB_SDMMC_D0_GPIO -1
#endif

#ifndef AB_SDMMC_D1_GPIO
#define AB_SDMMC_D1_GPIO -1
#endif

#ifndef AB_SDMMC_D2_GPIO
#define AB_SDMMC_D2_GPIO -1
#endif

#ifndef AB_SDMMC_D3_GPIO
#define AB_SDMMC_D3_GPIO -1
#endif

#ifndef AB_STORAGE_HAS_SDCARD
#define AB_STORAGE_HAS_SDCARD (AB_STORAGE_SDMMC_ENABLED != 0)
#endif

// Backward-compatible aliases used by the current firmware.
#define PIN_AS10_TX     AB_AS10_TX_GPIO
#define PIN_AS10_RX     AB_AS10_RX_GPIO
#define PIN_LED         AB_LED_GPIO

// Phase 2: MITM modem interception
// #define MODEM_UART_NUM  2
// #define PIN_MODEM_TX    18
// #define PIN_MODEM_RX    19


#define DEFAULT_HOSTNAME    "airbridge"
#define DEFAULT_OTA_PORT    3232
