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

The Zyte response wraps ~46 KB of HTML as base64 inside a JSON body — totalling ~62 KB. That is 3× the free heap. The code **never buffers the full response**:

1. `HTTPClient` opens an SSL connection via `BearSSL::WiFiClientSecure` (cert validation skipped with `setInsecure()`).
2. `http.useHTTP10(true)` forces `Content-Length` instead of chunked transfer encoding — required for reliable reads on ESP8266.
3. `streamFind()` scans the raw SSL byte stream for the literal string `"httpResponseBody":"`.
4. `b64Char()` + `B64State` decode the base64 stream one character at a time while KMP-matching the HTML anchor `class=h2>` in the decoded output.
5. Once the anchor is matched, 800 bytes are captured into a **stack buffer** (`char buf[801]`). All five weather fields fit within this window.
6. `http.end()` closes the connection — the remaining ~46 KB of page is discarded unread.

**Peak extra heap: ~810 bytes.**

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

## Build & flash commands

```bash
# Build serial variant
pio run -e d1_mini_serial

# Build TFT variant
pio run -e d1_mini_tft

# Kill serial monitor holding the port, then flash
lsof /dev/tty.usbserial-XXXX 2>/dev/null | awk 'NR>1{print $2}' | xargs kill 2>/dev/null
pio run -e d1_mini_serial --target upload

# Monitor
pio device monitor
```

## Key errors encountered and fixes

| Error | Root cause | Fix |
|---|---|---|
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
