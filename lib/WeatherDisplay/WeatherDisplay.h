#pragma once
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>

// RGB565 colour constants — use these when calling showStatus().
namespace WD {
  static const uint16_t BLACK  = 0x0000;
  static const uint16_t WHITE  = 0xFFFF;
  static const uint16_t YELLOW = 0xFFE0;
  static const uint16_t CYAN   = 0x07FF;
  static const uint16_t GREEN  = 0x07E0;
  static const uint16_t RED    = 0xF800;
  static const uint16_t ORANGE = 0xFD20;
  static const uint16_t BLUE   = 0x001F;
  static const uint16_t GRAY   = 0x8410;
}

class WeatherDisplay {
public:
  // cs/dc/rst: GPIO numbers matching your wiring.
  // city: label shown in the header bar (e.g. "London").
  WeatherDisplay(uint8_t cs, uint8_t dc, uint8_t rst, const char* city);

  // Initialise the TFT and show the startup screen.
  // tab: INITR_BLACKTAB (default), INITR_REDTAB, or INITR_GREENTAB —
  //      try the others if colours look wrong after first flash.
  void begin(uint8_t tab = INITR_BLACKTAB);

  // Show a full-screen "Connecting to <ssid>" splash.
  void showConnecting(const char* ssid);

  // Overwrite the status bar at the very bottom of the screen.
  void showStatus(const char* msg, uint16_t col = WD::GRAY);

  // Render the complete weather screen.
  void drawWeather(float tempC, float feelsC, int humidity,
                   float windKmh, const char* condition, const char* date);

private:
  Adafruit_ST7735 _tft;
  const char*     _city;

  uint16_t _tempColour(float t);
  void     _centred(const char* str, int16_t y, uint16_t col, uint8_t sz);
  void     _header();
  void     _drawTemp(float t);

  void _iconSun(int16_t cx, int16_t cy);
  void _iconCloud(int16_t cx, int16_t cy, uint16_t col);
  void _iconSmallSun(int16_t cx, int16_t cy);
  void _iconPartlyCloudy(int16_t cx, int16_t cy);
  void _iconRain(int16_t cx, int16_t cy);
  void _iconSnow(int16_t cx, int16_t cy);
  void _iconStorm(int16_t cx, int16_t cy);
  void _iconFog(int16_t cx, int16_t cy);
  void _conditionIcon(int16_t cx, int16_t cy, const char* cond);
  void _iconDroplet(int16_t x, int16_t y, uint16_t col);
  void _iconWind(int16_t x, int16_t y, uint16_t col);
};
