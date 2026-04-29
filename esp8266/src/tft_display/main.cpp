#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecure.h>
#include <base64.h>
#include <time.h>
#include <WeatherDisplay.h>

// ╔══════════════════════════════════════════════════════════════╗
// ║                     USER CONFIGURATION                      ║
// ╚══════════════════════════════════════════════════════════════╝

#define WIFI_SSID       "YOUR_WIFI_SSID"
#define WIFI_PASS       "YOUR_WIFI_PASSWORD"

#define ZYTE_API_KEY    "YOUR_ZYTE_API_KEY"

#define CITY_NAME       "London"
#define TIMEANDDATE_URL "https://www.timeanddate.com/weather/uk/london"

#define CAPTURE_LEN     800
#define REFRESH_MS      600000   // 10 minutes

// ── TFT wiring (Wemos D1 Mini) ────────────────────────────────
//   Display │ D1 Mini pin
//   ────────┼──────────────────────────────
//   GND     │ GND
//   VCC     │ 3.3V
//   SCL     │ D5  (GPIO14)  hardware SPI clock
//   SDA     │ D7  (GPIO13)  hardware SPI MOSI
//   RES     │ D1  (GPIO5)
//   DC      │ D2  (GPIO4)
//   CS      │ D8  (GPIO15)
//   BL      │ 3.3V          backlight always on
//
// Note: if colours look wrong after flash, pass INITR_REDTAB or
// INITR_GREENTAB to display.begin() — tab colour varies by seller.

#define TFT_CS   15   // D8  GPIO15
#define TFT_DC    4   // D2  GPIO4
#define TFT_RST   5   // D1  GPIO5

// ══════════════════════════════════════════════════════════════════

static const char ZYTE_URL[]  = "https://api.zyte.com/v1/extract";
static const char ZYTE_BODY[] =
  "{\"url\":\"" TIMEANDDATE_URL "\","
  "\"httpResponseBody\":true,"
  "\"customHttpRequestHeaders\":"
    "[{\"name\":\"Accept-Encoding\",\"value\":\"identity\"}]}";

WeatherDisplay display(TFT_CS, TFT_DC, TFT_RST, CITY_NAME);

// ── Base64 streaming decoder ──────────────────────────────────
// ESP8266 core only ships base64::encode; we decode manually.

static const char B64T[] =
  "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

struct B64State { int val = 0, bits = -8; };

// Returns decoded byte (0-255), -1 for padding/whitespace, -2 for '"' (end of field).
static int b64Char(char c, B64State& st) {
  if (c == '"') return -2;
  const char* p = strchr(B64T, c);
  if (!p) return -1;
  st.val = (st.val << 6) + (int)(p - B64T);
  st.bits += 6;
  if (st.bits >= 0) {
    int byte = (st.val >> st.bits) & 0xFF;
    st.bits -= 8;
    return byte;
  }
  return -1;
}

static bool streamFind(WiFiClient* s, const char* marker, unsigned long deadline) {
  int mlen = strlen(marker), match = 0;
  while (millis() < deadline) {
    if (!s->available()) { delay(1); continue; }
    char c = (char)s->read();
    match = (c == marker[match]) ? match + 1 : (c == marker[0] ? 1 : 0);
    if (match == mlen) return true;
  }
  return false;
}

// ── String helpers ────────────────────────────────────────────

static String between(const String& src, const char* before, const char* after) {
  int s = src.indexOf(before);
  if (s < 0) return "";
  s += strlen(before);
  int e = src.indexOf(after, s);
  if (e < 0) return "";
  return src.substring(s, e);
}

static float toC(float f)   { return (f - 32.0f) * 5.0f / 9.0f; }
static float toKmh(float m) { return m * 1.60934f; }

// ── WiFi / NTP ────────────────────────────────────────────────

void connectWiFi() {
  display.showConnecting(WIFI_SSID);
  WiFi.persistent(false);
  WiFi.disconnect(true);
  delay(100);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.printf("Connecting to \"%s\"", WIFI_SSID);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - start > 20000) {
      Serial.printf("\nFailed (status %d) — check SSID/password/2.4GHz\n",
                    WiFi.status());
      display.showStatus("WiFi failed!", WD::RED);
      return;
    }
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nConnected. IP: " + WiFi.localIP().toString());
  display.showStatus("WiFi OK", WD::GREEN);
}

void syncNTP() {
  display.showStatus("NTP sync...", WD::CYAN);
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  Serial.print("NTP sync");
  unsigned long start = millis();
  while (time(nullptr) < 1000000000UL && millis() - start < 20000) {
    delay(500);
    Serial.print(".");
  }
  bool ok = time(nullptr) > 1000000000UL;
  Serial.println(ok ? " OK" : " failed");
  display.showStatus(ok ? "NTP OK" : "NTP failed", ok ? WD::GREEN : WD::RED);
  delay(500);
}

// ── Main fetch ────────────────────────────────────────────────

void fetchWeather() {
  display.showStatus("Fetching...", WD::CYAN);

  BearSSL::WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setTimeout(30000);
  http.useHTTP10(true);
  http.begin(client, ZYTE_URL);

  String creds = String(ZYTE_API_KEY) + ":";
  http.addHeader("Authorization", "Basic " + base64::encode(creds));
  http.addHeader("Content-Type",    "application/json");
  http.addHeader("Accept-Encoding", "identity");

  int code = http.POST((const uint8_t*)ZYTE_BODY, strlen(ZYTE_BODY));
  if (code != 200) {
    Serial.printf("[Zyte] HTTP %d\n", code);
    char err[14]; snprintf(err, sizeof(err), "HTTP %d", code);
    display.showStatus(err, WD::RED);
    if (code > 0) Serial.println(http.getString());
    http.end();
    return;
  }

  // Stream-decode strategy: scan the JSON stream for the base64 field,
  // decode byte-by-byte, anchor-search in decoded output, capture 800 bytes.
  // Peak extra heap: ~810 bytes.  See ARCHITECTURE.md for full explanation.

  WiFiClient* stream = http.getStreamPtr();
  unsigned long deadline = millis() + 30000;

  if (!streamFind(stream, "\"httpResponseBody\":\"", deadline)) {
    Serial.println("[Zyte] httpResponseBody field not found");
    display.showStatus("No body field", WD::RED);
    http.end();
    return;
  }

  const char ANCHOR[] = "class=h2>";
  const int  ALEN     = sizeof(ANCHOR) - 1;
  B64State st;
  int anchorMatch = 0;

  while (anchorMatch < ALEN && millis() < deadline) {
    if (!stream->available()) { delay(1); continue; }
    int r = b64Char((char)stream->read(), st);
    if (r == -2) {
      Serial.println("[Zyte] stream ended before anchor");
      display.showStatus("No anchor", WD::RED);
      http.end();
      return;
    }
    if (r < 0) continue;
    char c = (char)r;
    anchorMatch = (c == ANCHOR[anchorMatch]) ? anchorMatch + 1
                : (c == ANCHOR[0])           ? 1 : 0;
  }

  char buf[CAPTURE_LEN + 1];
  memcpy(buf, ANCHOR, ALEN);
  int bufLen = ALEN;

  while (bufLen < CAPTURE_LEN && millis() < deadline) {
    if (!stream->available()) { delay(1); continue; }
    int r = b64Char((char)stream->read(), st);
    if (r == -2) break;
    if (r >= 0) buf[bufLen++] = (char)r;
  }
  buf[bufLen] = '\0';
  http.end();

  String html(buf);
  String tempF_s  = between(html, "class=h2>",           "&nbsp;");
  String condStr  = between(html, "</div><p>",            "</p>");
  String feelsF_s = between(html, "Feels Like: ",         "&nbsp;");
  String windS    = between(html, "Wind: ",               " mph");
  String humidS   = between(html, "Humidity: </th><td>",  "%");

  if (tempF_s.isEmpty() || condStr.isEmpty()) {
    Serial.println("[Parse] weather fields not found — anchor may need recalibration");
    display.showStatus("Parse error", WD::RED);
    return;
  }

  float tempC    = toC(tempF_s.toFloat());
  float feelsC   = feelsF_s.isEmpty() ? tempC : toC(feelsF_s.toFloat());
  float windKmh  = toKmh(windS.toFloat());
  int   humidity = humidS.toInt();

  condStr.replace(".", "");
  condStr.trim();

  char date[11] = "---";
  time_t now = time(nullptr);
  if (now > 1000000000UL) {
    struct tm* ti = gmtime(&now);
    strftime(date, sizeof(date), "%Y-%m-%d", ti);
  }

  Serial.println(F("-----------------------------"));
  Serial.printf( "  %s Weather | %s\n",                   CITY_NAME, date);
  Serial.println(F("-----------------------------"));
  Serial.printf( "  Condition : %s\n",                    condStr.c_str());
  Serial.printf( "  Temp      : %.1f C (feels %.1f C)\n", tempC, feelsC);
  Serial.printf( "  Humidity  : %d %%\n",                 humidity);
  Serial.printf( "  Wind      : %.1f km/h\n",             windKmh);
  Serial.println();

  display.drawWeather(tempC, feelsC, humidity, windKmh, condStr.c_str(), date);
}

// ── Entry points ──────────────────────────────────────────────

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println(F("\nESP8266 Weather Monitor — ST7735 TFT display"));
  display.begin();
  connectWiFi();
  if (WiFi.status() == WL_CONNECTED) syncNTP();
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println(F("WiFi lost, reconnecting..."));
    connectWiFi();
    if (WiFi.status() == WL_CONNECTED) syncNTP();
  }
  fetchWeather();
  delay(REFRESH_MS);
}
