#include <Arduino.h>
#include <WiFiManager.h>
#include <ArduinoJson.h>
#include <lvgl.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <Preferences.h>
#include "esp_system.h"
#include <PubSubClient.h>
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

// MQTT reconnect state
enum MqttState { MQTT_DOWN, MQTT_RECONNECTING, MQTT_UP };
static MqttState mqtt_state = MQTT_DOWN;
static unsigned long last_mqtt_attempt_ms = 0;
static unsigned long mqtt_backoff_ms = 1000;
static unsigned long last_mqtt_msg_ms = 0;  // wall-clock of last received message

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

static screen1_widgets_t s1;
static screen2_widgets_t s2;
static screen3_widgets_t s3;

// Oil tank UI objects
static lv_obj_t *screen1 = nullptr;
static lv_obj_t *screen2 = nullptr;
static lv_obj_t *screen3 = nullptr;
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
void switch_screen();
void auto_switch_cb(lv_timer_t *timer);
void show_boot_screen();
void update_oiltank_ui();
static void create_chrome(lv_obj_t *parent, screen_chrome_t *out, const char *dots_str);

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
  prefs.end();
  strncpy(mqtt_broker, broker.c_str(), sizeof(mqtt_broker));
  strncpy(mqtt_user, user.c_str(), sizeof(mqtt_user));
  strncpy(mqtt_pass, pass.c_str(), sizeof(mqtt_pass));
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
  lv_scr_load(screen1);
  current_screen = 1;
  // Set up tap-to-switch with debounce
  lv_obj_add_event_cb(lv_scr_act(), [](lv_event_t *e) {
    unsigned long now = millis();
    if (now - last_screen_switch > screen_switch_debounce) {
      switch_screen();
      last_screen_switch = now;
    }
  }, LV_EVENT_CLICKED, NULL);
  // Set up auto-switch timer (2 minutes)
  auto_switch_timer = lv_timer_create(auto_switch_cb, 120000, NULL);
  lv_timer_create(update_status_bars_cb, 1000, NULL);

  // NTP time sync
  configTime(0, 0, "0.uk.pool.ntp.org", "1.uk.pool.ntp.org");
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

void loop() {
  lv_timer_handler();
  if (mqtt_is_configured) {
    mqttClient.loop();
    mqtt_try_reconnect();
  }
  lv_tick_inc(5);
  delay(5);
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
  lv_obj_add_flag(out->ribbon, LV_OBJ_FLAG_HIDDEN);

  out->ribbon_label = lv_label_create(out->ribbon);
  lv_label_set_text(out->ribbon_label, "LEAK DETECTED");
  lv_obj_set_style_text_font(out->ribbon_label, &lv_font_montserrat_16, LV_PART_MAIN);
  lv_obj_set_style_text_color(out->ribbon_label, lv_color_hex(0xffffff), LV_PART_MAIN);
  lv_obj_align(out->ribbon_label, LV_ALIGN_CENTER, 0, 0);

  // --- Content container (everything between status bar and dots) ---
  out->content = lv_obj_create(parent);
  lv_obj_set_size(out->content, SCREEN_WIDTH, SCREEN_HEIGHT - 36 - 28);
  lv_obj_align(out->content, LV_ALIGN_TOP_MID, 0, 36);
  lv_obj_set_style_bg_opa(out->content, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(out->content, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(out->content, 0, LV_PART_MAIN);
  lv_obj_clear_flag(out->content, LV_OBJ_FLAG_SCROLLABLE);

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
  create_chrome(screen1, &s1.chrome, "*..");
  // Content widgets added in Task 7.
}

void create_screen2() {
  if (screen2) lv_obj_clean(screen2);
  else screen2 = lv_obj_create(NULL);
  create_chrome(screen2, &s2.chrome, ".*.");
  // Content widgets added in Task 8.
}

void create_screen3() {
  if (screen3) lv_obj_clean(screen3);
  else screen3 = lv_obj_create(NULL);
  create_chrome(screen3, &s3.chrome, "..*");
  // Content widgets added in Task 9.
}

void update_oiltank_ui() {
  // Per-screen update functions are added in Tasks 7/8/9.
  // Status bar and ribbon update from a separate timer in Task 6.
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

void switch_screen() {
  if (current_screen == 1) {
    lv_scr_load(screen2);
    current_screen = 2;
  } else {
    lv_scr_load(screen1);
    current_screen = 1;
  }
  last_screen_switch = millis();
  // Re-attach tap event to new screen with debounce
  lv_obj_add_event_cb(lv_scr_act(), [](lv_event_t *e) {
    unsigned long now = millis();
    if (now - last_screen_switch > screen_switch_debounce) {
      switch_screen();
      last_screen_switch = now;
    }
  }, LV_EVENT_CLICKED, NULL);
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
