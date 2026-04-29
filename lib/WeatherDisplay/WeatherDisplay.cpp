#include "WeatherDisplay.h"
#include <Arduino.h>

// ── Internal colour constants ─────────────────────────────────
static const uint16_t C_DGRAY  = 0x4208;   // dark gray  — dividers
static const uint16_t C_LGRAY  = 0xC618;   // light gray — clouds

// ── Pre-computed sun-ray endpoints ───────────────────────────
// 8 rays, inner radius ≈7, outer radius ≈11.
static const int8_t SUN_IX[] = { 7,  5,  0, -5, -7, -5,  0,  5};
static const int8_t SUN_IY[] = { 0,  5,  7,  5,  0, -5, -7, -5};
static const int8_t SUN_OX[] = {11,  8,  0, -8,-11, -8,  0,  8};
static const int8_t SUN_OY[] = { 0,  8, 11,  8,  0, -8,-11, -8};

// ─────────────────────────────────────────────────────────────

WeatherDisplay::WeatherDisplay(uint8_t cs, uint8_t dc, uint8_t rst, const char* city)
  : _tft(cs, dc, rst), _city(city) {}

void WeatherDisplay::begin(uint8_t tab) {
  _tft.initR(tab);
  _tft.setRotation(0);   // portrait, connector at bottom
  _tft.fillScreen(WD::BLACK);
  _header();
  _centred("Starting...", 70, WD::WHITE, 1);
}

void WeatherDisplay::showConnecting(const char* ssid) {
  _tft.fillScreen(WD::BLACK);
  _header();
  _centred("Connecting to", 62, WD::CYAN, 1);
  _centred(ssid, 74, WD::GRAY, 1);
}

void WeatherDisplay::showStatus(const char* msg, uint16_t col) {
  _tft.fillRect(0, 138, 128, 12, WD::BLACK);
  _tft.setTextSize(1);
  _tft.setTextColor(col);
  _tft.setTextWrap(false);
  int16_t w = (int16_t)strlen(msg) * 6;
  _tft.setCursor((128 - w) / 2, 140);
  _tft.print(msg);
}

// ── Full weather screen ───────────────────────────────────────
//
// Layout (128 × 160 portrait):
//   y  0-21  : gradient header — "<City> Weather"
//   y 24     : date (right-aligned)
//   y 30-56  : condition icon (centre cx=64, cy=42)
//   y 60     : condition text (size 1, centred)
//   y 72-95  : temperature (size 3, colour-coded, drawn degree symbol)
//   y 100    : feels-like (omitted if within 0.5° of actual temp)
//   y 112    : divider
//   y 118    : humidity (left) + wind speed (right)
//   y 130    : divider
//   y 140    : "Updated HH:MM UTC"

void WeatherDisplay::drawWeather(float tempC, float feelsC, int humidity,
                                 float windKmh, const char* condition, const char* date) {
  _tft.fillScreen(WD::BLACK);
  _header();

  // Date — right-aligned
  _tft.setTextSize(1);
  _tft.setTextColor(WD::GRAY);
  _tft.setTextWrap(false);
  _tft.setCursor(128 - (int16_t)strlen(date) * 6, 24);
  _tft.print(date);

  _conditionIcon(64, 42, condition);
  _centred(condition, 60, WD::WHITE, 1);
  _drawTemp(tempC);

  if (fabsf(feelsC - tempC) > 0.5f) {
    char buf[18];
    snprintf(buf, sizeof(buf), "feels %.1fC", feelsC);
    _centred(buf, 100, WD::GRAY, 1);
  }

  _tft.drawFastHLine(0, 112, 128, C_DGRAY);

  _iconDroplet(6, 114, WD::CYAN);
  _tft.setTextSize(1);
  _tft.setTextColor(WD::WHITE);
  _tft.setTextWrap(false);
  _tft.setCursor(16, 118);
  _tft.print(humidity);
  _tft.print('%');

  _iconWind(66, 115, C_LGRAY);
  char wStr[12];
  snprintf(wStr, sizeof(wStr), "%.1fkm/h", windKmh);
  _tft.setCursor(80, 118);
  _tft.print(wStr);

  _tft.drawFastHLine(0, 130, 128, C_DGRAY);

  char timeStr[20] = "Updated --:-- UTC";
  time_t now = time(nullptr);
  if (now > 1000000000UL) {
    struct tm* ti = gmtime(&now);
    snprintf(timeStr, sizeof(timeStr), "Updated %02d:%02d UTC",
             ti->tm_hour, ti->tm_min);
  }
  showStatus(timeStr, C_DGRAY);
}

// ── Private helpers ───────────────────────────────────────────

uint16_t WeatherDisplay::_tempColour(float t) {
  if (t < 0)   return WD::BLUE;
  if (t < 10)  return WD::CYAN;
  if (t < 20)  return WD::WHITE;
  if (t < 30)  return WD::ORANGE;
  return WD::RED;
}

void WeatherDisplay::_centred(const char* str, int16_t y, uint16_t col, uint8_t sz) {
  _tft.setTextSize(sz);
  _tft.setTextColor(col);
  _tft.setTextWrap(false);
  int16_t x1, y1; uint16_t w, h;
  _tft.getTextBounds(str, 0, 0, &x1, &y1, &w, &h);
  _tft.setCursor((128 - (int16_t)w) / 2, y);
  _tft.print(str);
}

// Gradient header bar (y 0-21): lighter blue at top fading to deep navy.
void WeatherDisplay::_header() {
  for (int16_t y = 0; y < 22; y++) {
    uint8_t g = (uint8_t)(8 - y * 6 / 21);
    uint8_t b = (uint8_t)(26 - y * 14 / 21);
    _tft.drawFastHLine(0, y, 128, ((uint16_t)g << 5) | b);
  }
  char hdr[28];
  snprintf(hdr, sizeof(hdr), "%s Weather", _city);
  _centred(hdr, 7, WD::WHITE, 1);
}

// Temperature with a drawn degree circle (no font glyph needed).
// Layout (128 px wide, text size 3 = 18 px/char):
//   [digits × 18px] [2px gap] [circle 6px] [2px gap] [C 18px]
void WeatherDisplay::_drawTemp(float t) {
  uint16_t col = _tempColour(t);
  char num[8];
  snprintf(num, sizeof(num), "%.1f", t);

  int16_t numW  = (int16_t)strlen(num) * 18;
  int16_t total = numW + 2 + 6 + 2 + 18;
  int16_t x     = (128 - total) / 2;

  _tft.setTextSize(3);
  _tft.setTextColor(col);
  _tft.setTextWrap(false);
  _tft.setCursor(x, 72);
  _tft.print(num);
  _tft.drawCircle(x + numW + 5, 72 + 3, 3, col);
  _tft.setCursor(x + numW + 12, 72);
  _tft.print('C');
}

// ── Weather icons ─────────────────────────────────────────────

void WeatherDisplay::_iconSun(int16_t cx, int16_t cy) {
  _tft.fillCircle(cx, cy, 5, WD::YELLOW);
  for (int i = 0; i < 8; i++)
    _tft.drawLine(cx + SUN_IX[i], cy + SUN_IY[i],
                  cx + SUN_OX[i], cy + SUN_OY[i], WD::YELLOW);
}

void WeatherDisplay::_iconCloud(int16_t cx, int16_t cy, uint16_t col) {
  _tft.fillCircle(cx - 5, cy,     6, col);
  _tft.fillCircle(cx + 4, cy - 2, 8, col);
  _tft.fillRect(cx - 11, cy, 22, 7, col);
}

void WeatherDisplay::_iconSmallSun(int16_t cx, int16_t cy) {
  _tft.fillCircle(cx, cy, 4, WD::YELLOW);
  _tft.drawFastHLine(cx - 7, cy,     5, WD::YELLOW);
  _tft.drawFastHLine(cx + 3, cy,     4, WD::YELLOW);
  _tft.drawFastVLine(cx,     cy - 7, 4, WD::YELLOW);
  _tft.drawFastVLine(cx,     cy + 3, 4, WD::YELLOW);
}

void WeatherDisplay::_iconPartlyCloudy(int16_t cx, int16_t cy) {
  _iconSmallSun(cx - 4, cy - 4);
  _iconCloud(cx + 2, cy + 2, C_LGRAY);
}

void WeatherDisplay::_iconRain(int16_t cx, int16_t cy) {
  _iconCloud(cx, cy - 4, C_LGRAY);
  _tft.drawFastVLine(cx - 6, cy + 6, 5, WD::CYAN);
  _tft.drawFastVLine(cx,     cy + 8, 5, WD::CYAN);
  _tft.drawFastVLine(cx + 6, cy + 6, 5, WD::CYAN);
}

void WeatherDisplay::_iconSnow(int16_t cx, int16_t cy) {
  _iconCloud(cx, cy - 4, C_LGRAY);
  _tft.fillCircle(cx - 6, cy + 9, 2, WD::WHITE);
  _tft.fillCircle(cx,     cy + 9, 2, WD::WHITE);
  _tft.fillCircle(cx + 6, cy + 9, 2, WD::WHITE);
}

void WeatherDisplay::_iconStorm(int16_t cx, int16_t cy) {
  _iconCloud(cx, cy - 4, C_DGRAY);
  _tft.fillTriangle(cx + 1, cy + 4,  cx + 6, cy + 4,  cx + 2, cy + 10, WD::YELLOW);
  _tft.fillTriangle(cx - 2, cy + 9,  cx + 3, cy + 9,  cx - 1, cy + 15, WD::YELLOW);
}

void WeatherDisplay::_iconFog(int16_t cx, int16_t cy) {
  _tft.drawFastHLine(cx - 10, cy - 6,  20, C_LGRAY);
  _tft.drawFastHLine(cx - 12, cy - 2,  24, C_LGRAY);
  _tft.drawFastHLine(cx - 10, cy + 2,  20, C_LGRAY);
  _tft.drawFastHLine(cx -  8, cy + 6,  16, C_LGRAY);
  _tft.drawFastHLine(cx - 10, cy + 10, 20, C_LGRAY);
}

void WeatherDisplay::_conditionIcon(int16_t cx, int16_t cy, const char* cond) {
  String s = cond; s.toLowerCase();
  if      (s.indexOf("thunder") >= 0 || s.indexOf("storm")   >= 0) _iconStorm(cx, cy);
  else if (s.indexOf("snow")    >= 0 || s.indexOf("sleet")   >= 0) _iconSnow(cx, cy);
  else if (s.indexOf("rain")    >= 0 || s.indexOf("drizzle") >= 0 ||
           s.indexOf("shower")  >= 0)                               _iconRain(cx, cy);
  else if (s.indexOf("fog")     >= 0 || s.indexOf("mist")    >= 0 ||
           s.indexOf("haze")    >= 0)                               _iconFog(cx, cy);
  else if (s.indexOf("partly")  >= 0 || s.indexOf("few")     >= 0) _iconPartlyCloudy(cx, cy);
  else if (s.indexOf("cloud")   >= 0 || s.indexOf("overcast")>= 0) _iconCloud(cx, cy, C_LGRAY);
  else                                                              _iconSun(cx, cy);
}

// Water droplet (7 × 9 px), top-left corner at (x, y).
void WeatherDisplay::_iconDroplet(int16_t x, int16_t y, uint16_t col) {
  _tft.fillCircle(x + 3, y + 6, 3, col);
  _tft.fillTriangle(x, y + 6, x + 6, y + 6, x + 3, y, col);
}

// Three horizontal wind lines (11 × 7 px), top-left at (x, y).
void WeatherDisplay::_iconWind(int16_t x, int16_t y, uint16_t col) {
  _tft.drawFastHLine(x,     y,     11, col);
  _tft.drawFastHLine(x + 2, y + 3,  9, col);
  _tft.drawFastHLine(x,     y + 6, 11, col);
}
