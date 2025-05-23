# KeroTrack Display (formerly Aura)

This project is a fork of the original [Aura](https://github.com/Surrey-Homeware/Aura) which was made [Aura weather display](https://makerworld.com/en/models/1382304-aura-smart-weather-forecast-display), repurposed to act as a real-time oil tank monitor for the [KeroTrack](https://github.com/your-org/KeroTrack) system. Instead of showing weather data, this display now retrieves and visualizes oil tank data published to an MQTT broker by KeroTrack sensors and backend services.

## Project Overview

KeroTrack Display runs on ESP32-2432S028R ILI9341-based devices ("CYD" or Cheap Yellow Display, 2.8" screen). It connects to WiFi, subscribes to MQTT topics, and shows:
- Oil level (litres, % full, bars)
- Temperature
- Days remaining (estimation)
- Cost to fill, litres to order, and other tank stats

The UI is built with [lvgl](https://lvgl.io/) and [TFT_eSPI](https://github.com/Bodmer/TFT_eSPI), and is optimized for touch and low-power operation.

It was coded using Cursor and I have no prior experience using C or compiling/deploying to CYD/ESP32 devices.

## How to Compile & Flash

1. **Configure Arduino IDE:**
    - Board: "ESP32 Dev Module"
    - Tools → Partition Scheme: "Huge App (3MB No OTA/1MB SPIFFS)"
2. **Install Required Libraries:**
    - ArduinoJson 7.4.1
    - HttpClient 2.2.0
    - TFT_eSPI 2.5.43_
    - WifiManager 2.0.17
    - XPT2046_Touchscreen 1.4
    - lvgl 9.2.2
    - PubSubClient (for MQTT)
3. **Source Code Placement:**
    - Place the source code folders in `~/Documents/Arduino/`.
    - Copy the included config files for `lvgl`, `TFT_eSPI` and `PubSubClient` into their respective library folders (see `lvgl/src/lv_conf.h`, `TFT_eSPI/User_Setup.h` and `PubSubClient/src/PubSubClient.h`).
4. **Build and Upload:**
    - Open `KeroDisplay/kerodisplay.ino` in Arduino IDE and upload to your ESP32 device.

## Configuration

- On first boot, the device will start and you'll need to connect to a wifi network called `KeroTrack` and open a browser to http://192.168.4.1.  From there you can configure your wifi network to connect to and your MQTT broker settings.
- After configuration, it will connect to the specified MQTT broker and subscribe to KeroTrack topics (e.g., `oiltank/level`, `oiltank/analysis`).

## Features

- **Real-time Oil Tank Monitoring:**
  - Displays live oil tank data from MQTT topics published by KeroTrack, including litres remaining, percentage full, temperature, litres to order, cost to fill, and more.
  - Visual bar indicator with color gradient (green/yellow/red) to show oil level and highlight low levels.

- **Touchscreen User Interface:**
  - Two main screens:
    - **Tank Status:** Litres remaining, percentage full, temperature, days left, and oil level bar.
    - **Order/Cost Info:** Litres to order, price per litre (PPL), and estimated cost to fill.
  - Tap anywhere to switch between screens (with debounce).
  - Auto-switches between screens every 2 minutes.

- **Backlight Scheduling & Power Management:**
  - Automatically dims the display between 23:00 and 06:00 (based on NTP time).
  - Touching the screen during dim hours temporarily wakes the backlight.
  - Always-on fallback if NTP time is not available.

- **WiFi & MQTT Setup via Captive Portal:**
  - On first boot (or if WiFi is not configured), launches a WiFiManager captive portal for easy WiFi and MQTT configuration.
  - MQTT credentials and broker address are stored in device preferences.

- **Robust MQTT Integration:**
  - Subscribes to both oil tank level and analysis topics.
  - Supports wildcard topic subscription for debugging.
  - Updates the UI instantly when new MQTT data arrives.

- **Persistent Preferences:**
  - Remembers WiFi, MQTT, and brightness settings across reboots.

- **Visual Alerts & Robustness:**
  - Color-coded bar indicator for oil level.
  - Handles WiFi and MQTT connection failures gracefully.
  - Displays setup and status screens for user feedback.

## Credits & Thanks

- Forked from [Aura](https://github.com/Surrey-Homeware/Aura)
- Uses the 3d printed case for the CYD on [Makerworld](https://makerworld.com/en/models/1382304-aura-smart-weather-forecast-display)
- [lvgl](https://lvgl.io/) for UI
- [TFT_eSPI](https://github.com/Bodmer/TFT_eSPI) for display driver
- [WifiManager](https://github.com/tzapu/WiFiManager) for easy setup
- [XPT2046_Touchscreen](https://github.com/PaulStoffregen/XPT2046_Touchscreen)
- [PubSubClient](https://github.com/knolleary/pubsubclient) for MQTT
- [KeroTrack](https://github.com/MrSiJo/KeroTrack) for backend and data

## License

The code is available under the GPL 3.0 license. See original Aura repo for icon and asset licenses.
