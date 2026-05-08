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

// Oil tank UI objects
static lv_obj_t *screen1 = nullptr;
static lv_obj_t *screen2 = nullptr;
static int current_screen = 1;
static lv_timer_t *auto_switch_timer = nullptr;

// Debounce for screen switching
static unsigned long last_screen_switch = 0;
const unsigned long screen_switch_debounce = 400; // ms

// Analysis data structure
struct OilTankAnalysis {
    float estimated_days_remaining = 0;
};
OilTankAnalysis oilTankAnalysis;

// Forward declarations
void create_screen1();
void create_screen2();
void switch_screen();
void auto_switch_cb(lv_timer_t *timer);
void bar_update(lv_obj_t *bars_container, int bars_remaining);
void show_boot_screen();
void update_oiltank_ui();

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

void create_screen1() {
  if (screen1) lv_obj_clean(screen1);
  else screen1 = lv_obj_create(NULL);
  // Dark theme background
  lv_obj_set_style_bg_color(screen1, lv_color_hex(0x181c24), LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_bg_grad_color(screen1, lv_color_hex(0x232a34), LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_bg_grad_dir(screen1, LV_GRAD_DIR_VER, LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_bg_opa(screen1, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);

  // Title at the top
  lv_obj_t *title1 = lv_label_create(screen1);
  lv_label_set_text(title1, "KeroTrack");
  lv_obj_set_style_text_font(title1, &lv_font_montserrat_20, LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_text_color(title1, lv_color_hex(0xeeeeee), LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_align(title1, LV_ALIGN_TOP_MID, 0, 8);

  // Bar on the left
  lv_obj_t *bars1 = lv_obj_create(screen1);
  lv_obj_set_size(bars1, 40, 200);
  lv_obj_align(bars1, LV_ALIGN_LEFT_MID, 10, 0);
  lv_obj_set_style_bg_opa(bars1, LV_OPA_TRANSP, LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_border_width(bars1, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
  for (int i = 0; i < 10; i++) {
    lv_obj_t *bar = lv_obj_create(bars1);
    lv_obj_set_size(bar, 32, 16);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, i * 18);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x333a44), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(bar, 4, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_user_data(bar, (void*)(intptr_t)i);
  }
  bar_update(bars1, oilTankData.bars_remaining);

  // Stats group - align with top of bar
  int stat_x = 70;
  int stat_y = 60; // align with top of bar
  int stat_spacing = 45;
  int line = 0;

  // Litres remaining
  lv_obj_t *lbl_litres1 = lv_label_create(screen1);
  lv_label_set_text_fmt(lbl_litres1, "Litres: %.1f", oilTankData.litres_remaining);
  lv_obj_set_style_text_font(lbl_litres1, &lv_font_montserrat_20, LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_text_color(lbl_litres1, lv_color_hex(0xeeeeee), LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_align(lbl_litres1, LV_ALIGN_TOP_LEFT, stat_x, stat_y + stat_spacing * line++);

  // Percentage remaining
  lv_obj_t *lbl_percent1 = lv_label_create(screen1);
  lv_label_set_text_fmt(lbl_percent1, "%.0f%% full", oilTankData.percentage_remaining);
  lv_obj_set_style_text_font(lbl_percent1, &lv_font_montserrat_20, LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_text_color(lbl_percent1, lv_color_hex(0xeeeeee), LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_align(lbl_percent1, LV_ALIGN_TOP_LEFT, stat_x, stat_y + stat_spacing * line++);

  // Temperature
  lv_obj_t *lbl_temp1 = lv_label_create(screen1);
  lv_label_set_text_fmt(lbl_temp1, "Temp: %.1f°C", oilTankData.temperature);
  lv_obj_set_style_text_font(lbl_temp1, &lv_font_montserrat_20, LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_text_color(lbl_temp1, lv_color_hex(0xeeeeee), LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_align(lbl_temp1, LV_ALIGN_TOP_LEFT, stat_x, stat_y + stat_spacing * line++);

  // Days left
  lv_obj_t *lbl_daysleft1 = lv_label_create(screen1);
  lv_label_set_text_fmt(lbl_daysleft1, "Days left: %.0f", oilTankAnalysis.estimated_days_remaining);
  lv_obj_set_style_text_font(lbl_daysleft1, &lv_font_montserrat_20, LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_text_color(lbl_daysleft1, lv_color_hex(0xeeeeee), LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_align(lbl_daysleft1, LV_ALIGN_TOP_LEFT, stat_x, stat_y + stat_spacing * line++);
}

void create_screen2() {
  if (screen2) lv_obj_clean(screen2);
  else screen2 = lv_obj_create(NULL);
  // Dark theme background
  lv_obj_set_style_bg_color(screen2, lv_color_hex(0x181c24), LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_bg_grad_color(screen2, lv_color_hex(0x232a34), LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_bg_grad_dir(screen2, LV_GRAD_DIR_VER, LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_bg_opa(screen2, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);

  // Title at the top
  lv_obj_t *title2 = lv_label_create(screen2);
  lv_label_set_text(title2, "KeroTrack");
  lv_obj_set_style_text_font(title2, &lv_font_montserrat_20, LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_text_color(title2, lv_color_hex(0xeeeeee), LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_align(title2, LV_ALIGN_TOP_MID, 0, 8);

  // Bar on the left
  lv_obj_t *bars2 = lv_obj_create(screen2);
  lv_obj_set_size(bars2, 40, 200);
  lv_obj_align(bars2, LV_ALIGN_LEFT_MID, 10, 0);
  lv_obj_set_style_bg_opa(bars2, LV_OPA_TRANSP, LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_border_width(bars2, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
  for (int i = 0; i < 10; i++) {
    lv_obj_t *bar = lv_obj_create(bars2);
    lv_obj_set_size(bar, 32, 16);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, i * 18);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x333a44), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(bar, 4, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_user_data(bar, (void*)(intptr_t)i);
  }
  bar_update(bars2, oilTankData.bars_remaining);

  // Stats group - align with top of bar
  int stat_x = 70;
  int stat_y = 60; // align with top of bar
  int stat_spacing = 45;
  int line = 0;

  // Subtitle 'To Order'
  lv_obj_t *lbl_order = lv_label_create(screen2);
  lv_label_set_text(lbl_order, "To Order");
  lv_obj_set_style_text_font(lbl_order, &lv_font_montserrat_20, LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_text_color(lbl_order, lv_color_hex(0xeeeeee), LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_align(lbl_order, LV_ALIGN_TOP_LEFT, stat_x, stat_y + stat_spacing * line++);

  // Litres
  lv_obj_t *lbl_to_order2 = lv_label_create(screen2);
  lv_label_set_text_fmt(lbl_to_order2, "Litres: %.1f", oilTankData.litres_to_order);
  lv_obj_set_style_text_font(lbl_to_order2, &lv_font_montserrat_20, LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_text_color(lbl_to_order2, lv_color_hex(0xeeeeee), LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_align(lbl_to_order2, LV_ALIGN_TOP_LEFT, stat_x, stat_y + stat_spacing * line++);

  // PPL
  lv_obj_t *lbl_ppl2 = lv_label_create(screen2);
  lv_label_set_text_fmt(lbl_ppl2, "PPL: %.2f", oilTankData.current_ppl);
  lv_obj_set_style_text_font(lbl_ppl2, &lv_font_montserrat_20, LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_text_color(lbl_ppl2, lv_color_hex(0xeeeeee), LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_align(lbl_ppl2, LV_ALIGN_TOP_LEFT, stat_x, stat_y + stat_spacing * line++);

  // To Fill (Cost)
  lv_obj_t *lbl_cost2 = lv_label_create(screen2);
  lv_label_set_text_fmt(lbl_cost2, "Cost: %.2f", oilTankData.cost_to_fill);
  lv_obj_set_style_text_font(lbl_cost2, &lv_font_montserrat_20, LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_text_color(lbl_cost2, lv_color_hex(0xeeeeee), LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_align(lbl_cost2, LV_ALIGN_TOP_LEFT, stat_x, stat_y + stat_spacing * line++);
}

void bar_update(lv_obj_t *bars_container, int bars_remaining) {
    uint32_t child_cnt = lv_obj_get_child_cnt(bars_container);
    for (uint32_t i = 0; i < child_cnt; i++) {
    lv_obj_t *bar = lv_obj_get_child(bars_container, 9 - i); // fill from bottom up
    // Always show a border
    lv_obj_set_style_border_width(bar, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(bar, lv_color_hex(0x444950), LV_PART_MAIN | LV_STATE_DEFAULT); // medium gray border
    if (i < bars_remaining) {
      // Color gradient: green (top), yellow (mid), red (low)
      uint32_t color = 0x1e7d3a; // dark green
      if (bars_remaining <= 3) color = 0x8b2323; // dark red
      else if (bars_remaining <= 6) color = 0x8b6f23; // dark yellow
      // Optionally, make the lowest filled bar more red/yellow
      if (bars_remaining <= 3 && i == 0) color = 0xff5555; // highlight lowest bar
      lv_obj_set_style_bg_color(bar, lv_color_hex(color), LV_PART_MAIN | LV_STATE_DEFAULT);
      lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
      } else {
      lv_obj_set_style_bg_color(bar, lv_color_hex(0x181c24), LV_PART_MAIN | LV_STATE_DEFAULT); // very dark gray
      lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    }
  }
}

// Update both screens when new data arrives
void update_oiltank_ui() {
  if (screen1) {
    // Update bar
    lv_obj_t *bars1 = lv_obj_get_child(screen1, 1);
    bar_update(bars1, oilTankData.bars_remaining);
  // Update labels
    lv_obj_t *lbl_litres1 = lv_obj_get_child(screen1, 2);
    lv_obj_t *lbl_percent1 = lv_obj_get_child(screen1, 3);
    lv_obj_t *lbl_temp1 = lv_obj_get_child(screen1, 4);
    lv_obj_t *lbl_daysleft1 = lv_obj_get_child(screen1, 5);
    lv_label_set_text_fmt(lbl_litres1, "Litres: %.1f", oilTankData.litres_remaining);
    lv_label_set_text_fmt(lbl_percent1, "%.0f%% full", oilTankData.percentage_remaining);
    lv_label_set_text_fmt(lbl_daysleft1, "Days left: %.0f", oilTankAnalysis.estimated_days_remaining);
    lv_label_set_text_fmt(lbl_temp1, "Temp: %.1f°C", oilTankData.temperature);
  }
  if (screen2) {
    // Update bar
    lv_obj_t *bars2 = lv_obj_get_child(screen2, 1);
    bar_update(bars2, oilTankData.bars_remaining);
    // Update labels (skip title, bar, subtitle)
    lv_obj_t *lbl_to_order2 = lv_obj_get_child(screen2, 3);
    lv_obj_t *lbl_ppl2 = lv_obj_get_child(screen2, 4);
    lv_obj_t *lbl_cost2 = lv_obj_get_child(screen2, 5);
    lv_label_set_text_fmt(lbl_to_order2, "Litres: %.1f", oilTankData.litres_to_order);
    lv_label_set_text_fmt(lbl_ppl2, "PPL: %.2f", oilTankData.current_ppl);
    lv_label_set_text_fmt(lbl_cost2, "Cost: %.2f", oilTankData.cost_to_fill);
  }
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
  // Additional analysis fields are populated in Task 4.
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
