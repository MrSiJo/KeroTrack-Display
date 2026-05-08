# KeroTrack Display

A four-screen oil-tank dashboard for the ESP32-2432S028R "CYD" (Cheap Yellow
Display, 2.8" 240×320 ILI9341 with resistive touch). Subscribes to a
[KeroTrack v2](https://github.com/MrSiJo/KeroTrack) MQTT broker and surfaces
tank level, consumption, cost, and current weather in a touch-cycling UI.

Originally forked from [Aura](https://github.com/Surrey-Homeware/Aura) — the
weather screen reuses Aura's icon set and Latin Montserrat font files (see
`KeroDisplay/kerodisplay/WEATHER_ASSETS.md` for attribution).

## Screens

The display cycles through four screens with an asymmetric auto-rotate
(60s on screen 1, 30s on the others). Tap anywhere to advance and reset the
hold timer for the new screen.

| # | Screen | Hero | Supporting data |
|---|--------|------|-----------------|
| 1 | At-a-glance | `73%` (colour-coded green/amber/red) | litres remaining, level bar, "Empty 23 Aug 2026 / 47 days" |
| 2 | Consumption | `285 L` used since refill | days since refill, hot-water vs heating split bar with daily L breakdown, avg L/day burn, "↑ refill detected today" flag |
| 3 | Cost | `243` avg monthly (gold tint, `(GBP)` cue) | weekly + annual cost, cost-to-fill cell with 500L order-ready threshold (green tint + pip when ≥ 500L), current p/L |
| 4 | Weather | `12°` current temperature with condition icon | feels-like, condition descriptor, location, 3-day forecast strip with icons + day + high/low |

Bottom dot indicator (`*...`, `.*..`, `..*.`, `...*`) shows which screen is
active. Order: 1 → 2 → 3 → 4 → 1.

## Status bar (every screen)

A 36px status bar sits across the top of every screen:

- WiFi icon (dims when disconnected)
- MQTT state dot (green = connected, amber = reconnecting, red = down)
- NTP clock `HH:MM` in centre (blank until first sync; auto-applies UK BST/GMT)
- Last-update age on the right (`2m ago`, `42m ago`, `3h ago`)
  - Default light blue, turns amber after 10 min stale, red after 60 min

## Alerts

- **Leak detected:** when MQTT publishes `leak_detected: "y"`, a red ribbon
  with a soft pulsing background appears below the status bar on every
  screen. Content shifts down 28px while active.
- **Refill detected:** subtle `↑ refill detected today` line on the
  consumption screen when `refill_detected: "y"`.

## Hardware

- **Board:** ESP32-2432S028R "CYD" (240×320 portrait ILI9341, XPT2046 touch)
- **Partition scheme:** Huge App (3MB No OTA/1MB SPIFFS) — required, the
  sketch with all weather assets does not fit on the default partition

## Building and flashing

This is an Arduino IDE project. There is no CLI build system; `.ino`
compilation goes through the IDE.

1. **Arduino IDE setup:**
   - Board: "ESP32 Dev Module"
   - Tools → Partition Scheme: **"Huge App (3MB No OTA/1MB SPIFFS)"**
   - Tools → Port: your CYD's COM port

2. **Required libraries** (Arduino Library Manager):
   - ArduinoJson 7.4.1
   - HttpClient 2.2.0
   - TFT_eSPI 2.5.43
   - WiFiManager 2.0.17
   - XPT2046_Touchscreen 1.4
   - lvgl 9.2.2
   - PubSubClient

3. **Library config override (important):**
   The `lvgl/`, `TFT_eSPI/`, and `PubSubClient/` folders at the repo root are
   **patched header files**, not vendored library copies. After installing
   the libraries through Library Manager, copy these into the user's
   installed Arduino libraries:

   ```powershell
   Copy-Item lvgl\src\lv_conf.h "$HOME\Documents\Arduino\libraries\lvgl\src\lv_conf.h" -Force
   Copy-Item TFT_eSPI\User_Setup.h "$HOME\Documents\Arduino\libraries\TFT_eSPI\User_Setup.h" -Force
   Copy-Item PubSubClient\src\PubSubClient.h "$HOME\Documents\Arduino\libraries\PubSubClient\src\PubSubClient.h" -Force
   ```

   The CYD-specific TFT pin mapping and the LVGL feature configuration
   (image widget, fonts, etc.) live in those headers. Edits to them only
   take effect on the device after the file is copied into the Arduino
   libraries folder.

4. **Open and upload:**
   `KeroDisplay/kerodisplay/kerodisplay.ino` → Sketch → Upload.

   Watch the boot log on Serial Monitor at 115200 baud.

## First-time setup (captive portal)

On first boot (or if the saved WiFi credentials are wrong), the device
launches a WiFiManager captive portal:

1. Connect from a phone to the open WiFi network **`KeroTrack`**.
2. Browser opens to `http://192.168.4.1` (or scan for a captive-portal
   notification).
3. Pick your home WiFi and enter MQTT broker / port / user / password.
4. Save. The device prompts you to power-cycle, then boots into normal
   mode.

## Settings web page (after first setup)

Once on WiFi, the device runs an always-on settings server. Visit
`http://<device-ip>/` from any browser on the LAN — the IP is printed on
serial at boot, e.g.:

```
Settings page: http://172.16.0.130
```

The settings page has two sections:

- **MQTT** — change broker, port, username, password. Submitting reboots.
- **Weather** — type a place name (city/town/postcode) and click "Search
  and change". The device hits the
  [Open-Meteo geocoding API](https://open-meteo.com/en/docs/geocoding-api)
  and presents the matches as radio buttons. Pick one, click "Save and
  reboot". The chosen name + lat/lon are persisted to NVS.
  - Leave the field blank and search to disable the weather screen
    (screen 4 will show "Weather not configured").

## MQTT topics consumed

Subscribed by the device. All published by the
[KeroTrack v2](https://github.com/MrSiJo/KeroTrack) backend.

| Topic | Used for |
|-------|----------|
| `oiltank/level` | percentage, litres, leak/refill flags, cost-to-fill, ppl, litres-to-order, current temperature |
| `oiltank/analysis` | empty date, days remaining, days since refill, total used since refill, avg daily consumption, hot-water vs heating breakdown |
| `oiltank/cost_analysis` | avg daily/weekly/monthly/annual cost, latest refill amount/cost/ppl |

A non-blocking reconnect helper runs in `loop()` with exponential backoff
(1s → 2s → 5s → 15s → 30s → 60s, capped) so transient broker outages don't
freeze the UI.

## Weather (screen 4)

Powered by [Open-Meteo](https://open-meteo.com) — no API key needed.
A FreeRTOS task wakes every 15 minutes, fetches current + 3-day forecast,
and uses `lv_async_call` to refresh the UI on the LVGL thread.

Saved location format: `Surrey, GB` (city + ISO country code, kept short
to fit the screen). Long names are dot-truncated. The 26-icon set covers
all WMO weather codes; icons swap between day/night variants based on the
API's `is_day` flag.

## Project layout

```
KeroTrack-Display/
├── KeroDisplay/
│   └── kerodisplay/
│       ├── kerodisplay.ino           # entire firmware (single-file sketch)
│       ├── icon_*.c                  # 26 weather icons (LVGL bitmap arrays)
│       ├── lv_font_montserrat_latin_*.c  # Latin-1 font variants (5 sizes)
│       └── WEATHER_ASSETS.md         # attribution for icons/fonts
├── lvgl/
│   └── src/lv_conf.h                 # patched LVGL config (fonts, image widget)
├── TFT_eSPI/
│   └── User_Setup.h                  # patched TFT pin mapping for CYD
├── PubSubClient/
│   └── src/PubSubClient.h            # patched MQTT buffer size
├── docs/
│   └── superpowers/                  # design specs and implementation plans
└── README.md / CLAUDE.md
```

## Architecture notes

- The sketch is structured around three event sources sharing global
  state: an LVGL UI loop, an MQTT callback, and a FreeRTOS weather task.
- Screen widgets are pre-built once during `setup()` with cached pointers
  in per-screen structs (`s1`, `s2`, `s3`, `s4`) — no fragile child-index
  lookups in update paths.
- Reusable `create_chrome()` helper builds the status bar + hidden leak
  ribbon + content container + dot indicator on every screen.
- `update_oiltank_ui()` runs on every received MQTT message and dispatches
  to per-screen update functions; the weather task feeds `update_screen4()`
  separately via `lv_async_call`.

## Credits

- [Aura](https://github.com/Surrey-Homeware/Aura) — original parent project,
  source of the weather icons and Latin Montserrat fonts. Aura's
  3D-printed case from
  [MakerWorld](https://makerworld.com/en/models/1382304-aura-smart-weather-forecast-display)
  fits this project unchanged.
- [Google Weather Icons](https://github.com/mrdarrengriffin/google-weather-icons)
  — origin of the icon artwork (used with attribution).
- [Open-Meteo](https://open-meteo.com) — weather + geocoding APIs.
- [LVGL](https://lvgl.io/), [TFT_eSPI](https://github.com/Bodmer/TFT_eSPI),
  [WiFiManager](https://github.com/tzapu/WiFiManager),
  [PubSubClient](https://github.com/knolleary/pubsubclient),
  [ArduinoJson](https://arduinojson.org/),
  [XPT2046_Touchscreen](https://github.com/PaulStoffregen/XPT2046_Touchscreen).
- [KeroTrack](https://github.com/MrSiJo/KeroTrack) — the backend that
  feeds this display.

## License

GPL-3.0 (inherited from Aura, the parent project). Note that the weather
icons in `KeroDisplay/kerodisplay/icon_*.c` originate from Google and are
not GPL-licensed — see `WEATHER_ASSETS.md` for details.
