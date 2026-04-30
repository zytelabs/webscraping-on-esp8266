# ESP8266 Weather Monitor — Project Context

## Hardware

| Field | Value |
|---|---|
| Board | Wemos D1 Mini (ESP-12F) |
| Flash | 4 MB |
| RAM | ~81 920 bytes total, ~22 KB free at runtime |
| On-board LED | GPIO2 (`LED_BUILTIN`), active LOW |
| Serial port | `/dev/tty.usbserial-XXXX` @ 115200 baud (macOS) or `/dev/ttyUSB0` (Linux) |
| Toolchain | PlatformIO CLI (`pio`) |

## Project layout

```
esp8266/
├── README.md
├── ARCHITECTURE.md
├── LICENSE
├── platformio.ini
├── src/
│   ├── scraper_example/main.cpp   ← minimal Zyte API example
│   ├── serial_monitor/main.cpp    ← weather, serial output
│   └── tft_display/main.cpp       ← weather, ST7735 TFT display
└── lib/
    └── WeatherDisplay/            ← display library (TFT variant)
```

## What it does

Fetches live weather for London from `timeanddate.com/weather/uk/london` via the **Zyte API** and renders it on either a 128×160 ST7735 TFT display (10-minute refresh) or the serial monitor (3-second refresh):

```
-----------------------------
  London Weather | 2026-04-23
-----------------------------
  Condition : Sunny
  Temp      : 17.8 C (feels 17.8 C)
  Humidity  : 37 %
  Wind      : 20.9 km/h
```

## Why Zyte is needed

timeanddate.com returns 403 to any data-centre IP or non-browser User-Agent. Zyte's residential proxy pool and transparent header spoofing are the only way to reach it from an ESP8266.

## Architecture: streaming decode (critical for RAM)

### The problem

The ESP8266 has ~22 KB of free heap at runtime. The Zyte API response is ~62 KB — a JSON envelope containing the 46 KB target HTML base64-encoded. That is nearly 3× available memory.

```cpp
String body = http.getString();  // requests ~62 KB heap allocation → crash
```

Any approach that buffers the full response is dead on arrival.

### The solution: one forward pass, 801 bytes max

The firmware processes the TCP stream as a pipe. At no point does it hold more than 801 bytes in memory.

```
Zyte TCP stream (~62 KB)
│
├── {"httpResponseBody":"           (1) streamFind() scans forward,
│                                       discards every byte as it goes
│
├── PGh0bWw+...base64...            (2) b64Char() decodes one char at a time
│   │                                   each decoded byte is checked against
│   │                                   the anchor, then immediately discarded
│   │
│   ├── <html><head>...<nav>...
│   │   (~7 KB decoded, discarded)
│   │
│   ├── <div class=h2>              (3) anchor matched — open the capture window
│   │
│   ├── 64°F</div><p>Sunny...       (4) next 800 decoded bytes go into buf[]
│   │   Feels Like: 61°F                this is the only heap allocation
│   │   Wind: 13 mph
│   │   Humidity: 45%
│   │
│   └── ...remaining ~39 KB...      (5) http.end() closes the TCP socket
│                                       these bytes are never read at all
└── "}
```

**Peak extra heap: ~810 bytes** (800-byte stack buffer + `B64State` accumulator).

### (1) Skipping the JSON wrapper — `streamFind()`

The function scans the raw SSL stream one byte at a time, matching a literal string using a single integer:

```cpp
match = (c == marker[match]) ? match + 1 : (c == marker[0] ? 1 : 0);
```

This is a minimal KMP-style matcher. It never allocates; the byte is tested and thrown away. Once `"httpResponseBody":"` is found, base64 decoding begins.

### (2) Base64 decoding — `b64Char()` + `B64State`

Base64 encodes 3 bytes as 4 ASCII characters (6 bits per character). The decoder accumulates bits in a single integer and emits one byte every time it has collected 8:

```cpp
struct B64State { int val = 0, bits = -8; };

static int b64Char(char c, B64State& st) {
    st.val = (st.val << 6) + index_of(c);   // shift in 6 new bits
    st.bits += 6;
    if (st.bits >= 0) {
        int byte = (st.val >> st.bits) & 0xFF;
        st.bits -= 8;
        return byte;                         // one decoded HTML byte
    }
    return -1;                               // accumulating — not yet
}
```

No intermediate buffer. No heap. The decoded byte is returned directly to the caller which either discards it or appends it to the capture buffer.

### (3) Anchor search — single-pass KMP in the decoded stream

The same KMP matcher runs on the decoded output while decoding is happening:

```cpp
anchorMatch = (c == ANCHOR[anchorMatch]) ? anchorMatch + 1
            : (c == ANCHOR[0])           ? 1 : 0;
```

The anchor `class=h2>` is the CSS class on timeanddate.com's current-conditions widget. It appears after ~7 KB of decoded HTML. Every decoded byte before it is checked, not matched, and discarded. No backtracking, no allocation — just one integer that resets on mismatch.

### Why an anchor instead of a fixed byte offset

A hardcoded `SKIP_BYTES = 7400` would also work, but page size varies slightly between requests (ad scripts, A/B tests, minor HTML changes). The anchor search finds the weather widget regardless, making the firmware resilient without any code changes.

### (4) Capture window — stack allocation

Once the anchor matches, a 801-byte buffer is opened on the stack:

```cpp
char buf[CAPTURE_LEN + 1];       // 801 bytes — never touches the heap
memcpy(buf, ANCHOR, ALEN);       // seed with the anchor we just consumed
```

The next 800 decoded bytes fill it. All five weather fields (temperature, condition, feels-like, wind, humidity) appear within this window.

### (5) Early close — dropping the tail

```cpp
http.end();   // closes TCP connection
```

The remaining ~39 KB of HTML (tables, scripts, footer) is never read from the socket. The OS discards the buffered TCP data. This is what keeps the entire fetch within budget.

## Zyte API call

```
POST https://api.zyte.com/v1/extract
Authorization: Basic <base64(API_KEY + ":")>
Content-Type: application/json

{
  "url": "https://www.timeanddate.com/weather/uk/london",
  "httpResponseBody": true,
  "customHttpRequestHeaders": [
    {"name": "Accept-Encoding", "value": "identity"}
  ]
}
```

`Accept-Encoding: identity` is forwarded to the target to prevent gzip, keeping the response body as plain HTML. The `httpResponseBody` field in the JSON response is a base64 string of the raw HTML.

## HTML patterns parsed

Matched with `String::indexOf` / `substring` — no HTML parser, no ArduinoJson:

| Field | Before marker | After marker |
|---|---|---|
| Temperature | `class=h2>` | `&nbsp;` |
| Condition | `</div><p>` | `</p>` |
| Feels like | `Feels Like: ` | `&nbsp;` |
| Wind speed | `Wind: ` | ` mph` |
| Humidity | `Humidity: </th><td>` | `%` |

Units from timeanddate.com are imperial (°F, mph). Converted to °C and km/h in code.

## NTP

`configTime(0, 0, "pool.ntp.org", "time.nist.gov")` — 20-second timeout. The ESP8266 has no RTC; NTP is the only source of current date. Date printed as `YYYY-MM-DD` (UTC).

## WiFi connection pattern

```cpp
WiFi.persistent(false);   // don't save AP to flash
WiFi.disconnect(true);    // clear any stale saved AP
delay(100);
WiFi.mode(WIFI_STA);
WiFi.begin(SSID, PASS);
```

Without `persistent(false)` + `disconnect(true)`, stale flash-saved credentials block reconnection after a failed attempt.

## User-configurable constants (top of main.cpp)

| Constant | Default | Meaning |
|---|---|---|
| `WIFI_SSID` | `"YOUR_WIFI_SSID"` | AP name |
| `WIFI_PASS` | `"YOUR_WIFI_PASSWORD"` | AP password |
| `ZYTE_API_KEY` | `"YOUR_ZYTE_API_KEY"` | Zyte API key |
| `CITY_NAME` | `"London"` | Label in serial output / TFT header |
| `TIMEANDDATE_URL` | `.../weather/uk/london` | Target page |
| `CAPTURE_LEN` | `800` | Bytes captured after anchor |
| `REFRESH_MS` | `600000` (TFT) / `3000` (serial) | Fetch interval (ms) |

## First-time setup on a new machine

After cloning, a fresh PlatformIO install needs the platform and libraries before it can build.

```bash
# 1. Install the ESP8266 toolchain (~200 MB, once per machine)
#    Without this you get: Error: Unknown platform 'espressif8266'
pio platform install espressif8266

# 2. Install project libraries (Adafruit GFX, ST7735, etc.)
#    Without this you get: fatal error: Adafruit_GFX.h: No such file or directory
pio pkg install

# 3. Build to verify everything is wired up
pio run -e d1_mini_example
```

## Build & flash commands

```bash
# Build only (no upload)
pio run -e d1_mini_example
pio run -e d1_mini_serial
pio run -e d1_mini_tft

# Kill serial monitor holding the port, then flash
lsof /dev/tty.usbserial-XXXX 2>/dev/null | awk 'NR>1{print $2}' | xargs kill 2>/dev/null
pio run -e d1_mini_example --target upload

# Monitor (115200 baud, Ctrl+C to exit)
pio device monitor
```

## Key errors encountered and fixes

| Error | Root cause | Fix |
|---|---|---|
| `Error: Unknown platform 'espressif8266'` | Platform not installed on this machine | `pio platform install espressif8266` |
| `fatal error: Adafruit_GFX.h: No such file or directory` | Libraries not installed | `pio pkg install` |
| `base64::decode is not a member of 'base64'` | ESP8266 core only ships `encode` | Custom `b64Char()` streaming decoder |
| `[JSON] outer: InvalidInput` | BearSSL stream not ready when ArduinoJson reads it | Switched to `http.getString()` then to full streaming |
| `[JSON] outer: IncompleteInput` | Chunked transfer truncated by `getString()` | `http.useHTTP10(true)` |
| `[JSON] outer: NoMemory` | ArduinoJson copying 10 KB base64 into heap | Dropped ArduinoJson entirely; raw stream scan |
| `[Parse] weather fields not found` (intermittent) | Page size varies → fixed `SKIP_BYTES` offset drifts | Dynamic anchor search for `class=h2>` in decoded stream |
| WiFi not connecting | Stale AP saved to flash | `persistent(false)` + `disconnect(true)` |
| NTP sync failed | 10s timeout too short on hotspot | Increased to 20s |
| `String(char*, int)` error | ESP8266 `String` has no two-arg `char*` constructor | `String html(buf)` (null-terminated buffer) |
| Port blocked | Previous `pio device monitor` holds tty | `lsof ... | xargs kill` before flash |

## Library dependencies

No external `lib_deps` — all headers are part of the ESP8266 Arduino core:

- `ESP8266WiFi.h`
- `ESP8266HTTPClient.h`
- `WiFiClientSecure.h` (BearSSL)
- `base64.h` (encode only; decode is custom)
- `time.h`
