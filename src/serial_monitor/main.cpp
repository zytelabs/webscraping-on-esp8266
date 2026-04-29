#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecure.h>
#include <base64.h>
#include <time.h>

// ╔══════════════════════════════════════════════════════════════╗
// ║                     USER CONFIGURATION                      ║
// ╚══════════════════════════════════════════════════════════════╝

#define WIFI_SSID       "YOUR_WIFI_SSID"
#define WIFI_PASS       "YOUR_WIFI_PASSWORD"

#define ZYTE_API_KEY    "YOUR_ZYTE_API_KEY"

// City label shown on serial output
#define CITY_NAME       "London"

// timeanddate.com weather page for the target city
#define TIMEANDDATE_URL "https://www.timeanddate.com/weather/uk/london"

// Bytes to capture once the weather anchor is found in the decoded stream.
// All five weather fields fit within 800 bytes of the anchor point.
#define CAPTURE_LEN     800

// Fetch interval in milliseconds
#define REFRESH_MS      3000

// ══════════════════════════════════════════════════════════════════

static const char ZYTE_URL[] = "https://api.zyte.com/v1/extract";

// timeanddate.com blocks data-centre IPs and non-browser User-Agents.
// Zyte's residential proxy and header spoofing handle both transparently.
// Accept-Encoding:identity keeps the response as plain HTML (not gzip).
static const char ZYTE_BODY[] =
  "{\"url\":\"" TIMEANDDATE_URL "\","
  "\"httpResponseBody\":true,"
  "\"customHttpRequestHeaders\":"
    "[{\"name\":\"Accept-Encoding\",\"value\":\"identity\"}]}";

// ── Base64 streaming decoder ──────────────────────────────────────

// ESP8266 core only ships base64::encode; we decode manually.
static const char B64T[] =
  "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

struct B64State { int val = 0, bits = -8; };

// Feed one base64 character into the accumulator.
// Returns a decoded byte (0-255), -1 for padding/whitespace, or -2 for '"'
// (end of the JSON string field).
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

// Scan the SSL stream byte-by-byte for a literal marker string.
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

// ── Helpers ───────────────────────────────────────────────────────

// Extract the substring between two literal markers in src.
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

// ── WiFi / NTP ────────────────────────────────────────────────────

void connectWiFi() {
  // persistent(false) + disconnect(true) clear any stale saved AP
  // that could block a fresh connection attempt.
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
      return;
    }
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nConnected. IP: " + WiFi.localIP().toString());
}

// NTP gives us the current date; the ESP8266 has no on-board RTC.
void syncNTP() {
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  Serial.print("NTP sync");
  unsigned long start = millis();
  while (time(nullptr) < 1000000000UL && millis() - start < 20000) {
    delay(500);
    Serial.print(".");
  }
  Serial.println(time(nullptr) > 1000000000UL ? " OK" : " failed");
}

// ── Main fetch ────────────────────────────────────────────────────

void fetchWeather() {
  BearSSL::WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setTimeout(30000);
  http.useHTTP10(true);   // Content-Length instead of chunked — more reliable on ESP8266
  http.begin(client, ZYTE_URL);

  String creds = String(ZYTE_API_KEY) + ":";
  http.addHeader("Authorization", "Basic " + base64::encode(creds));
  http.addHeader("Content-Type",    "application/json");
  http.addHeader("Accept-Encoding", "identity");

  int code = http.POST((const uint8_t*)ZYTE_BODY, strlen(ZYTE_BODY));
  if (code != 200) {
    Serial.printf("[Zyte] HTTP %d\n", code);
    if (code > 0) Serial.println(http.getString());
    http.end();
    return;
  }

  // ── Stream-decode the Zyte response ────────────────────────────
  // The full Zyte response is ~62 KB (base64 of 46 KB HTML + JSON wrapper) —
  // too large to buffer on the ESP8266 heap.
  //
  // Strategy:
  //   1. Scan the raw JSON stream for "httpResponseBody":".
  //   2. Base64-decode byte-by-byte, searching the decoded output for the
  //      anchor "class=h2>" which opens the current-conditions widget.
  //   3. Once the anchor is found, capture CAPTURE_LEN bytes into a small
  //      stack buffer — all five weather fields live within this window.
  //   4. Close the connection; the rest of the 46 KB page is discarded.
  //
  // Searching for a decoded pattern (not a fixed byte offset) makes this
  // robust to minor page-size variations across requests.
  //
  // Peak extra RAM: ~810 bytes (capture buffer + anchor copy).

  WiFiClient* stream = http.getStreamPtr();
  unsigned long deadline = millis() + 30000;

  if (!streamFind(stream, "\"httpResponseBody\":\"", deadline)) {
    Serial.println("[Zyte] httpResponseBody field not found");
    http.end();
    return;
  }

  // Search decoded byte stream for the anchor that opens the weather widget.
  const char ANCHOR[]  = "class=h2>";
  const int  ALEN      = sizeof(ANCHOR) - 1;
  B64State st;
  int anchorMatch = 0;

  while (anchorMatch < ALEN && millis() < deadline) {
    if (!stream->available()) { delay(1); continue; }
    int r = b64Char((char)stream->read(), st);
    if (r == -2) { Serial.println("[Zyte] stream ended before anchor"); http.end(); return; }
    if (r < 0) continue;
    char c = (char)r;
    anchorMatch = (c == ANCHOR[anchorMatch]) ? anchorMatch + 1
                : (c == ANCHOR[0])           ? 1 : 0;
  }

  // Seed the capture buffer with the anchor we just consumed so that
  // the "class=h2>" pattern is available to the parser.
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
  http.end();   // done — drop the rest of the page

  // ── Parse the 800-byte capture buffer ──────────────────────────
  String html(buf);   // buf is already null-terminated above

  // Temperature and condition sit inside:
  //   <div class=h2>64&nbsp;°F</div><p>Sunny.</p>
  String tempF_s  = between(html, "class=h2>",           "&nbsp;");
  String condStr  = between(html, "</div><p>",            "</p>");
  String feelsF_s = between(html, "Feels Like: ",         "&nbsp;");
  String windS    = between(html, "Wind: ",               " mph");
  String humidS   = between(html, "Humidity: </th><td>",  "%");

  if (tempF_s.isEmpty() || condStr.isEmpty()) {
    Serial.println("[Parse] weather fields not found — anchor may need recalibration");
    return;
  }

  // Convert imperial → metric and strip trailing punctuation from condition
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
  Serial.printf( "  %s Weather | %s\n",                    CITY_NAME, date);
  Serial.println(F("-----------------------------"));
  Serial.printf( "  Condition : %s\n",                     condStr.c_str());
  Serial.printf( "  Temp      : %.1f C (feels %.1f C)\n",  tempC, feelsC);
  Serial.printf( "  Humidity  : %d %%\n",                  humidity);
  Serial.printf( "  Wind      : %.1f km/h\n",              windKmh);
  Serial.println();
}

// ── Entry points ──────────────────────────────────────────────────

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println(F("\nESP8266 Weather Monitor — scraping timeanddate.com via Zyte API"));
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
