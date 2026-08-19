// TFT_eSPI configuration for the Marauder Mini V3 (ESP32-C5).
// Select this file from TFT_eSPI's User_Setup_Select.h, or copy it over
// TFT_eSPI/User_Setup.h before building.

#define USER_SETUP_INFO "Marauder Mini V3 ESP32-C5"

#define ST7735_DRIVER
#define TFT_RGB_ORDER TFT_BGR
#define TFT_WIDTH 128
#define TFT_HEIGHT 128
#define ST7735_GREENTAB3
#define TFT_BACKLIGHT_ON LOW

// The display and SD card share this SPI bus on production Mini V3 boards.
#define TFT_MISO 2
#define TFT_MOSI 7
#define TFT_SCLK 6
#define TFT_CS 23
#define TFT_DC 24
#define TFT_RST -1
#define TFT_BL 5
#define TOUCH_CS -1

#define LOAD_GLCD
#define LOAD_FONT2
#define LOAD_FONT4
#define LOAD_FONT6
#define LOAD_FONT7
#define LOAD_FONT8
#define LOAD_GFXFF
#define SMOOTH_FONT

#define SPI_FREQUENCY 20000000
#define SPI_READ_FREQUENCY 20000000
#define SPI_TOUCH_FREQUENCY 2500000
