/*
 * ESP8266 Web Scraper — minimal example using Zyte API
 * =====================================================
 * Target : https://books.toscrape.com (a dedicated scraping practice site)
 * Output : Book title, price, rating — printed to the serial monitor
 *
 * WHY ZYTE API?
 *   Many sites block requests that come from cloud/datacenter IPs or that
 *   lack browser-like headers. The ESP8266 hits both conditions. Zyte API
 *   routes the request through a residential proxy and spoofs browser headers
 *   transparently, making the scrape succeed without any change to this code.
 *
 * WHY STREAM-DECODE INSTEAD OF BUFFERING?
 *   Zyte returns the page HTML base64-encoded inside a JSON field. The full
 *   response can be 3-4× the ESP8266's free heap (~22 KB). This code never
 *   buffers the full response — it decodes the base64 stream one byte at a
 *   time, stops the moment it has captured the data it needs, and drops the
 *   rest. Peak extra RAM: ~620 bytes (the capture buffer).
 *
 * HOW TO USE
 *   1. Fill in your WiFi credentials and Zyte API key below.
 *   2. Build:  pio run -e d1_mini_example
 *   3. Flash:  pio run -e d1_mini_example --target upload
 *   4. Monitor (115200 baud): pio device monitor
 */

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecure.h>
#include <base64.h>

// ╔══════════════════════════════════════════════════════════════╗
// ║                     USER CONFIGURATION                      ║
// ╚══════════════════════════════════════════════════════════════╝

#define WIFI_SSID    "YOUR_WIFI_SSID"
#define WIFI_PASS    "YOUR_WIFI_PASSWORD"
#define ZYTE_API_KEY "YOUR_ZYTE_API_KEY"

// The page to scrape — any books.toscrape.com product URL works.
#define TARGET_URL   "https://books.toscrape.com/catalogue/a-light-in-the-attic_1000/index.html"

// Bytes to capture once the <h1> anchor is found in the decoded stream.
// Title, price, availability, and rating all appear within ~450 bytes of <h1>.
#define CAPTURE_LEN  600

#define REFRESH_MS   10000   // re-scrape every 10 seconds

// ══════════════════════════════════════════════════════════════════

static const char ZYTE_URL[]  = "https://api.zyte.com/v1/extract";
static const char ZYTE_BODY[] =
  "{\"url\":\"" TARGET_URL "\","
  "\"httpResponseBody\":true,"
  "\"customHttpRequestHeaders\":"
    "[{\"name\":\"Accept-Encoding\",\"value\":\"identity\"}]}";

// ── Base64 streaming decoder ──────────────────────────────────
// The ESP8266 Arduino core ships base64::encode only; we decode manually.

static const char B64T[] =
  "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

struct B64State { int val = 0, bits = -8; };

// Feed one raw character from the JSON stream into the accumulator.
// Returns: decoded byte (0–255), -1 to skip (padding/whitespace), -2 for '"' (field end).
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

// Scan the SSL byte stream for a literal marker string.
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

// Extract the substring between two literal markers.
static String between(const String& src, const char* before, const char* after) {
  int s = src.indexOf(before);
  if (s < 0) return "";
  s += strlen(before);
  int e = src.indexOf(after, s);
  if (e < 0) return "";
  return src.substring(s, e);
}

// ── WiFi ──────────────────────────────────────────────────────

void connectWiFi() {
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

// ── Scrape ────────────────────────────────────────────────────

void scrape() {
  Serial.println("Fetching via Zyte API...");

  BearSSL::WiFiClientSecure client;
  client.setInsecure();   // skip TLS cert verification (acceptable for this demo)

  HTTPClient http;
  http.setTimeout(30000);
  http.useHTTP10(true);   // forces Content-Length instead of chunked transfer —
                          // required for reliable streaming reads on ESP8266
  http.begin(client, ZYTE_URL);
  http.addHeader("Authorization",
                 "Basic " + base64::encode(String(ZYTE_API_KEY) + ":"));
  http.addHeader("Content-Type",    "application/json");
  http.addHeader("Accept-Encoding", "identity");

  int code = http.POST((const uint8_t*)ZYTE_BODY, strlen(ZYTE_BODY));
  if (code != 200) {
    Serial.printf("[Zyte] HTTP %d\n", code);
    if (code > 0) Serial.println(http.getString());
    http.end();
    return;
  }

  WiFiClient* stream   = http.getStreamPtr();
  unsigned long deadline = millis() + 30000;

  // ── Step 1: skip past the JSON wrapper to the base64 field ─────
  if (!streamFind(stream, "\"httpResponseBody\":\"", deadline)) {
    Serial.println("[Error] httpResponseBody not found in response");
    http.end();
    return;
  }

  // ── Step 2: decode base64 byte-by-byte, searching for <h1> ─────
  // The book title is the first <h1> on the page; price and rating follow.
  const char ANCHOR[] = "<h1>";
  const int  ALEN     = sizeof(ANCHOR) - 1;
  B64State   st;
  int        anchorMatch = 0;

  while (anchorMatch < ALEN && millis() < deadline) {
    if (!stream->available()) { delay(1); continue; }
    int r = b64Char((char)stream->read(), st);
    if (r == -2) {
      Serial.println("[Error] stream ended before <h1> anchor");
      http.end();
      return;
    }
    if (r < 0) continue;
    char c = (char)r;
    anchorMatch = (c == ANCHOR[anchorMatch]) ? anchorMatch + 1
                : (c == ANCHOR[0])           ? 1 : 0;
  }

  // ── Step 3: capture CAPTURE_LEN decoded bytes into a stack buffer ─
  char buf[CAPTURE_LEN + 1];
  memcpy(buf, ANCHOR, ALEN);   // seed buffer with the anchor we just consumed
  int bufLen = ALEN;

  while (bufLen < CAPTURE_LEN && millis() < deadline) {
    if (!stream->available()) { delay(1); continue; }
    int r = b64Char((char)stream->read(), st);
    if (r == -2) break;
    if (r >= 0) buf[bufLen++] = (char)r;
  }
  buf[bufLen] = '\0';
  http.end();   // drop the rest of the page — we have what we need

  // ── Step 4: parse the 600-byte HTML snippet ─────────────────────
  // HTML around our anchor looks like:
  //   <h1>A Light in the Attic</h1>
  //   <p class="price_color">£51.77</p>
  //   <p class="instock availability">...</p>
  //   <p class="star-rating Three"></p>

  String html(buf);
  String title  = between(html, "<h1>",           "</h1>");
  String price  = between(html, "price_color\">", "</p>");
  String rating = between(html, "star-rating ",   "\"");
  String avail  = html.indexOf("In stock") >= 0 ? "In stock" : "Out of stock";

  if (title.isEmpty()) {
    Serial.println("[Error] parse failed — HTML structure may have changed");
    return;
  }

  // Trim any stray whitespace the page may include
  title.trim();
  price.trim();
  rating.trim();

  Serial.println(F("--- books.toscrape.com ---"));
  Serial.println("  Title        : " + title);
  Serial.println("  Price        : " + price);
  Serial.println("  Rating       : " + rating + " stars");
  Serial.println("  Availability : " + avail);
  Serial.println();
}

// ── Entry points ──────────────────────────────────────────────

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println(F("\nESP8266 Web Scraper — books.toscrape.com via Zyte API"));
  connectWiFi();
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
    return;
  }
  scrape();
  delay(REFRESH_MS);
}
