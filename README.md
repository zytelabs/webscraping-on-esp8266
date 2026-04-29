# ESP8266 Weather Monitor via Zyte API

> **Web scraping on an ESP8266/ESP32? Yes, it's possible — with [Zyte API](https://docs.zyte.com/zyte-api/get-started.html).**

This project fetches live weather data from [timeanddate.com](https://www.timeanddate.com/weather/) on a **Wemos D1 Mini (ESP8266)** using the Zyte API as a scraping proxy, then displays it either on a 128×160 ST7735 TFT screen or the serial monitor — all within the ESP8266's ~22 KB of free RAM.

![ESP8266 weather monitor on a breadboard with ST7735 TFT display showing London weather](assets/demo.jpg)

```
-----------------------------
  London Weather | 2026-04-29
-----------------------------
  Condition : Sunny
  Temp      : 17.8 C (feels 17.8 C)
  Humidity  : 37 %
  Wind      : 20.9 km/h
```

---

## Start here — minimal example

**New to the project?** Flash [`src/scraper_example/`](src/scraper_example/main.cpp) first. It scrapes [books.toscrape.com](https://books.toscrape.com) (a public scraping practice site), fits in ~160 lines, and has step-by-step comments that explain exactly how the Zyte API call and stream-decode work. No display hardware needed.

```
--- books.toscrape.com ---
  Title        : A Light in the Attic
  Price        : £51.77
  Rating       : Three stars
  Availability : In stock
```

Once you understand the example, the weather monitor variants extend the same pattern with real-world parsing and optional display output.

---

## Why Zyte API?

timeanddate.com blocks requests from data-centre IP ranges and non-browser `User-Agent` strings with a `403 Forbidden`. The ESP8266 hits both conditions simultaneously.

[Zyte API](https://docs.zyte.com/zyte-api/get-started.html) routes the request through a residential proxy pool and transparently spoofs browser headers, making the scrape succeed. The response body is returned as a base64-encoded string inside a JSON envelope — which this firmware stream-decodes on the fly without ever buffering the full response in RAM (see [Architecture](#architecture)).

---

## Variants

| Variant | Source | Hardware needed | Build env | Default refresh |
|---|---|---|---|---|
| **Minimal example** | `src/scraper_example/` | D1 Mini only | `d1_mini_example` | 10 s |
| **Serial Monitor** | `src/serial_monitor/` | D1 Mini only | `d1_mini_serial` | 3 s |
| **TFT Display** | `src/tft_display/` | D1 Mini + ST7735 | `d1_mini_tft` | 10 min |

All three use identical networking and stream-decode logic. Start with the example, then move to whichever weather variant suits your hardware.

---

## Hardware

### Both variants
- [Wemos D1 Mini](https://www.wemos.cc/en/latest/d1/d1_mini.html) (ESP8266, 4 MB flash)
- USB cable for programming

### TFT variant only
- 1.8" ST7735 SPI TFT display, 128×160 px (widely available, ~$3)

---

## Wiring (TFT variant)

| Display pin | D1 Mini pin | GPIO | Notes |
|---|---|---|---|
| GND | GND | — | |
| VCC | 3.3V | — | Do **not** use 5V |
| SCL | D5 | GPIO14 | Hardware SPI clock |
| SDA | D7 | GPIO13 | Hardware SPI MOSI |
| RES | D1 | GPIO5 | Reset |
| DC | D2 | GPIO4 | Data/Command select |
| CS | D8 | GPIO15 | Chip select |
| BL | 3.3V | — | Backlight always on |

> If colours look wrong after flashing, try changing `tft.initR(INITR_BLACKTAB)` to `INITR_REDTAB` or `INITR_GREENTAB` in `setup()` — the tab colour varies by display seller.

---

## Prerequisites

1. **[PlatformIO](https://platformio.org/install/cli)** — `pip install platformio` or install the VS Code extension.
2. **Zyte API key** — [sign up at zyte.com](https://app.zyte.com/o/zyte-api/trial) (free trial available). Your key is a 32-character hex string, e.g. `a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4`.
3. A 2.4 GHz Wi-Fi network (ESP8266 does not support 5 GHz).

---

## Setup

### 1. Clone the repository

```bash
git clone https://github.com/zytelabs/webscraping-on-esp8266.git
cd webscraping-on-esp8266
```

### 2. Set your credentials

Open the source file for the variant you want to use and fill in the three required values at the top of the `USER CONFIGURATION` block:

**Minimal example** → [src/scraper_example/main.cpp](src/scraper_example/main.cpp)  
**Serial variant** → [src/serial_monitor/main.cpp](src/serial_monitor/main.cpp)  
**TFT variant** → [src/tft_display/main.cpp](src/tft_display/main.cpp)

```cpp
#define WIFI_SSID    "YOUR_WIFI_SSID"       // ← your 2.4 GHz network name
#define WIFI_PASS    "YOUR_WIFI_PASSWORD"   // ← your Wi-Fi password
#define ZYTE_API_KEY "YOUR_ZYTE_API_KEY"    // ← your Zyte API key
```

### 3. Set the serial port

Open [platformio.ini](platformio.ini) and update `upload_port` and `monitor_port` to match your system. All three environments use the same port values:

```ini
; macOS / Linux — find yours with: ls /dev/tty.*  or  ls /dev/ttyUSB*
upload_port = /dev/tty.usbserial-XXXX
monitor_port = /dev/tty.usbserial-XXXX

; Windows — check Device Manager → Ports (COM & LPT)
; upload_port = COM3
; monitor_port = COM3
```

### 4. (Optional) Change the city

See [Changing the city](#changing-the-city) below.

### 5. Build and flash

```bash
# Recommended first step — minimal scraper example
pio run -e d1_mini_example --target upload

# Serial Monitor weather variant (no display hardware needed)
pio run -e d1_mini_serial --target upload

# TFT Display weather variant
pio run -e d1_mini_tft --target upload
```

### 6. Monitor serial output

```bash
pio device monitor
```

For the example and serial variants, the device connects to Wi-Fi and immediately starts printing scraped data. The TFT variant also syncs NTP and renders the graphical UI.

---

## Configuration Reference

All tuneable constants live at the top of each `main.cpp` in the `USER CONFIGURATION` block:

| Constant | Default | Description |
|---|---|---|
| `WIFI_SSID` | `"YOUR_WIFI_SSID"` | 2.4 GHz Wi-Fi network name |
| `WIFI_PASS` | `"YOUR_WIFI_PASSWORD"` | Wi-Fi password |
| `ZYTE_API_KEY` | `"YOUR_ZYTE_API_KEY"` | Zyte API key |
| `CITY_NAME` | `"London"` | City label for display / serial output |
| `TIMEANDDATE_URL` | `https://…/weather/uk/london` | timeanddate.com weather page URL |
| `CAPTURE_LEN` | `800` | Bytes captured after the HTML anchor (rarely needs changing) |
| `REFRESH_MS` | `600000` (TFT) / `3000` (serial) | Fetch interval in milliseconds |
| `TFT_CS` | `15` | TFT Chip Select GPIO (TFT variant only) |
| `TFT_DC` | `4` | TFT Data/Command GPIO (TFT variant only) |
| `TFT_RST` | `5` | TFT Reset GPIO (TFT variant only) |

---

## Changing the City

1. Find the timeanddate.com weather URL for your city, e.g.:
   - London → `https://www.timeanddate.com/weather/uk/london`
   - New York → `https://www.timeanddate.com/weather/usa/new-york`
   - Tokyo → `https://www.timeanddate.com/weather/japan/tokyo`

2. Update both constants in `main.cpp`:

```cpp
#define CITY_NAME       "Tokyo"
#define TIMEANDDATE_URL "https://www.timeanddate.com/weather/japan/tokyo"
```

The HTML parsing anchors (`class=h2>`, `Feels Like:`, etc.) are consistent across all city pages, so no other changes are needed.

---

## Build / Flash / Debug

```bash
# Build without flashing
pio run -e d1_mini_serial
pio run -e d1_mini_tft

# Flash (if the serial monitor is open and holding the port, kill it first)
lsof /dev/tty.usbserial-XXXX 2>/dev/null | awk 'NR>1{print $2}' | xargs kill 2>/dev/null
pio run -e d1_mini_serial --target upload

# Open serial monitor (Ctrl+C to exit)
pio device monitor

# Clean build artifacts
pio run --target clean
```

**Finding your serial port:**
```bash
# macOS
ls /dev/tty.*

# Linux
ls /dev/ttyUSB* /dev/ttyACM*

# Windows — check Device Manager → Ports (COM & LPT)
```

---

## Architecture

The Zyte API returns the scraped HTML base64-encoded inside a JSON field (`httpResponseBody`). The full response is ~62 KB — about 3× the ESP8266's free heap. The firmware never buffers it all:

1. `HTTPClient` opens an HTTPS connection via `BearSSL::WiFiClientSecure`.
2. `http.useHTTP10(true)` forces `Content-Length` (not chunked transfer), required for reliable streaming on ESP8266.
3. `streamFind()` scans the raw byte stream for the literal `"httpResponseBody":"` prefix.
4. `b64Char()` + `B64State` decode the base64 stream **one character at a time**, simultaneously KMP-matching the HTML anchor `class=h2>` in the decoded output.
5. Once the anchor is matched, exactly `CAPTURE_LEN` (800) decoded bytes are captured into a stack buffer. All five weather fields fit within this window.
6. `http.end()` closes the connection — the remaining ~46 KB of the page is discarded unread.

**Peak extra heap usage: ~810 bytes.**

For the full design notes and error history see [ARCHITECTURE.md](ARCHITECTURE.md).

---

## Troubleshooting

| Symptom | Likely cause | Fix |
|---|---|---|
| `[Zyte] HTTP 401` | Invalid API key | Check `ZYTE_API_KEY` |
| `[Zyte] HTTP 422` | Malformed request body | Verify `TIMEANDDATE_URL` is a valid timeanddate.com URL |
| `WiFi failed!` | Wrong SSID/password or 5 GHz band | Double-check credentials; ensure network is 2.4 GHz |
| `NTP failed` | NTP timeout (hotspot / firewall) | Increase timeout in `syncNTP()` or try a different network |
| `[Parse] weather fields not found` | timeanddate.com changed their HTML | Increase `CAPTURE_LEN` or update the `ANCHOR` string in `fetchWeather()` |
| Port busy when flashing | `pio device monitor` holds the tty | `lsof /dev/tty.usbserial-XXX \| awk 'NR>1{print $2}' \| xargs kill` |
| Wrong colours on TFT | Tab colour variant | Change `INITR_BLACKTAB` → `INITR_REDTAB` or `INITR_GREENTAB` in `setup()` |

---

## Dependencies & Citations

| Library / Service | Purpose | Link |
|---|---|---|
| **Zyte API** | Residential proxy + browser-header spoofing for scraping | [docs.zyte.com](https://docs.zyte.com/zyte-api/get-started.html) |
| **timeanddate.com** | Source of weather data | [timeanddate.com/weather](https://www.timeanddate.com/weather/) |
| **Adafruit GFX Library** | Graphics primitives for TFT variant | [github.com/adafruit/Adafruit-GFX-Library](https://github.com/adafruit/Adafruit-GFX-Library) |
| **Adafruit ST7735 Library** | ST7735/ST7789 TFT driver | [github.com/adafruit/Adafruit-ST7735-Library](https://github.com/adafruit/Adafruit-ST7735-Library) |
| **ESP8266 Arduino Core** | `ESP8266WiFi`, `ESP8266HTTPClient`, `WiFiClientSecure`, `base64` | [github.com/esp8266/Arduino](https://github.com/esp8266/Arduino) |
| **PlatformIO** | Build system and library manager | [platformio.org](https://platformio.org/) |

---

## License

[MIT](LICENSE) — free to use, modify, and distribute.
