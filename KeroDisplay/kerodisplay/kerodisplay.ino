#include <Arduino.h>
#include <WiFiManager.h>
#include <ArduinoJson.h>
#include <lvgl.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <Preferences.h>
#include "esp_system.h"
#include <PubSubClient.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <time.h>

// MQTT settings
#define XPT2046_IRQ 36   // T_IRQ
#define XPT2046_MOSI 32  // T_DIN
#define XPT2046_MISO 39  // T_OUT
#define XPT2046_CLK 25   // T_CLK
#define XPT2046_CS 33    // T_CS
#define LCD_BACKLIGHT_PIN 21
#define SCREEN_WIDTH 240
#define SCREEN_HEIGHT 320
#define DRAW_BUF_SIZE (SCREEN_WIDTH * SCREEN_HEIGHT / 10 * (LV_COLOR_DEPTH / 8))
#define DEFAULT_CAPTIVE_SSID "KeroTrack"
static const uint32_t SCREEN1_HOLD_MS = 60000;
static const uint32_t SCREEN2_HOLD_MS = 30000;
static const uint32_t SCREEN3_HOLD_MS = 30000;
static const uint32_t SCREEN4_HOLD_MS = 30000;
#define MIN_ORDER_LITRES 500

SPIClass touchscreenSPI = SPIClass(VSPI);
XPT2046_Touchscreen touchscreen(XPT2046_CS, XPT2046_IRQ);
uint32_t draw_buf[DRAW_BUF_SIZE / 4];
int x, y, z;

// Preferences
static Preferences prefs;

// MQTT settings
static char mqtt_broker[64] = "";
static int mqtt_port = 1883;

// static char mqtt_topic[64] = "";
static char mqtt_user[32] = "";
static char mqtt_pass[32] = "";
static bool mqtt_is_configured = false;

WiFiClient espClient;
PubSubClient mqttClient(espClient);
WebServer settingsServer(80);

// Oil tank data structure
struct OilTankData {
    String date;
    int id;
    float temperature;
    float litres_remaining;
    float litres_used_since_last;
    float percentage_remaining;
    float oil_depth_cm;
    float air_gap_cm;
    float current_ppl;
    float cost_used;
    float cost_to_fill;
    int heating_degree_days;
    float seasonal_efficiency;
    String refill_detected;
    String leak_detected;
    int raw_flags;
    float litres_to_order;
    int bars_remaining;
};

OilTankData oilTankData;

struct OilTankCost {
    float avg_daily_cost = 0;
    float avg_weekly_cost = 0;
    float avg_monthly_cost = 0;
    float avg_annual_cost = 0;
    float latest_refill_amount = 0;
    float latest_refill_cost = 0;
    float latest_refill_ppl = 0;
};
OilTankCost oilTankCost;

// ----- Weather (Open-Meteo) -----
struct WeatherDay {
  int weather_code = 0;
  float temp_max = 0;
  float temp_min = 0;
};
struct WeatherData {
  bool valid = false;
  unsigned long fetched_ms = 0;
  float temp_now = 0;
  float temp_feels = 0;
  int code_now = 0;
  bool is_day = true;
  WeatherDay daily[3];
};
WeatherData weatherData;

// Location persisted in NVS "kerotank" namespace.
static char weather_location[64] = "";
static char weather_lat[16] = "";
static char weather_lon[16] = "";

// Geocoding result for the settings page (max 5 results shown).
struct GeocodeMatch {
  String name;
  String country;
  String admin1;
  String latitude;
  String longitude;
};

#define WEATHER_FETCH_INTERVAL_MS (15UL * 60UL * 1000UL)

// MQTT reconnect state
enum MqttState { MQTT_DOWN, MQTT_RECONNECTING, MQTT_UP };
static MqttState mqtt_state = MQTT_DOWN;
static unsigned long last_mqtt_attempt_ms = 0;
static unsigned long mqtt_backoff_ms = 1000;
static unsigned long last_mqtt_msg_ms = 0;  // wall-clock of last received message

// Weather icons (defined in icon_*.c files in the sketch directory).
LV_IMG_DECLARE(icon_blizzard);
LV_IMG_DECLARE(icon_blowing_snow);
LV_IMG_DECLARE(icon_clear_night);
LV_IMG_DECLARE(icon_cloudy);
LV_IMG_DECLARE(icon_drizzle);
LV_IMG_DECLARE(icon_flurries);
LV_IMG_DECLARE(icon_haze_fog_dust_smoke);
LV_IMG_DECLARE(icon_heavy_rain);
LV_IMG_DECLARE(icon_heavy_snow);
LV_IMG_DECLARE(icon_isolated_scattered_tstorms_day);
LV_IMG_DECLARE(icon_isolated_scattered_tstorms_night);
LV_IMG_DECLARE(icon_mostly_clear_night);
LV_IMG_DECLARE(icon_mostly_cloudy_day);
LV_IMG_DECLARE(icon_mostly_cloudy_night);
LV_IMG_DECLARE(icon_mostly_sunny);
LV_IMG_DECLARE(icon_partly_cloudy);
LV_IMG_DECLARE(icon_partly_cloudy_night);
LV_IMG_DECLARE(icon_scattered_showers_day);
LV_IMG_DECLARE(icon_scattered_showers_night);
LV_IMG_DECLARE(icon_showers_rain);
LV_IMG_DECLARE(icon_sleet_hail);
LV_IMG_DECLARE(icon_snow_showers_snow);
LV_IMG_DECLARE(icon_strong_tstorms);
LV_IMG_DECLARE(icon_sunny);
LV_IMG_DECLARE(icon_tornado);
LV_IMG_DECLARE(icon_wintry_mix_rain_snow);

// Latin Montserrat fonts (defined in lv_font_montserrat_latin_*.c, used for screen 4).
LV_FONT_DECLARE(lv_font_montserrat_latin_12);
LV_FONT_DECLARE(lv_font_montserrat_latin_14);
LV_FONT_DECLARE(lv_font_montserrat_latin_16);
LV_FONT_DECLARE(lv_font_montserrat_latin_20);
LV_FONT_DECLARE(lv_font_montserrat_latin_42);

typedef struct {
  lv_obj_t *icon_wifi;
  lv_obj_t *dot_mqtt;
  lv_obj_t *lbl_clock;
  lv_obj_t *lbl_age;
} status_bar_widgets_t;

typedef struct {
  status_bar_widgets_t sb;
  lv_obj_t *ribbon;        // leak ribbon container, hidden by default
  lv_obj_t *ribbon_label;  // text inside the ribbon
  lv_obj_t *content;       // container all screen content lives in (shifts when ribbon shown)
  lv_obj_t *dots;          // bottom screen-indicator dots
} screen_chrome_t;

typedef struct {
  screen_chrome_t chrome;
  lv_obj_t *lbl_pct;
  lv_obj_t *lbl_litres;
  lv_obj_t *bar_fill;       // an lv_bar inside content
  lv_obj_t *lbl_empty_date; // "Empty 23 Aug 2026 - 47 days"
} screen1_widgets_t;

typedef struct {
  screen_chrome_t chrome;
  lv_obj_t *lbl_used;
  lv_obj_t *lbl_over_days;
  lv_obj_t *bar_split_hw;
  lv_obj_t *bar_split_heat;
  lv_obj_t *lbl_hw_legend;
  lv_obj_t *lbl_heat_legend;
  lv_obj_t *lbl_avg_burn;
  lv_obj_t *lbl_refill_note;
} screen2_widgets_t;

typedef struct {
  screen_chrome_t chrome;
  lv_obj_t *lbl_avg_monthly;
  lv_obj_t *lbl_avg_weekly;
  lv_obj_t *lbl_avg_annual;
  lv_obj_t *cell_to_fill;
  lv_obj_t *lbl_cost_to_fill;
  lv_obj_t *lbl_to_fill_label;
  lv_obj_t *pip;
  lv_obj_t *lbl_ppl;
} screen3_widgets_t;

typedef struct {
  screen_chrome_t chrome;
  lv_obj_t *icon_hero;        // big weather icon (20x20 native, scaled 3x)
  lv_obj_t *lbl_temp;         // big current temp
  lv_obj_t *lbl_descriptor;   // "feels Y° · Light rain"
  lv_obj_t *lbl_location;     // location name
  lv_obj_t *daily_icon[3];    // 3-day forecast row icons (20x20 native)
  lv_obj_t *daily_day[3];     // "Today" / day-of-week labels
  lv_obj_t *daily_range[3];   // "14° / 8°" temp ranges
} screen4_widgets_t;

static screen1_widgets_t s1;
static screen2_widgets_t s2;
static screen3_widgets_t s3;
static screen4_widgets_t s4;

// Oil tank UI objects
static lv_obj_t *screen1 = nullptr;
static lv_obj_t *screen2 = nullptr;
static lv_obj_t *screen3 = nullptr;
static lv_obj_t *screen4 = nullptr;
static int current_screen = 1;
static lv_timer_t *auto_switch_timer = nullptr;

// Debounce for screen switching
static unsigned long last_screen_switch = 0;
const unsigned long screen_switch_debounce = 400; // ms

// Analysis data structure
struct OilTankAnalysis {
    float estimated_days_remaining = 0;
    String estimated_empty_date;                   // "YYYY-MM-DD HH:MM:SS"
    float total_consumption_since_refill = 0;
    int   days_since_refill = 0;
    float avg_daily_consumption_l = 0;
    float estimated_daily_hot_water_consumption_l = 0;
    float estimated_daily_heating_consumption_l = 0;
};
OilTankAnalysis oilTankAnalysis;

// Forward declarations
void create_screen1();
void create_screen2();
void create_screen3();
void create_screen4();
void switch_screen();
void auto_switch_cb(lv_timer_t *timer);
void show_boot_screen();
void update_oiltank_ui();
static void create_chrome(lv_obj_t *parent, screen_chrome_t *out, const char *dots_str);
static void handle_settings_root();
static void handle_settings_save();
static void handle_search_location();
static void handle_save_location();
static const lv_img_dsc_t* choose_icon(int wmo_code, bool is_day);
static const char* describe_weather(int wmo_code);
static int geocode_location(const char *query, GeocodeMatch *out, int max_results);
static bool fetch_weather();
static void weather_task(void *param);
static void weather_task_start();
static void update_screen4();

void touchscreen_read(lv_indev_t *indev, lv_indev_data_t *data) {
  if (touchscreen.tirqTouched() && touchscreen.touched()) {
    TS_Point p = touchscreen.getPoint();
    x = map(p.x, 200, 3700, 1, SCREEN_WIDTH);
    y = map(p.y, 240, 3800, 1, SCREEN_HEIGHT);
    z = p.z;
    data->state = LV_INDEV_STATE_PRESSED;
    data->point.x = x;
    data->point.y = y;
  } else {
    data->state = LV_INDEV_STATE_RELEASED;
  }
}

// Add WiFiManager custom parameters for MQTT
WiFiManagerParameter custom_mqtt_broker("broker", "MQTT Broker", mqtt_broker, sizeof(mqtt_broker));
WiFiManagerParameter custom_mqtt_port("port", "MQTT Port", "1883", 6);

// WiFiManagerParameter custom_mqtt_topic("topic", "MQTT Topic", mqtt_topic, sizeof(mqtt_topic));
WiFiManagerParameter custom_mqtt_user("user", "MQTT User", mqtt_user, sizeof(mqtt_user));
WiFiManagerParameter custom_mqtt_pass("pass", "MQTT Pass", mqtt_pass, sizeof(mqtt_pass));

void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println("Booting...");
  TFT_eSPI tft = TFT_eSPI();
  tft.init();
  pinMode(LCD_BACKLIGHT_PIN, OUTPUT);
  lv_init();
  Serial.println("Display and LVGL initialized");
  // Init touchscreen
  touchscreenSPI.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS);
  touchscreen.begin(touchscreenSPI);
  touchscreen.setRotation(0);
  Serial.println("Touchscreen initialized");
  lv_display_t *disp = lv_tft_espi_create(SCREEN_WIDTH, SCREEN_HEIGHT, draw_buf, sizeof(draw_buf));
  lv_indev_t *indev = lv_indev_create();
  lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(indev, touchscreen_read);
  Serial.println("LVGL display and input device created");
  // Load saved prefs
  prefs.begin("kerotank", false);
  uint32_t brightness = prefs.getUInt("brightness", 255);
  analogWrite(LCD_BACKLIGHT_PIN, brightness);
  Serial.println("Preferences loaded, brightness set");

  // Show splash screen
  show_boot_screen();
  delay(1500);

  // Wait for WiFi to connect after reboot
  WiFi.mode(WIFI_STA);
  WiFi.begin();
  Serial.print("Waiting for WiFi to connect");
  unsigned long startAttemptTime = millis();
  const unsigned long wifiTimeout = 10000; // 10 seconds
  while (WiFi.status() != WL_CONNECTED && millis() - startAttemptTime < wifiTimeout) {
    delay(100);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("Connected to WiFi. IP address: ");
    Serial.println(WiFi.localIP());
    // Settings web server (always available while WiFi is up)
    settingsServer.on("/", handle_settings_root);
    settingsServer.on("/save", HTTP_POST, handle_settings_save);
    settingsServer.on("/search_location", HTTP_POST, handle_search_location);
    settingsServer.on("/save_location", HTTP_POST, handle_save_location);
    settingsServer.begin();
    Serial.print("Settings page: http://"); Serial.println(WiFi.localIP());
    weather_task_start();
  } else {
    Serial.println("WiFi not connected, starting WiFiManager portal");
    show_setup_screen();
    WiFiManager wm;
    wm.addParameter(&custom_mqtt_broker);
    wm.addParameter(&custom_mqtt_port);
    
    // wm.addParameter(&custom_mqtt_topic);
    wm.addParameter(&custom_mqtt_user);
    wm.addParameter(&custom_mqtt_pass);
    bool wifiConnected = wm.autoConnect(DEFAULT_CAPTIVE_SSID);
    Serial.print("WiFiManager portal result: "); Serial.println(wifiConnected ? "connected" : "not connected");
    if (!wifiConnected) {
      show_setup_screen();
      Serial.println("WiFiManager failed, staying in setup mode");
      while (1) delay(100);
    }
    // Save MQTT config from portal
    strncpy(mqtt_broker, custom_mqtt_broker.getValue(), sizeof(mqtt_broker));
    mqtt_port = atoi(custom_mqtt_port.getValue());
    
    // strncpy(mqtt_topic, custom_mqtt_topic.getValue(), sizeof(mqtt_topic));
    strncpy(mqtt_user, custom_mqtt_user.getValue(), sizeof(mqtt_user));
    strncpy(mqtt_pass, custom_mqtt_pass.getValue(), sizeof(mqtt_pass));
    prefs.putString("mqtt_broker", mqtt_broker);
    prefs.putInt("mqtt_port", mqtt_port);
    
    // prefs.putString("mqtt_topic", mqtt_topic);
    prefs.putString("mqtt_user", mqtt_user);
    prefs.putString("mqtt_pass", mqtt_pass);
    prefs.end();
    Serial.println("MQTT config saved, setup complete. Prompting for power cycle.");
    show_setup_complete_screen();
  }

  Serial.println("WiFi connected, loading MQTT config from prefs");
  // Load MQTT config from preferences
  prefs.begin("kerotank", true);
  String broker = prefs.getString("mqtt_broker", "");
  mqtt_port = prefs.getInt("mqtt_port", 1883);
  String user = prefs.getString("mqtt_user", "");
  String pass = prefs.getString("mqtt_pass", "");
  String locName = prefs.getString("loc_name", "");
  String locLat = prefs.getString("loc_lat", "");
  String locLon = prefs.getString("loc_lon", "");
  prefs.end();
  strncpy(mqtt_broker, broker.c_str(), sizeof(mqtt_broker));
  strncpy(mqtt_user, user.c_str(), sizeof(mqtt_user));
  strncpy(mqtt_pass, pass.c_str(), sizeof(mqtt_pass));
  strncpy(weather_location, locName.c_str(), sizeof(weather_location) - 1);
  weather_location[sizeof(weather_location) - 1] = '\0';
  strncpy(weather_lat, locLat.c_str(), sizeof(weather_lat) - 1);
  weather_lat[sizeof(weather_lat) - 1] = '\0';
  strncpy(weather_lon, locLon.c_str(), sizeof(weather_lon) - 1);
  weather_lon[sizeof(weather_lon) - 1] = '\0';
  Serial.print("Weather location: '"); Serial.print(weather_location);
  Serial.print("' ("); Serial.print(weather_lat); Serial.print(","); Serial.print(weather_lon); Serial.println(")");
  mqtt_is_configured = strlen(mqtt_broker) > 0 && strlen(mqtt_user) > 0;
  Serial.print("MQTT config: broker="); Serial.print(mqtt_broker);
  Serial.print(", port="); Serial.print(mqtt_port);
  Serial.print(", user="); Serial.print(mqtt_user);
  Serial.println();

  // --- Connect to MQTT with loaded config ---
  if (mqtt_is_configured) {
    Serial.println("Setting up MQTT client...");
    mqttClient.setServer(mqtt_broker, mqtt_port);
    mqttClient.setCallback(mqtt_callback);
    unsigned long mqttAttemptTime = millis();
    const unsigned long mqttTimeout = 5000; // 5 seconds
    bool mqttConnected = false;
    while (!mqttClient.connected() && millis() - mqttAttemptTime < mqttTimeout) {
      Serial.println("Attempting MQTT connection...");
      if (mqttClient.connect("KeroTankClient", mqtt_user, mqtt_pass)) {
        Serial.println("MQTT connected, subscribing to topics");
        mqttClient.subscribe("oiltank/level");
        Serial.println("Subscribed to topic: oiltank/level");
        mqttClient.subscribe("oiltank/analysis");
        Serial.println("Subscribed to topic: oiltank/analysis");
        mqttClient.subscribe("oiltank/cost_analysis");
        Serial.println("Subscribed to topic: oiltank/cost_analysis");
        mqtt_state = MQTT_UP;
        mqtt_backoff_ms = 1000;
        mqttConnected = true;
      } else {
        Serial.println("MQTT connect failed, retrying...");
        delay(2000);
      }
    }
    if (!mqttConnected) {
      Serial.println("MQTT connection failed after timeout");
      delay(3000); // Show error for 3 seconds
    }
  }

  Serial.println("Creating oil tank UI...");
  create_screen1();
  create_screen2();
  create_screen3();
  create_screen4();
  lv_scr_load(screen1);
  current_screen = 1;
  // Set up auto-switch timer with screen-1 hold duration; load_screen() updates
  // the period on every advance so the timer matches the active screen.
  auto_switch_timer = lv_timer_create(auto_switch_cb, hold_for(1), NULL);

  // Tap anywhere on any screen advances to next and resets the timer.
  auto tap_cb = [](lv_event_t *e) {
    unsigned long now = millis();
    if (now - last_screen_switch > screen_switch_debounce) {
      switch_screen();
      last_screen_switch = now;
    }
  };
  lv_obj_add_event_cb(screen1, tap_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_add_event_cb(screen2, tap_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_add_event_cb(screen3, tap_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_add_event_cb(screen4, tap_cb, LV_EVENT_CLICKED, NULL);
  lv_timer_create(update_status_bars_cb, 1000, NULL);

  // NTP time sync
  configTzTime("GMT0BST,M3.5.0/1,M10.5.0", "0.uk.pool.ntp.org", "1.uk.pool.ntp.org");
  Serial.println("Waiting for NTP time sync...");
  time_t now = 0;
  int ntp_attempts = 0;
  while (now < 8 * 3600 * 2 && ntp_attempts < 20) { // wait up to ~20s
    delay(500);
    now = time(nullptr);
    ntp_attempts++;
  }
  if (now > 8 * 3600 * 2) {
    Serial.println("NTP time acquired.");
    Serial.print("NTP time: ");
    Serial.println(ctime(&now));
  } else {
    Serial.println("NTP time NOT acquired; clock will show blank in status bar.");
  }

  Serial.println("Setup complete");
}

static void mqtt_try_reconnect() {
  if (!mqtt_is_configured) return;
  if (mqttClient.connected()) {
    if (mqtt_state != MQTT_UP) {
      mqtt_state = MQTT_UP;
      mqtt_backoff_ms = 1000;
    }
    return;
  }
  mqtt_state = MQTT_RECONNECTING;
  unsigned long now = millis();
  if (now - last_mqtt_attempt_ms < mqtt_backoff_ms) return;
  last_mqtt_attempt_ms = now;
  Serial.print("MQTT reconnect attempt (backoff "); Serial.print(mqtt_backoff_ms); Serial.println("ms)");
  if (mqttClient.connect("KeroTankClient", mqtt_user, mqtt_pass)) {
    Serial.println("MQTT reconnected; resubscribing");
    mqttClient.subscribe("oiltank/level");
    mqttClient.subscribe("oiltank/analysis");
    mqttClient.subscribe("oiltank/cost_analysis");
    mqtt_state = MQTT_UP;
    mqtt_backoff_ms = 1000;
  } else {
    if      (mqtt_backoff_ms < 2000)  mqtt_backoff_ms = 2000;
    else if (mqtt_backoff_ms < 5000)  mqtt_backoff_ms = 5000;
    else if (mqtt_backoff_ms < 15000) mqtt_backoff_ms = 15000;
    else if (mqtt_backoff_ms < 30000) mqtt_backoff_ms = 30000;
    else                              mqtt_backoff_ms = 60000;
  }
}

// WMO weather code -> icon. is_day controls day/night variants where applicable.
static const lv_img_dsc_t* choose_icon(int code, bool is_day) {
  switch (code) {
    case 0:  return is_day ? &icon_sunny : &icon_clear_night;
    case 1:  return is_day ? &icon_mostly_sunny : &icon_mostly_clear_night;
    case 2:  return is_day ? &icon_partly_cloudy : &icon_partly_cloudy_night;
    case 3:  return &icon_cloudy;
    case 45: case 48: return &icon_haze_fog_dust_smoke;
    case 51: case 53: case 55: return &icon_drizzle;
    case 56: case 57: return &icon_sleet_hail;
    case 61: case 63: return &icon_showers_rain;
    case 65: return &icon_heavy_rain;
    case 66: case 67: return &icon_sleet_hail;
    case 71: case 73: return &icon_snow_showers_snow;
    case 75: return &icon_heavy_snow;
    case 77: return &icon_flurries;
    case 80: case 81: return is_day ? &icon_scattered_showers_day : &icon_scattered_showers_night;
    case 82: return &icon_heavy_rain;
    case 85: case 86: return &icon_snow_showers_snow;
    case 95: return is_day ? &icon_isolated_scattered_tstorms_day : &icon_isolated_scattered_tstorms_night;
    case 96: case 99: return &icon_strong_tstorms;
    default: return &icon_cloudy;
  }
}

static const char* describe_weather(int code) {
  switch (code) {
    case 0:  return "Clear";
    case 1:  return "Mostly clear";
    case 2:  return "Partly cloudy";
    case 3:  return "Overcast";
    case 45: case 48: return "Fog";
    case 51: return "Light drizzle";
    case 53: return "Drizzle";
    case 55: return "Heavy drizzle";
    case 56: case 57: return "Freezing drizzle";
    case 61: return "Light rain";
    case 63: return "Rain";
    case 65: return "Heavy rain";
    case 66: case 67: return "Freezing rain";
    case 71: return "Light snow";
    case 73: return "Snow";
    case 75: return "Heavy snow";
    case 77: return "Snow grains";
    case 80: return "Light showers";
    case 81: return "Showers";
    case 82: return "Heavy showers";
    case 85: return "Light snow showers";
    case 86: return "Snow showers";
    case 95: return "Thunderstorm";
    case 96: case 99: return "Severe thunder";
    default: return "Unknown";
  }
}

// URL-encode a string for query parameters. Only handles characters likely to
// appear in city names (spaces, common punctuation). Output written to dst,
// truncated if cap is too small.
static void url_encode(const char *s, char *dst, size_t cap) {
  size_t o = 0;
  for (const char *p = s; *p && o + 4 < cap; p++) {
    unsigned char c = (unsigned char)*p;
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      dst[o++] = c;
    } else if (c == ' ') {
      dst[o++] = '+';
    } else {
      static const char hex[] = "0123456789ABCDEF";
      dst[o++] = '%';
      dst[o++] = hex[c >> 4];
      dst[o++] = hex[c & 0xF];
    }
  }
  dst[o] = '\0';
}

// Geocode a place name to up to max_results lat/lon matches.
// Returns the number of matches, or -1 on error.
static int geocode_location(const char *query, GeocodeMatch *out, int max_results) {
  if (WiFi.status() != WL_CONNECTED) return -1;
  if (!query || !*query) return 0;

  char encoded[160];
  url_encode(query, encoded, sizeof(encoded));
  String url = String("https://geocoding-api.open-meteo.com/v1/search?count=")
             + max_results + "&name=" + encoded;

  HTTPClient http;
  http.setTimeout(8000);
  http.begin(url);
  int rc = http.GET();
  if (rc != HTTP_CODE_OK) {
    Serial.print("Geocode HTTP "); Serial.println(rc);
    http.end();
    return -1;
  }
  String body = http.getString();
  http.end();

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body);
  if (err) {
    Serial.print("Geocode JSON parse: "); Serial.println(err.c_str());
    return -1;
  }

  JsonArray results = doc["results"].as<JsonArray>();
  int count = 0;
  for (JsonVariant v : results) {
    if (count >= max_results) break;
    out[count].name = v["name"].as<String>();
    out[count].country = v["country"].as<String>();
    out[count].admin1 = v["admin1"].as<String>();
    char lat[16], lon[16];
    snprintf(lat, sizeof(lat), "%.4f", v["latitude"].as<float>());
    snprintf(lon, sizeof(lon), "%.4f", v["longitude"].as<float>());
    out[count].latitude = lat;
    out[count].longitude = lon;
    count++;
  }
  return count;
}

// Schedule a UI repaint of screen 4 on the LVGL thread. Safe to call from
// the weather task. The lambda fires on the LVGL timer thread.
static void schedule_screen4_update_cb(void *unused) {
  update_screen4();
}

// Fetch current + 3-day forecast from Open-Meteo. Returns true on success.
// Populates global weatherData and schedules a screen 4 refresh.
static bool fetch_weather() {
  if (WiFi.status() != WL_CONNECTED) return false;
  if (strlen(weather_lat) == 0 || strlen(weather_lon) == 0) return false;

  String url = String("http://api.open-meteo.com/v1/forecast?latitude=")
    + weather_lat + "&longitude=" + weather_lon
    + "&current=temperature_2m,apparent_temperature,is_day,weather_code"
    + "&daily=temperature_2m_min,temperature_2m_max,weather_code"
    + "&forecast_days=3&timezone=auto";

  HTTPClient http;
  http.setTimeout(10000);
  http.begin(url);
  int rc = http.GET();
  if (rc != HTTP_CODE_OK) {
    Serial.print("Weather fetch HTTP "); Serial.println(rc);
    http.end();
    return false;
  }
  String body = http.getString();
  http.end();

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body);
  if (err) {
    Serial.print("Weather JSON parse: "); Serial.println(err.c_str());
    return false;
  }

  weatherData.temp_now   = doc["current"]["temperature_2m"].as<float>();
  weatherData.temp_feels = doc["current"]["apparent_temperature"].as<float>();
  weatherData.code_now   = doc["current"]["weather_code"].as<int>();
  weatherData.is_day     = doc["current"]["is_day"].as<int>() != 0;
  for (int i = 0; i < 3; i++) {
    weatherData.daily[i].weather_code = doc["daily"]["weather_code"][i].as<int>();
    weatherData.daily[i].temp_max     = doc["daily"]["temperature_2m_max"][i].as<float>();
    weatherData.daily[i].temp_min     = doc["daily"]["temperature_2m_min"][i].as<float>();
  }
  weatherData.valid = true;
  weatherData.fetched_ms = millis();
  Serial.println("Weather fetched OK");

  lv_async_call(schedule_screen4_update_cb, nullptr);
  return true;
}

// Background task: fetch weather every WEATHER_FETCH_INTERVAL_MS, with a 5s
// initial delay so WiFi has time to fully come up after boot.
static void weather_task(void *param) {
  vTaskDelay(pdMS_TO_TICKS(5000));
  for (;;) {
    fetch_weather();
    vTaskDelay(pdMS_TO_TICKS(WEATHER_FETCH_INTERVAL_MS));
  }
}

static TaskHandle_t weatherTaskHandle = nullptr;
static void weather_task_start() {
  if (weatherTaskHandle != nullptr) return;
  xTaskCreatePinnedToCore(
    weather_task, "weather", 8192, nullptr, 1, &weatherTaskHandle, 0
  );
}

static void handle_settings_root() {
  String html = F(
    "<!doctype html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>KeroTrack Settings</title>"
    "<style>"
    "body{font-family:system-ui,sans-serif;background:#181c24;color:#eee;padding:24px;max-width:480px;margin:0 auto}"
    "h1{font-size:22px;margin:0 0 8px;color:#4ade80}"
    "h2{font-size:17px;margin:28px 0 12px;color:#aaa;border-bottom:1px solid #333;padding-bottom:6px}"
    "form{margin:0}"
    "label{display:block;font-size:13px;color:#aaa;margin-top:14px}"
    "input{width:100%;padding:9px 10px;margin-top:4px;background:#232a34;color:#eee;border:1px solid #444b58;border-radius:5px;box-sizing:border-box;font-size:15px}"
    "button{margin-top:18px;padding:11px 18px;background:#4ade80;color:#181c24;border:0;border-radius:5px;font-weight:600;cursor:pointer;font-size:15px}"
    "p.note{font-size:12px;color:#888;margin-top:6px}"
    ".kv{font-family:monospace;color:#fcd34d;background:#232a34;padding:6px 10px;border-radius:4px;display:inline-block;margin-top:4px}"
    ".ip{font-family:monospace;color:#4ade80}"
    "</style></head><body>"
    "<h1>KeroTrack Settings</h1>"
    "<p class='note'>Device IP: <span class='ip'>__IP__</span></p>"

    "<h2>MQTT</h2>"
    "<form method='POST' action='/save'>"
    "<label>MQTT broker</label>"
    "<input name='broker' value='__BROKER__' placeholder='192.168.1.10'>"
    "<label>Port</label>"
    "<input name='port' type='number' min='1' max='65535' value='__PORT__'>"
    "<label>Username</label>"
    "<input name='user' value='__USER__'>"
    "<label>Password</label>"
    "<input name='pass' type='password' placeholder='leave blank to keep existing'>"
    "<p class='note'>Saving reboots the device.</p>"
    "<button type='submit'>Save MQTT and reboot</button>"
    "</form>"

    "<h2>Weather</h2>"
    "<form method='POST' action='/search_location'>"
    "<p class='note'>Current location:</p>"
    "<div class='kv'>__LOCDISPLAY__</div>"
    "<label>Change location</label>"
    "<input name='location' placeholder='e.g. Surrey, GB or RH1 2AB'>"
    "<p class='note'>Leave blank to disable weather. Search hits the Open-Meteo geocoding API.</p>"
    "<button type='submit'>Search and change</button>"
    "</form>"

    "</body></html>"
  );
  html.replace("__BROKER__", String(mqtt_broker));
  html.replace("__PORT__", String(mqtt_port));
  html.replace("__USER__", String(mqtt_user));
  html.replace("__IP__", WiFi.localIP().toString());

  String locDisplay;
  if (strlen(weather_location) > 0) {
    locDisplay = String(weather_location) + " (" + weather_lat + ", " + weather_lon + ")";
  } else {
    locDisplay = "(not configured)";
  }
  html.replace("__LOCDISPLAY__", locDisplay);

  settingsServer.send(200, "text/html", html);
}

static void handle_settings_save() {
  if (settingsServer.hasArg("broker")) {
    String v = settingsServer.arg("broker");
    strncpy(mqtt_broker, v.c_str(), sizeof(mqtt_broker) - 1);
    mqtt_broker[sizeof(mqtt_broker) - 1] = '\0';
  }
  if (settingsServer.hasArg("port")) {
    int p = settingsServer.arg("port").toInt();
    if (p >= 1 && p <= 65535) mqtt_port = p;
  }
  if (settingsServer.hasArg("user")) {
    String v = settingsServer.arg("user");
    strncpy(mqtt_user, v.c_str(), sizeof(mqtt_user) - 1);
    mqtt_user[sizeof(mqtt_user) - 1] = '\0';
  }
  String passArg = settingsServer.arg("pass");
  bool pass_changed = passArg.length() > 0;
  if (pass_changed) {
    strncpy(mqtt_pass, passArg.c_str(), sizeof(mqtt_pass) - 1);
    mqtt_pass[sizeof(mqtt_pass) - 1] = '\0';
  }

  prefs.begin("kerotank", false);
  prefs.putString("mqtt_broker", mqtt_broker);
  prefs.putInt("mqtt_port", mqtt_port);
  prefs.putString("mqtt_user", mqtt_user);
  if (pass_changed) prefs.putString("mqtt_pass", mqtt_pass);
  prefs.end();

  Serial.println("Settings saved via web; rebooting in 1.5s");
  settingsServer.send(200, "text/html",
    "<!doctype html><html><body style='font-family:sans-serif;background:#181c24;color:#eee;padding:24px;text-align:center'>"
    "<h2>Saved</h2><p>Rebooting...</p></body></html>");
  delay(1500);
  ESP.restart();
}

static void handle_search_location() {
  String name = settingsServer.arg("location");
  name.trim();

  // Empty input = clear/disable weather.
  if (name.length() == 0) {
    prefs.begin("kerotank", false);
    prefs.putString("loc_name", "");
    prefs.putString("loc_lat", "");
    prefs.putString("loc_lon", "");
    prefs.end();
    weather_location[0] = '\0';
    weather_lat[0] = '\0';
    weather_lon[0] = '\0';
    Serial.println("Weather location cleared");
    settingsServer.send(200, "text/html",
      "<!doctype html><html><head><meta charset='utf-8'>"
      "<style>body{font-family:system-ui,sans-serif;background:#181c24;color:#eee;padding:24px;max-width:420px;margin:0 auto;text-align:center}"
      "a{color:#4ade80}</style></head><body>"
      "<h2>Weather disabled</h2>"
      "<p>Location cleared. The weather screen will hide on next boot.</p>"
      "<p><a href='/'>Back</a> &middot; <a href='javascript:void(0)' onclick='setTimeout(()=>location.reload(),100)'>Reboot now to apply</a></p>"
      "</body></html>");
    return;
  }

  GeocodeMatch matches[5];
  int count = geocode_location(name.c_str(), matches, 5);

  if (count < 0) {
    settingsServer.send(200, "text/html",
      "<!doctype html><html><head><meta charset='utf-8'>"
      "<style>body{font-family:system-ui,sans-serif;background:#181c24;color:#eee;padding:24px;max-width:420px;margin:0 auto}"
      "a{color:#4ade80}</style></head><body>"
      "<h2>Lookup failed</h2>"
      "<p class='note'>Network error or geocoding API unavailable. Check WiFi and try again.</p>"
      "<p><a href='/'>Back</a></p>"
      "</body></html>");
    return;
  }
  if (count == 0) {
    String html = "<!doctype html><html><head><meta charset='utf-8'>"
      "<style>body{font-family:system-ui,sans-serif;background:#181c24;color:#eee;padding:24px;max-width:420px;margin:0 auto}"
      "a{color:#4ade80}</style></head><body>"
      "<h2>No matches</h2>"
      "<p>Couldn't find anywhere called \"" + name + "\". Try a town/city or postcode.</p>"
      "<p><a href='/'>Back</a></p></body></html>";
    settingsServer.send(200, "text/html", html);
    return;
  }

  // 1+ matches — render confirmation form.
  String html = F("<!doctype html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Confirm location</title>"
    "<style>"
    "body{font-family:system-ui,sans-serif;background:#181c24;color:#eee;padding:24px;max-width:480px;margin:0 auto}"
    "h2{color:#4ade80;font-size:20px}"
    "label.match{display:block;background:#232a34;border:1px solid #444b58;border-radius:6px;padding:10px 12px;margin-top:8px;cursor:pointer}"
    "label.match input{margin-right:10px}"
    "label.match .name{font-weight:600}"
    "label.match .coord{color:#888;font-family:monospace;font-size:12px;display:block;margin-top:2px}"
    "button{margin-top:18px;padding:11px 18px;background:#4ade80;color:#181c24;border:0;border-radius:5px;font-weight:600;cursor:pointer;font-size:15px}"
    "a{color:#888}"
    "</style></head><body>"
    "<h2>Confirm location</h2>"
    "<p>Pick the match that fits best:</p>"
    "<form method='POST' action='/save_location'>");
  for (int i = 0; i < count; i++) {
    String label = matches[i].name;
    if (matches[i].admin1.length()) label += ", " + matches[i].admin1;
    if (matches[i].country.length()) label += ", " + matches[i].country;
    html += "<label class='match'>"
            "<input type='radio' name='choice' value='" + String(i) + "'";
    if (i == 0) html += " checked";
    html += "><span class='name'>" + label + "</span>"
            "<span class='coord'>" + matches[i].latitude + ", " + matches[i].longitude + "</span></label>";

    html += "<input type='hidden' name='name_" + String(i) + "' value='" + label + "'>";
    html += "<input type='hidden' name='lat_" + String(i) + "' value='" + matches[i].latitude + "'>";
    html += "<input type='hidden' name='lon_" + String(i) + "' value='" + matches[i].longitude + "'>";
  }
  html += "<button type='submit'>Save and reboot</button>"
          "<p><a href='/'>Cancel</a></p></form></body></html>";
  settingsServer.send(200, "text/html", html);
}

static void handle_save_location() {
  if (!settingsServer.hasArg("choice")) {
    settingsServer.send(400, "text/plain", "Missing choice");
    return;
  }
  int idx = settingsServer.arg("choice").toInt();
  if (idx < 0 || idx > 4) {
    settingsServer.send(400, "text/plain", "Invalid choice");
    return;
  }
  String name = settingsServer.arg(String("name_") + idx);
  String lat  = settingsServer.arg(String("lat_") + idx);
  String lon  = settingsServer.arg(String("lon_") + idx);
  if (lat.length() == 0 || lon.length() == 0) {
    settingsServer.send(400, "text/plain", "Missing lat/lon for chosen match");
    return;
  }

  strncpy(weather_location, name.c_str(), sizeof(weather_location) - 1);
  weather_location[sizeof(weather_location) - 1] = '\0';
  strncpy(weather_lat, lat.c_str(), sizeof(weather_lat) - 1);
  weather_lat[sizeof(weather_lat) - 1] = '\0';
  strncpy(weather_lon, lon.c_str(), sizeof(weather_lon) - 1);
  weather_lon[sizeof(weather_lon) - 1] = '\0';

  prefs.begin("kerotank", false);
  prefs.putString("loc_name", weather_location);
  prefs.putString("loc_lat", weather_lat);
  prefs.putString("loc_lon", weather_lon);
  prefs.end();

  Serial.print("Location saved via web: "); Serial.print(weather_location);
  Serial.print(" ("); Serial.print(weather_lat); Serial.print(","); Serial.print(weather_lon); Serial.println(")");

  settingsServer.send(200, "text/html",
    "<!doctype html><html><head><meta charset='utf-8'></head>"
    "<body style='font-family:system-ui,sans-serif;background:#181c24;color:#eee;padding:24px;text-align:center;max-width:420px;margin:0 auto'>"
    "<h2 style='color:#4ade80'>Location saved</h2>"
    "<p>Rebooting...</p></body></html>");
  delay(1500);
  ESP.restart();
}

void loop() {
  lv_timer_handler();
  settingsServer.handleClient();
  if (mqtt_is_configured) {
    mqttClient.loop();
    mqtt_try_reconnect();
  }
  lv_tick_inc(5);
  delay(5);
}

static void ribbon_pulse_anim(void *obj, int32_t v) {
  lv_obj_set_style_bg_opa((lv_obj_t*)obj, v, LV_PART_MAIN);
}

static void create_chrome(lv_obj_t *parent, screen_chrome_t *out, const char *dots_str) {
  // Background (matches existing dark theme)
  lv_obj_set_style_bg_color(parent, lv_color_hex(0x181c24), LV_PART_MAIN);
  lv_obj_set_style_bg_grad_color(parent, lv_color_hex(0x232a34), LV_PART_MAIN);
  lv_obj_set_style_bg_grad_dir(parent, LV_GRAD_DIR_VER, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_pad_all(parent, 0, LV_PART_MAIN);
  lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

  // --- Status bar (36px) ---
  lv_obj_t *sb = lv_obj_create(parent);
  lv_obj_set_size(sb, SCREEN_WIDTH, 36);
  lv_obj_align(sb, LV_ALIGN_TOP_MID, 0, 0);
  lv_obj_set_style_bg_opa(sb, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(sb, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_hor(sb, 8, LV_PART_MAIN);
  lv_obj_set_style_pad_ver(sb, 6, LV_PART_MAIN);
  lv_obj_clear_flag(sb, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(sb, LV_OBJ_FLAG_EVENT_BUBBLE);

  out->sb.icon_wifi = lv_label_create(sb);
  lv_label_set_text(out->sb.icon_wifi, LV_SYMBOL_WIFI);
  lv_obj_set_style_text_font(out->sb.icon_wifi, &lv_font_montserrat_16, LV_PART_MAIN);
  lv_obj_set_style_text_color(out->sb.icon_wifi, lv_color_hex(0x88a0c0), LV_PART_MAIN);
  lv_obj_align(out->sb.icon_wifi, LV_ALIGN_LEFT_MID, 0, 0);

  out->sb.dot_mqtt = lv_obj_create(sb);
  lv_obj_set_size(out->sb.dot_mqtt, 10, 10);
  lv_obj_set_style_radius(out->sb.dot_mqtt, 5, LV_PART_MAIN);
  lv_obj_set_style_bg_color(out->sb.dot_mqtt, lv_color_hex(0x4ade80), LV_PART_MAIN);
  lv_obj_set_style_border_width(out->sb.dot_mqtt, 0, LV_PART_MAIN);
  lv_obj_align(out->sb.dot_mqtt, LV_ALIGN_LEFT_MID, 28, 0);

  out->sb.lbl_clock = lv_label_create(sb);
  lv_label_set_text(out->sb.lbl_clock, "");
  lv_obj_set_style_text_font(out->sb.lbl_clock, &lv_font_montserrat_16, LV_PART_MAIN);
  lv_obj_set_style_text_color(out->sb.lbl_clock, lv_color_hex(0x88a0c0), LV_PART_MAIN);
  lv_obj_align(out->sb.lbl_clock, LV_ALIGN_CENTER, 0, 0);

  out->sb.lbl_age = lv_label_create(sb);
  lv_label_set_text(out->sb.lbl_age, "--");
  lv_obj_set_style_text_font(out->sb.lbl_age, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_set_style_text_color(out->sb.lbl_age, lv_color_hex(0x88a0c0), LV_PART_MAIN);
  lv_obj_align(out->sb.lbl_age, LV_ALIGN_RIGHT_MID, 0, 0);

  // --- Leak ribbon (28px), hidden by default ---
  out->ribbon = lv_obj_create(parent);
  lv_obj_set_size(out->ribbon, SCREEN_WIDTH, 28);
  lv_obj_align(out->ribbon, LV_ALIGN_TOP_MID, 0, 36);
  lv_obj_set_style_bg_color(out->ribbon, lv_color_hex(0x8b2323), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(out->ribbon, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(out->ribbon, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(out->ribbon, 0, LV_PART_MAIN);
  lv_obj_clear_flag(out->ribbon, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(out->ribbon, LV_OBJ_FLAG_EVENT_BUBBLE);
  lv_obj_add_flag(out->ribbon, LV_OBJ_FLAG_HIDDEN);

  out->ribbon_label = lv_label_create(out->ribbon);
  lv_label_set_text(out->ribbon_label, "LEAK DETECTED");
  lv_obj_set_style_text_font(out->ribbon_label, &lv_font_montserrat_16, LV_PART_MAIN);
  lv_obj_set_style_text_color(out->ribbon_label, lv_color_hex(0xffffff), LV_PART_MAIN);
  lv_obj_align(out->ribbon_label, LV_ALIGN_CENTER, 0, 0);

  // Soft pulse on the ribbon (continuous; harmless when ribbon is hidden)
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, out->ribbon);
  lv_anim_set_values(&a, LV_OPA_70, LV_OPA_COVER);
  lv_anim_set_time(&a, 800);
  lv_anim_set_playback_time(&a, 800);
  lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
  lv_anim_set_exec_cb(&a, ribbon_pulse_anim);
  lv_anim_start(&a);

  // --- Content container (everything between status bar and dots) ---
  out->content = lv_obj_create(parent);
  lv_obj_set_size(out->content, SCREEN_WIDTH, SCREEN_HEIGHT - 36 - 28);
  lv_obj_align(out->content, LV_ALIGN_TOP_MID, 0, 36);
  lv_obj_set_style_bg_opa(out->content, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(out->content, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(out->content, 0, LV_PART_MAIN);
  lv_obj_clear_flag(out->content, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(out->content, LV_OBJ_FLAG_EVENT_BUBBLE);

  // --- Bottom dots indicator ---
  out->dots = lv_label_create(parent);
  lv_label_set_text(out->dots, dots_str);
  lv_obj_set_style_text_font(out->dots, &lv_font_montserrat_20, LV_PART_MAIN);
  lv_obj_set_style_text_color(out->dots, lv_color_hex(0x666666), LV_PART_MAIN);
  lv_obj_set_style_text_letter_space(out->dots, 6, LV_PART_MAIN);
  lv_obj_align(out->dots, LV_ALIGN_BOTTOM_MID, 0, -8);
}

void create_screen1() {
  if (screen1) lv_obj_clean(screen1);
  else screen1 = lv_obj_create(NULL);
  create_chrome(screen1, &s1.chrome, "*...");

  lv_obj_t *c = s1.chrome.content;

  s1.lbl_pct = lv_label_create(c);
  lv_label_set_text(s1.lbl_pct, "--%");
  lv_obj_set_style_text_font(s1.lbl_pct, &lv_font_montserrat_48, LV_PART_MAIN);
  lv_obj_set_style_text_color(s1.lbl_pct, lv_color_hex(0x4ade80), LV_PART_MAIN);
  lv_obj_align(s1.lbl_pct, LV_ALIGN_TOP_MID, 0, 30);

  lv_obj_t *sub = lv_label_create(c);
  lv_label_set_text(sub, "OIL LEVEL");
  lv_obj_set_style_text_font(sub, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_set_style_text_color(sub, lv_color_hex(0x888888), LV_PART_MAIN);
  lv_obj_set_style_text_letter_space(sub, 3, LV_PART_MAIN);
  lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 90);

  s1.lbl_litres = lv_label_create(c);
  lv_label_set_text(s1.lbl_litres, "-- L");
  lv_obj_set_style_text_font(s1.lbl_litres, &lv_font_montserrat_28, LV_PART_MAIN);
  lv_obj_set_style_text_color(s1.lbl_litres, lv_color_hex(0xeeeeee), LV_PART_MAIN);
  lv_obj_align(s1.lbl_litres, LV_ALIGN_TOP_MID, 0, 115);

  s1.bar_fill = lv_bar_create(c);
  lv_obj_set_size(s1.bar_fill, SCREEN_WIDTH - 40, 12);
  lv_obj_align(s1.bar_fill, LV_ALIGN_TOP_MID, 0, 165);
  lv_bar_set_range(s1.bar_fill, 0, 100);
  lv_bar_set_value(s1.bar_fill, 0, LV_ANIM_OFF);
  lv_obj_set_style_bg_color(s1.bar_fill, lv_color_hex(0x2a2f3a), LV_PART_MAIN);
  lv_obj_set_style_bg_color(s1.bar_fill, lv_color_hex(0x4ade80), LV_PART_INDICATOR);
  lv_obj_set_style_radius(s1.bar_fill, 6, LV_PART_MAIN);
  lv_obj_set_style_radius(s1.bar_fill, 6, LV_PART_INDICATOR);
  lv_obj_add_flag(s1.bar_fill, LV_OBJ_FLAG_EVENT_BUBBLE);

  s1.lbl_empty_date = lv_label_create(c);
  lv_label_set_text(s1.lbl_empty_date, "Empty --\n-- days");
  lv_obj_set_style_text_font(s1.lbl_empty_date, &lv_font_montserrat_16, LV_PART_MAIN);
  lv_obj_set_style_text_color(s1.lbl_empty_date, lv_color_hex(0xaaaaaa), LV_PART_MAIN);
  lv_obj_set_style_text_align(s1.lbl_empty_date, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_align(s1.lbl_empty_date, LV_ALIGN_TOP_MID, 0, 188);
}

void create_screen2() {
  if (screen2) lv_obj_clean(screen2);
  else screen2 = lv_obj_create(NULL);
  create_chrome(screen2, &s2.chrome, ".*..");

  lv_obj_t *c = s2.chrome.content;

  s2.lbl_used = lv_label_create(c);
  lv_label_set_text(s2.lbl_used, "-- L");
  lv_obj_set_style_text_font(s2.lbl_used, &lv_font_montserrat_48, LV_PART_MAIN);
  lv_obj_set_style_text_color(s2.lbl_used, lv_color_hex(0xeeeeee), LV_PART_MAIN);
  lv_obj_align(s2.lbl_used, LV_ALIGN_TOP_MID, 0, 20);

  lv_obj_t *sub = lv_label_create(c);
  lv_label_set_text(sub, "USED SINCE REFILL");
  lv_obj_set_style_text_font(sub, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_set_style_text_color(sub, lv_color_hex(0x888888), LV_PART_MAIN);
  lv_obj_set_style_text_letter_space(sub, 3, LV_PART_MAIN);
  lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 80);

  s2.lbl_over_days = lv_label_create(c);
  lv_label_set_text(s2.lbl_over_days, "over -- days");
  lv_obj_set_style_text_font(s2.lbl_over_days, &lv_font_montserrat_16, LV_PART_MAIN);
  lv_obj_set_style_text_color(s2.lbl_over_days, lv_color_hex(0xaaaaaa), LV_PART_MAIN);
  lv_obj_align(s2.lbl_over_days, LV_ALIGN_TOP_MID, 0, 105);

  // Divider
  lv_obj_t *div = lv_obj_create(c);
  lv_obj_set_size(div, 80, 1);
  lv_obj_set_style_bg_color(div, lv_color_hex(0x444b58), LV_PART_MAIN);
  lv_obj_set_style_border_width(div, 0, LV_PART_MAIN);
  lv_obj_align(div, LV_ALIGN_TOP_MID, 0, 135);

  // Split bar — two adjacent rectangles, total width = SCREEN_WIDTH - 40 = 200px
  s2.bar_split_hw = lv_obj_create(c);
  lv_obj_set_size(s2.bar_split_hw, 100, 14);
  lv_obj_set_style_bg_color(s2.bar_split_hw, lv_color_hex(0x4a90e2), LV_PART_MAIN);
  lv_obj_set_style_border_width(s2.bar_split_hw, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(s2.bar_split_hw, 0, LV_PART_MAIN);
  lv_obj_align(s2.bar_split_hw, LV_ALIGN_TOP_LEFT, 20, 155);
  lv_obj_add_flag(s2.bar_split_hw, LV_OBJ_FLAG_EVENT_BUBBLE);

  s2.bar_split_heat = lv_obj_create(c);
  lv_obj_set_size(s2.bar_split_heat, 100, 14);
  lv_obj_set_style_bg_color(s2.bar_split_heat, lv_color_hex(0xe28b3a), LV_PART_MAIN);
  lv_obj_set_style_border_width(s2.bar_split_heat, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(s2.bar_split_heat, 0, LV_PART_MAIN);
  lv_obj_align(s2.bar_split_heat, LV_ALIGN_TOP_LEFT, 120, 155);
  lv_obj_add_flag(s2.bar_split_heat, LV_OBJ_FLAG_EVENT_BUBBLE);

  // Legend
  s2.lbl_hw_legend = lv_label_create(c);
  lv_label_set_text(s2.lbl_hw_legend, "* -- L hot water");
  lv_obj_set_style_text_font(s2.lbl_hw_legend, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_set_style_text_color(s2.lbl_hw_legend, lv_color_hex(0x4a90e2), LV_PART_MAIN);
  lv_obj_align(s2.lbl_hw_legend, LV_ALIGN_TOP_LEFT, 20, 175);

  s2.lbl_heat_legend = lv_label_create(c);
  lv_label_set_text(s2.lbl_heat_legend, "* -- L heating");
  lv_obj_set_style_text_font(s2.lbl_heat_legend, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_set_style_text_color(s2.lbl_heat_legend, lv_color_hex(0xe28b3a), LV_PART_MAIN);
  lv_obj_align(s2.lbl_heat_legend, LV_ALIGN_TOP_LEFT, 20, 192);

  s2.lbl_avg_burn = lv_label_create(c);
  lv_label_set_text(s2.lbl_avg_burn, "-- L/day avg burn");
  lv_obj_set_style_text_font(s2.lbl_avg_burn, &lv_font_montserrat_16, LV_PART_MAIN);
  lv_obj_set_style_text_color(s2.lbl_avg_burn, lv_color_hex(0xaaaaaa), LV_PART_MAIN);
  lv_obj_align(s2.lbl_avg_burn, LV_ALIGN_TOP_MID, 0, 218);

  s2.lbl_refill_note = lv_label_create(c);
  lv_label_set_text(s2.lbl_refill_note, LV_SYMBOL_UP " refill detected today");
  lv_obj_set_style_text_font(s2.lbl_refill_note, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_set_style_text_color(s2.lbl_refill_note, lv_color_hex(0x4ade80), LV_PART_MAIN);
  lv_obj_align(s2.lbl_refill_note, LV_ALIGN_TOP_MID, 0, 240);
  lv_obj_add_flag(s2.lbl_refill_note, LV_OBJ_FLAG_HIDDEN);
}

void create_screen3() {
  if (screen3) lv_obj_clean(screen3);
  else screen3 = lv_obj_create(NULL);
  create_chrome(screen3, &s3.chrome, "..*.");

  lv_obj_t *c = s3.chrome.content;

  s3.lbl_avg_monthly = lv_label_create(c);
  lv_label_set_text(s3.lbl_avg_monthly, "--");
  lv_obj_set_style_text_font(s3.lbl_avg_monthly, &lv_font_montserrat_48, LV_PART_MAIN);
  lv_obj_set_style_text_color(s3.lbl_avg_monthly, lv_color_hex(0xfcd34d), LV_PART_MAIN);
  lv_obj_align(s3.lbl_avg_monthly, LV_ALIGN_TOP_MID, 0, 20);

  lv_obj_t *sub = lv_label_create(c);
  lv_label_set_text(sub, "AVG MONTHLY (GBP)");
  lv_obj_set_style_text_font(sub, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_set_style_text_color(sub, lv_color_hex(0x888888), LV_PART_MAIN);
  lv_obj_set_style_text_letter_space(sub, 3, LV_PART_MAIN);
  lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 80);

  s3.lbl_avg_weekly = lv_label_create(c);
  lv_label_set_text(s3.lbl_avg_weekly, "-- /wk");
  lv_obj_set_style_text_font(s3.lbl_avg_weekly, &lv_font_montserrat_16, LV_PART_MAIN);
  lv_obj_set_style_text_color(s3.lbl_avg_weekly, lv_color_hex(0xfcd34d), LV_PART_MAIN);
  lv_obj_align(s3.lbl_avg_weekly, LV_ALIGN_TOP_LEFT, 30, 110);

  s3.lbl_avg_annual = lv_label_create(c);
  lv_label_set_text(s3.lbl_avg_annual, "-- /yr");
  lv_obj_set_style_text_font(s3.lbl_avg_annual, &lv_font_montserrat_16, LV_PART_MAIN);
  lv_obj_set_style_text_color(s3.lbl_avg_annual, lv_color_hex(0xfcd34d), LV_PART_MAIN);
  lv_obj_align(s3.lbl_avg_annual, LV_ALIGN_TOP_RIGHT, -30, 110);

  // Divider
  lv_obj_t *div = lv_obj_create(c);
  lv_obj_set_size(div, 80, 1);
  lv_obj_set_style_bg_color(div, lv_color_hex(0x444b58), LV_PART_MAIN);
  lv_obj_set_style_border_width(div, 0, LV_PART_MAIN);
  lv_obj_align(div, LV_ALIGN_TOP_MID, 0, 145);

  // To-fill cell (container so we can toggle bg/border)
  s3.cell_to_fill = lv_obj_create(c);
  lv_obj_set_size(s3.cell_to_fill, 100, 60);
  lv_obj_align(s3.cell_to_fill, LV_ALIGN_TOP_LEFT, 20, 165);
  lv_obj_set_style_bg_opa(s3.cell_to_fill, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(s3.cell_to_fill, 1, LV_PART_MAIN);
  lv_obj_set_style_border_color(s3.cell_to_fill, lv_color_hex(0x444b58), LV_PART_MAIN);
  lv_obj_set_style_radius(s3.cell_to_fill, 6, LV_PART_MAIN);
  lv_obj_set_style_pad_all(s3.cell_to_fill, 4, LV_PART_MAIN);
  lv_obj_clear_flag(s3.cell_to_fill, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(s3.cell_to_fill, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
  lv_obj_add_flag(s3.cell_to_fill, LV_OBJ_FLAG_EVENT_BUBBLE);

  s3.lbl_cost_to_fill = lv_label_create(s3.cell_to_fill);
  lv_label_set_text(s3.lbl_cost_to_fill, "--");
  lv_obj_set_style_text_font(s3.lbl_cost_to_fill, &lv_font_montserrat_28, LV_PART_MAIN);
  lv_obj_set_style_text_color(s3.lbl_cost_to_fill, lv_color_hex(0xfcd34d), LV_PART_MAIN);
  lv_obj_align(s3.lbl_cost_to_fill, LV_ALIGN_TOP_MID, 0, 0);

  s3.lbl_to_fill_label = lv_label_create(s3.cell_to_fill);
  lv_label_set_text(s3.lbl_to_fill_label, "to fill");
  lv_obj_set_style_text_font(s3.lbl_to_fill_label, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_set_style_text_color(s3.lbl_to_fill_label, lv_color_hex(0x888888), LV_PART_MAIN);
  lv_obj_align(s3.lbl_to_fill_label, LV_ALIGN_BOTTOM_MID, 0, 0);

  s3.pip = lv_obj_create(s3.cell_to_fill);
  lv_obj_set_size(s3.pip, 12, 12);
  lv_obj_set_style_radius(s3.pip, 6, LV_PART_MAIN);
  lv_obj_set_style_bg_color(s3.pip, lv_color_hex(0x4ade80), LV_PART_MAIN);
  lv_obj_set_style_border_width(s3.pip, 2, LV_PART_MAIN);
  lv_obj_set_style_border_color(s3.pip, lv_color_hex(0x181c24), LV_PART_MAIN);
  lv_obj_align(s3.pip, LV_ALIGN_TOP_RIGHT, 4, -4);
  lv_obj_add_flag(s3.pip, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(s3.pip, LV_OBJ_FLAG_EVENT_BUBBLE);

  s3.lbl_ppl = lv_label_create(c);
  lv_label_set_text(s3.lbl_ppl, "--");
  lv_obj_set_style_text_font(s3.lbl_ppl, &lv_font_montserrat_28, LV_PART_MAIN);
  lv_obj_set_style_text_color(s3.lbl_ppl, lv_color_hex(0xeeeeee), LV_PART_MAIN);
  lv_obj_align(s3.lbl_ppl, LV_ALIGN_TOP_RIGHT, -15, 169);

  lv_obj_t *ppl_label = lv_label_create(c);
  lv_label_set_text(ppl_label, "current p/L");
  lv_obj_set_style_text_font(ppl_label, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_set_style_text_color(ppl_label, lv_color_hex(0x888888), LV_PART_MAIN);
  lv_obj_align(ppl_label, LV_ALIGN_TOP_RIGHT, -15, 207);
}

void create_screen4() {
  if (screen4) lv_obj_clean(screen4);
  else screen4 = lv_obj_create(NULL);
  create_chrome(screen4, &s4.chrome, "...*");

  lv_obj_t *c = s4.chrome.content;

  // Hero weather icon (20x20 native, scaled 3x = 60 visible).
  s4.icon_hero = lv_image_create(c);
  lv_image_set_src(s4.icon_hero, &icon_cloudy);
  lv_image_set_scale(s4.icon_hero, 768);  // 3x in LVGL units (256 = 1x)
  lv_obj_align(s4.icon_hero, LV_ALIGN_TOP_MID, 0, 25);
  lv_obj_add_flag(s4.icon_hero, LV_OBJ_FLAG_EVENT_BUBBLE);

  // Big current temperature (using Latin font for ° support).
  s4.lbl_temp = lv_label_create(c);
  lv_label_set_text(s4.lbl_temp, "--°");
  lv_obj_set_style_text_font(s4.lbl_temp, &lv_font_montserrat_latin_42, LV_PART_MAIN);
  lv_obj_set_style_text_color(s4.lbl_temp, lv_color_hex(0xeeeeee), LV_PART_MAIN);
  lv_obj_align(s4.lbl_temp, LV_ALIGN_TOP_MID, 0, 95);

  // Feels-like + condition descriptor.
  s4.lbl_descriptor = lv_label_create(c);
  lv_label_set_text(s4.lbl_descriptor, "");
  lv_obj_set_style_text_font(s4.lbl_descriptor, &lv_font_montserrat_latin_14, LV_PART_MAIN);
  lv_obj_set_style_text_color(s4.lbl_descriptor, lv_color_hex(0xaaaaaa), LV_PART_MAIN);
  lv_obj_align(s4.lbl_descriptor, LV_ALIGN_TOP_MID, 0, 142);

  // Location name.
  s4.lbl_location = lv_label_create(c);
  lv_label_set_text(s4.lbl_location, "(not configured)");
  lv_obj_set_style_text_font(s4.lbl_location, &lv_font_montserrat_latin_14, LV_PART_MAIN);
  lv_obj_set_style_text_color(s4.lbl_location, lv_color_hex(0x888888), LV_PART_MAIN);
  lv_obj_align(s4.lbl_location, LV_ALIGN_TOP_MID, 0, 160);

  // Divider.
  lv_obj_t *div = lv_obj_create(c);
  lv_obj_set_size(div, 80, 1);
  lv_obj_set_style_bg_color(div, lv_color_hex(0x444b58), LV_PART_MAIN);
  lv_obj_set_style_border_width(div, 0, LV_PART_MAIN);
  lv_obj_align(div, LV_ALIGN_TOP_MID, 0, 182);

  // 3-day forecast rows: icon | day name | high/low.
  for (int i = 0; i < 3; i++) {
    int y = 192 + i * 22;

    s4.daily_icon[i] = lv_image_create(c);
    lv_image_set_src(s4.daily_icon[i], &icon_cloudy);
    lv_obj_align(s4.daily_icon[i], LV_ALIGN_TOP_LEFT, 30, y);
    lv_obj_add_flag(s4.daily_icon[i], LV_OBJ_FLAG_EVENT_BUBBLE);

    s4.daily_day[i] = lv_label_create(c);
    lv_label_set_text(s4.daily_day[i], "---");
    lv_obj_set_style_text_font(s4.daily_day[i], &lv_font_montserrat_latin_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(s4.daily_day[i], lv_color_hex(0xeeeeee), LV_PART_MAIN);
    lv_obj_align(s4.daily_day[i], LV_ALIGN_TOP_LEFT, 65, y + 3);

    s4.daily_range[i] = lv_label_create(c);
    lv_label_set_text(s4.daily_range[i], "--/--");
    lv_obj_set_style_text_font(s4.daily_range[i], &lv_font_montserrat_latin_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(s4.daily_range[i], lv_color_hex(0xeeeeee), LV_PART_MAIN);
    lv_obj_align(s4.daily_range[i], LV_ALIGN_TOP_RIGHT, -30, y + 3);
  }
}

// Format "2026-08-23 14:32:00" -> "23 Aug 2026". Returns false on parse failure.
static bool format_empty_date(const String &iso, char *out, size_t cap) {
  if (iso.length() < 10) return false;
  static const char *MONTHS[] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
  int year = iso.substring(0, 4).toInt();
  int mon  = iso.substring(5, 7).toInt();
  int day  = iso.substring(8, 10).toInt();
  if (mon < 1 || mon > 12 || year < 2020 || day < 1 || day > 31) return false;
  snprintf(out, cap, "%d %s %d", day, MONTHS[mon-1], year);
  return true;
}

static uint32_t pct_color(float pct) {
  if (pct >= 50.0f) return 0x4ade80; // green
  if (pct >= 25.0f) return 0xffb74d; // amber
  return 0xff5555;                   // red
}

static void update_screen1() {
  if (!screen1) return;
  char buf[40];
  // Hero %
  snprintf(buf, sizeof(buf), "%.0f%%", oilTankData.percentage_remaining);
  lv_label_set_text(s1.lbl_pct, buf);
  uint32_t col = pct_color(oilTankData.percentage_remaining);
  lv_obj_set_style_text_color(s1.lbl_pct, lv_color_hex(col), LV_PART_MAIN);
  // Litres (with thousands separator)
  long litres = (long)(oilTankData.litres_remaining + 0.5f);
  if (litres >= 1000) snprintf(buf, sizeof(buf), "%ld,%03ld L", litres/1000, litres%1000);
  else                snprintf(buf, sizeof(buf), "%ld L", litres);
  lv_label_set_text(s1.lbl_litres, buf);
  // Bar
  int v = (int)oilTankData.percentage_remaining;
  if (v < 0) v = 0; if (v > 100) v = 100;
  lv_bar_set_value(s1.bar_fill, v, LV_ANIM_OFF);
  lv_obj_set_style_bg_color(s1.bar_fill, lv_color_hex(col), LV_PART_INDICATOR);
  // Empty date
  char date_buf[16];
  if (format_empty_date(oilTankAnalysis.estimated_empty_date, date_buf, sizeof(date_buf))) {
    snprintf(buf, sizeof(buf), "Empty %s\n%.0f days", date_buf, oilTankAnalysis.estimated_days_remaining);
  } else {
    snprintf(buf, sizeof(buf), "%.0f days remaining", oilTankAnalysis.estimated_days_remaining);
  }
  lv_label_set_text(s1.lbl_empty_date, buf);
}

static void update_screen2() {
  if (!screen2) return;
  char buf[40];
  long used = (long)(oilTankAnalysis.total_consumption_since_refill + 0.5f);
  if (used >= 1000) snprintf(buf, sizeof(buf), "%ld,%03ld L", used/1000, used%1000);
  else              snprintf(buf, sizeof(buf), "%ld L", used);
  lv_label_set_text(s2.lbl_used, buf);

  snprintf(buf, sizeof(buf), "over %d days", oilTankAnalysis.days_since_refill);
  lv_label_set_text(s2.lbl_over_days, buf);

  // Split bar — proportional widths sum to (SCREEN_WIDTH - 40) px = 200px.
  float hw   = oilTankAnalysis.estimated_daily_hot_water_consumption_l;
  float heat = oilTankAnalysis.estimated_daily_heating_consumption_l;
  float total = hw + heat;
  const int total_px = SCREEN_WIDTH - 40;
  if (total <= 0.01f) {
    // Both effectively zero — show a single muted bar
    lv_obj_set_size(s2.bar_split_hw, total_px, 14);
    lv_obj_set_style_bg_color(s2.bar_split_hw, lv_color_hex(0x444b58), LV_PART_MAIN);
    lv_obj_set_size(s2.bar_split_heat, 0, 14);
  } else {
    int hw_px   = (int)(total_px * (hw / total) + 0.5f);
    int heat_px = total_px - hw_px;
    lv_obj_set_size(s2.bar_split_hw, hw_px, 14);
    lv_obj_set_style_bg_color(s2.bar_split_hw, lv_color_hex(0x4a90e2), LV_PART_MAIN);
    lv_obj_set_size(s2.bar_split_heat, heat_px, 14);
    lv_obj_align(s2.bar_split_heat, LV_ALIGN_TOP_LEFT, 20 + hw_px, 155);
  }

  snprintf(buf, sizeof(buf), "* %.1f L hot water", hw);
  lv_label_set_text(s2.lbl_hw_legend, buf);
  snprintf(buf, sizeof(buf), "* %.1f L heating", heat);
  lv_label_set_text(s2.lbl_heat_legend, buf);

  snprintf(buf, sizeof(buf), "%.1f L/day avg burn", oilTankAnalysis.avg_daily_consumption_l);
  lv_label_set_text(s2.lbl_avg_burn, buf);

  bool refill = oilTankData.refill_detected == "y";
  if (refill) lv_obj_clear_flag(s2.lbl_refill_note, LV_OBJ_FLAG_HIDDEN);
  else        lv_obj_add_flag(s2.lbl_refill_note,   LV_OBJ_FLAG_HIDDEN);
}

static void update_screen3() {
  if (!screen3) return;
  char buf[24];
  snprintf(buf, sizeof(buf), "%.0f", oilTankCost.avg_monthly_cost);
  lv_label_set_text(s3.lbl_avg_monthly, buf);
  snprintf(buf, sizeof(buf), "%.0f /wk", oilTankCost.avg_weekly_cost);
  lv_label_set_text(s3.lbl_avg_weekly, buf);
  snprintf(buf, sizeof(buf), "%.0f /yr", oilTankCost.avg_annual_cost);
  lv_label_set_text(s3.lbl_avg_annual, buf);

  snprintf(buf, sizeof(buf), "%.0f", oilTankData.cost_to_fill);
  lv_label_set_text(s3.lbl_cost_to_fill, buf);

  snprintf(buf, sizeof(buf), "%.1f", oilTankData.current_ppl);
  lv_label_set_text(s3.lbl_ppl, buf);

  // 500L threshold styling on the to-fill cell
  bool order_ready = oilTankData.litres_to_order >= MIN_ORDER_LITRES;
  if (order_ready) {
    lv_obj_set_style_bg_opa(s3.cell_to_fill, LV_OPA_30, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s3.cell_to_fill, lv_color_hex(0x1e3d2a), LV_PART_MAIN);
    lv_obj_set_style_border_color(s3.cell_to_fill, lv_color_hex(0x4ade80), LV_PART_MAIN);
    lv_obj_set_style_text_color(s3.lbl_cost_to_fill, lv_color_hex(0x4ade80), LV_PART_MAIN);
    lv_obj_set_style_text_color(s3.lbl_to_fill_label, lv_color_hex(0x4ade80), LV_PART_MAIN);
    lv_label_set_text(s3.lbl_to_fill_label, "order ready");
    lv_obj_clear_flag(s3.pip, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_set_style_bg_opa(s3.cell_to_fill, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_color(s3.cell_to_fill, lv_color_hex(0x444b58), LV_PART_MAIN);
    lv_obj_set_style_text_color(s3.lbl_cost_to_fill, lv_color_hex(0xfcd34d), LV_PART_MAIN);
    lv_obj_set_style_text_color(s3.lbl_to_fill_label, lv_color_hex(0x888888), LV_PART_MAIN);
    lv_label_set_text(s3.lbl_to_fill_label, "to fill");
    lv_obj_add_flag(s3.pip, LV_OBJ_FLAG_HIDDEN);
  }
}

static void update_screen4() {
  if (!screen4) return;
  char buf[64];

  bool configured = strlen(weather_location) > 0;
  bool data_valid = weatherData.valid;

  // -- "Not configured" state --
  if (!configured) {
    lv_obj_add_flag(s4.icon_hero, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s4.lbl_temp, "--");
    lv_label_set_text(s4.lbl_descriptor, "Weather not configured");
    lv_label_set_text(s4.lbl_location, "Set location in web settings");
    for (int i = 0; i < 3; i++) {
      lv_obj_add_flag(s4.daily_icon[i], LV_OBJ_FLAG_HIDDEN);
      lv_label_set_text(s4.daily_day[i], "");
      lv_label_set_text(s4.daily_range[i], "");
    }
    return;
  }

  // -- "Fetching" state (location set, but no data yet) --
  if (!data_valid) {
    lv_obj_add_flag(s4.icon_hero, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s4.lbl_temp, "--");
    lv_label_set_text(s4.lbl_descriptor, "Fetching forecast...");
    lv_label_set_text(s4.lbl_location, weather_location);
    for (int i = 0; i < 3; i++) {
      lv_obj_add_flag(s4.daily_icon[i], LV_OBJ_FLAG_HIDDEN);
      lv_label_set_text(s4.daily_day[i], "");
      lv_label_set_text(s4.daily_range[i], "");
    }
    return;
  }

  // -- Active state --
  lv_obj_clear_flag(s4.icon_hero, LV_OBJ_FLAG_HIDDEN);
  lv_image_set_src(s4.icon_hero, choose_icon(weatherData.code_now, weatherData.is_day));

  snprintf(buf, sizeof(buf), "%.0f\xc2\xb0", weatherData.temp_now);
  lv_label_set_text(s4.lbl_temp, buf);

  snprintf(buf, sizeof(buf), "feels %.0f\xc2\xb0 \xc2\xb7 %s",
           weatherData.temp_feels, describe_weather(weatherData.code_now));
  lv_label_set_text(s4.lbl_descriptor, buf);

  lv_label_set_text(s4.lbl_location, weather_location);

  static const char *DOW[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
  time_t now_t = time(nullptr);

  for (int i = 0; i < 3; i++) {
    lv_obj_clear_flag(s4.daily_icon[i], LV_OBJ_FLAG_HIDDEN);
    lv_image_set_src(s4.daily_icon[i], choose_icon(weatherData.daily[i].weather_code, true));

    if (i == 0) {
      lv_label_set_text(s4.daily_day[i], "Today");
    } else {
      time_t day_t = now_t + (time_t)i * 86400;
      struct tm t;
      localtime_r(&day_t, &t);
      lv_label_set_text(s4.daily_day[i], DOW[t.tm_wday]);
    }

    snprintf(buf, sizeof(buf), "%.0f\xc2\xb0 / %.0f\xc2\xb0",
             weatherData.daily[i].temp_max, weatherData.daily[i].temp_min);
    lv_label_set_text(s4.daily_range[i], buf);
  }
}

static void apply_leak_to(screen_chrome_t *ch, bool leak) {
  if (leak) {
    lv_obj_clear_flag(ch->ribbon, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(ch->content, LV_ALIGN_TOP_MID, 0, 36 + 28);
    lv_obj_set_size(ch->content, SCREEN_WIDTH, SCREEN_HEIGHT - 36 - 28 - 28);
  } else {
    lv_obj_add_flag(ch->ribbon, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(ch->content, LV_ALIGN_TOP_MID, 0, 36);
    lv_obj_set_size(ch->content, SCREEN_WIDTH, SCREEN_HEIGHT - 36 - 28);
  }
}

static void apply_leak_state() {
  bool leak = oilTankData.leak_detected == "y";
  apply_leak_to(&s1.chrome, leak);
  apply_leak_to(&s2.chrome, leak);
  apply_leak_to(&s3.chrome, leak);
  apply_leak_to(&s4.chrome, leak);
}

void update_oiltank_ui() {
  apply_leak_state();
  update_screen1();
  update_screen2();
  update_screen3();
}

static void format_age(char *out, size_t cap, unsigned long age_ms) {
  if (last_mqtt_msg_ms == 0) { snprintf(out, cap, "--"); return; }
  unsigned long s = age_ms / 1000;
  if (s < 60)        snprintf(out, cap, "%lus ago", s);
  else if (s < 3600) snprintf(out, cap, "%lum ago", s / 60);
  else               snprintf(out, cap, "%luh ago", s / 3600);
}

static lv_color_t age_color(unsigned long age_ms) {
  if (last_mqtt_msg_ms == 0)         return lv_color_hex(0x88a0c0);
  if (age_ms > 60UL * 60UL * 1000UL) return lv_color_hex(0xff5555); // > 60 min: red
  if (age_ms > 10UL * 60UL * 1000UL) return lv_color_hex(0xffb74d); // > 10 min: amber
  return lv_color_hex(0x88a0c0);
}

static lv_color_t mqtt_dot_color() {
  switch (mqtt_state) {
    case MQTT_UP:           return lv_color_hex(0x4ade80); // green
    case MQTT_RECONNECTING: return lv_color_hex(0xffb74d); // amber
    default:                return lv_color_hex(0xff5555); // red
  }
}

static void update_one_status_bar(status_bar_widgets_t *sb) {
  // Clock
  time_t now = time(nullptr);
  if (now > 8 * 3600 * 2) {
    struct tm t;
    localtime_r(&now, &t);
    char clock_buf[8]; snprintf(clock_buf, sizeof(clock_buf), "%02d:%02d", t.tm_hour, t.tm_min);
    lv_label_set_text(sb->lbl_clock, clock_buf);
  } else {
    lv_label_set_text(sb->lbl_clock, "");
  }
  // MQTT dot
  lv_obj_set_style_bg_color(sb->dot_mqtt, mqtt_dot_color(), LV_PART_MAIN);
  // WiFi icon dim if WiFi down
  bool wifi_ok = (WiFi.status() == WL_CONNECTED);
  lv_obj_set_style_text_color(sb->icon_wifi,
    lv_color_hex(wifi_ok ? 0x88a0c0 : 0x554455), LV_PART_MAIN);
  // Age
  char age_buf[16];
  unsigned long age = millis() - last_mqtt_msg_ms;
  format_age(age_buf, sizeof(age_buf), age);
  lv_label_set_text(sb->lbl_age, age_buf);
  lv_obj_set_style_text_color(sb->lbl_age, age_color(age), LV_PART_MAIN);
}

static void update_status_bars_cb(lv_timer_t *t) {
  update_one_status_bar(&s1.chrome.sb);
  update_one_status_bar(&s2.chrome.sb);
  update_one_status_bar(&s3.chrome.sb);
  update_one_status_bar(&s4.chrome.sb);
}

static void parse_level(JsonDocument &doc) {
  if (!doc["date"].isNull()) oilTankData.date = doc["date"].as<String>();
  if (!doc["id"].isNull()) oilTankData.id = doc["id"].as<int>();
  if (!doc["temperature"].isNull()) oilTankData.temperature = doc["temperature"].as<float>();
  if (!doc["litres_remaining"].isNull()) oilTankData.litres_remaining = doc["litres_remaining"].as<float>();
  if (!doc["litres_used_since_last"].isNull()) oilTankData.litres_used_since_last = doc["litres_used_since_last"].as<float>();
  if (!doc["percentage_remaining"].isNull()) oilTankData.percentage_remaining = doc["percentage_remaining"].as<float>();
  if (!doc["oil_depth_cm"].isNull()) oilTankData.oil_depth_cm = doc["oil_depth_cm"].as<float>();
  if (!doc["air_gap_cm"].isNull()) oilTankData.air_gap_cm = doc["air_gap_cm"].as<float>();
  if (!doc["current_ppl"].isNull()) oilTankData.current_ppl = doc["current_ppl"].as<float>();
  if (!doc["cost_used"].isNull()) oilTankData.cost_used = doc["cost_used"].as<float>();
  if (!doc["cost_to_fill"].isNull()) {
    if (doc["cost_to_fill"].is<const char*>()) {
      oilTankData.cost_to_fill = atof(doc["cost_to_fill"].as<const char*>());
    } else {
      oilTankData.cost_to_fill = doc["cost_to_fill"].as<float>();
    }
  }
  if (!doc["heating_degree_days"].isNull()) oilTankData.heating_degree_days = doc["heating_degree_days"].as<int>();
  if (!doc["seasonal_efficiency"].isNull()) oilTankData.seasonal_efficiency = doc["seasonal_efficiency"].as<float>();
  if (!doc["refill_detected"].isNull()) oilTankData.refill_detected = doc["refill_detected"].as<String>();
  if (!doc["leak_detected"].isNull()) oilTankData.leak_detected = doc["leak_detected"].as<String>();
  if (!doc["raw_flags"].isNull()) oilTankData.raw_flags = doc["raw_flags"].as<int>();
  if (!doc["litres_to_order"].isNull()) oilTankData.litres_to_order = doc["litres_to_order"].as<float>();
  if (!doc["bars_remaining"].isNull()) oilTankData.bars_remaining = doc["bars_remaining"].as<int>();
}

static void parse_analysis(JsonDocument &doc) {
  if (!doc["estimated_days_remaining"].isNull())
    oilTankAnalysis.estimated_days_remaining = doc["estimated_days_remaining"].as<float>();
  if (!doc["estimated_empty_date"].isNull())
    oilTankAnalysis.estimated_empty_date = doc["estimated_empty_date"].as<String>();
  if (!doc["total_consumption_since_refill"].isNull())
    oilTankAnalysis.total_consumption_since_refill = doc["total_consumption_since_refill"].as<float>();
  if (!doc["days_since_refill"].isNull())
    oilTankAnalysis.days_since_refill = doc["days_since_refill"].as<int>();
  if (!doc["avg_daily_consumption_l"].isNull())
    oilTankAnalysis.avg_daily_consumption_l = doc["avg_daily_consumption_l"].as<float>();
  if (!doc["estimated_daily_hot_water_consumption_l"].isNull())
    oilTankAnalysis.estimated_daily_hot_water_consumption_l = doc["estimated_daily_hot_water_consumption_l"].as<float>();
  if (!doc["estimated_daily_heating_consumption_l"].isNull())
    oilTankAnalysis.estimated_daily_heating_consumption_l = doc["estimated_daily_heating_consumption_l"].as<float>();
}

static void parse_cost(JsonDocument &doc) {
  if (!doc["avg_daily_cost"].isNull())   oilTankCost.avg_daily_cost   = doc["avg_daily_cost"].as<float>();
  if (!doc["avg_weekly_cost"].isNull())  oilTankCost.avg_weekly_cost  = doc["avg_weekly_cost"].as<float>();
  if (!doc["avg_monthly_cost"].isNull()) oilTankCost.avg_monthly_cost = doc["avg_monthly_cost"].as<float>();
  if (!doc["avg_annual_cost"].isNull())  oilTankCost.avg_annual_cost  = doc["avg_annual_cost"].as<float>();
  if (!doc["latest_refill_amount"].isNull()) oilTankCost.latest_refill_amount = doc["latest_refill_amount"].as<float>();
  if (!doc["latest_refill_cost"].isNull())   oilTankCost.latest_refill_cost   = doc["latest_refill_cost"].as<float>();
  if (!doc["latest_refill_ppl"].isNull())    oilTankCost.latest_refill_ppl    = doc["latest_refill_ppl"].as<float>();
}

void mqtt_callback(char* topic, byte* payload, unsigned int length) {
  Serial.print("MQTT message on '"); Serial.print(topic);
  Serial.print("' ("); Serial.print(length); Serial.println(" bytes)");

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload, length);
  if (err) {
    Serial.print("deserializeJson() failed: ");
    Serial.println(err.c_str());
    return;
  }

  if (strcmp(topic, "oiltank/level") == 0) {
    parse_level(doc);
  } else if (strcmp(topic, "oiltank/analysis") == 0) {
    parse_analysis(doc);
  } else if (strcmp(topic, "oiltank/cost_analysis") == 0) {
    parse_cost(doc);
  } else {
    Serial.println("Unknown topic, ignoring.");
    return;
  }

  last_mqtt_msg_ms = millis();
  update_oiltank_ui();
}

void show_setup_screen() {
  lv_obj_clean(lv_scr_act());
  lv_obj_t *label = lv_label_create(lv_scr_act());
  lv_label_set_text(label, "Setup Mode\nConnect to WiFi:\nKeroTrack\nand visit 192.168.4.1");
  lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);
}

void show_saved_screen() {
  lv_obj_clean(lv_scr_act());
  lv_obj_t *label = lv_label_create(lv_scr_act());
  lv_label_set_text(label, "WiFi/MQTT info saved!\nRebooting...");
  lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);
  lv_timer_handler(); // Force LVGL to update display
  delay(2500); // Wait 2.5 seconds
}

void show_mqtt_status(const char* msg) {
  lv_obj_clean(lv_scr_act());
  lv_obj_t *label = lv_label_create(lv_scr_act());
  lv_label_set_text(label, msg);
  lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);
  lv_timer_handler();
}

void show_setup_complete_screen() {
  lv_obj_clean(lv_scr_act());
  lv_obj_t *label = lv_label_create(lv_scr_act());
  lv_label_set_text(label, "Setup complete.\nPlease power off and on.");
  lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);
  lv_timer_handler();
  while (1) delay(100); // Halt here
}

static uint32_t hold_for(int screen) {
  switch (screen) {
    case 1: return SCREEN1_HOLD_MS;
    case 2: return SCREEN2_HOLD_MS;
    case 3: return SCREEN3_HOLD_MS;
    default: return SCREEN4_HOLD_MS;
  }
}

static void load_screen(int n) {
  current_screen = n;
  switch (n) {
    case 1: lv_scr_load(screen1); break;
    case 2: lv_scr_load(screen2); break;
    case 3: lv_scr_load(screen3); break;
    default: n = 4; lv_scr_load(screen4); break;
  }
  if (auto_switch_timer) {
    lv_timer_set_period(auto_switch_timer, hold_for(n));
    lv_timer_reset(auto_switch_timer);
  }
}

void switch_screen() {
  int next = current_screen + 1;
  if (next > 4) next = 1;
  load_screen(next);
}

void auto_switch_cb(lv_timer_t *timer) {
  switch_screen();
}

void show_boot_screen() {
  lv_obj_clean(lv_scr_act());
  lv_obj_t *title = lv_label_create(lv_scr_act());
  lv_label_set_text(title, "KeroTrack");
  lv_obj_set_style_text_font(title, &lv_font_montserrat_20, LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_text_color(title, lv_color_hex(0xeeeeee), LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_align(title, LV_ALIGN_CENTER, 0, -30);

  lv_obj_t *subtitle = lv_label_create(lv_scr_act());
  lv_label_set_text(subtitle, "Display");
  lv_obj_set_style_text_font(subtitle, &lv_font_montserrat_20, LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_text_color(subtitle, lv_color_hex(0xeeeeee), LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_align(subtitle, LV_ALIGN_CENTER, 0, 20);
}
