/**
 * Gemini Weather Station
 * A standalone, landscape-first weather display for ESP32 and ILI9341 TFT.
 * 
 * REQUIRED LIBRARIES:
 * - TFT_eSPI by Bodmer
 * - LVGL by lvgl
 * - ArduinoJson by Benoit Blanchon
 * - WiFiManager by tzapu
 * 
 * HARDWARE WIRING:
 * - Ensure pins in the "Hardware Pins" section below match your ESP32 board.
 * - Also ensure your TFT_eSPI library's User_Setup.h is configured for your board.
 */

// ------------------- INCLUDES -------------------
#include <Arduino.h>
#include <lvgl.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h> // Or your specific touch driver
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <WiFiManager.h>
#include <time.h>

// ------------------- HARDWARE PINS -------------------
#define XPT2046_IRQ 36
#define XPT2046_MOSI 32
#define XPT2046_MISO 39
#define XPT2046_CLK 25
#define XPT2046_CS 33
#define LCD_BACKLIGHT_PIN 21

// ------------------- DISPLAY CONFIG -------------------
#define SCREEN_WIDTH 320
#define SCREEN_HEIGHT 240
static uint32_t draw_buf[SCREEN_WIDTH * 10]; // LVGL draw buffer

// ------------------- WEATHER CONFIG -------------------
#define LATITUDE "51.5074"      // Your latitude (default: London)
#define LONGITUDE "-0.1278"     // Your longitude (default: London)
#define LOCATION_NAME "London"  // A name for your location
#define UPDATE_INTERVAL_SECS 900 // 15 minutes

// ------------------- HARDWARE OBJECTS -------------------
TFT_eSPI tft = TFT_eSPI();
SPIClass touchscreenSPI = SPIClass(VSPI);
XPT2046_Touchscreen touchscreen(XPT2046_CS, XPT2046_IRQ);

// ------------------- UI OBJECTS (GLOBALS) -------------------
static lv_obj_t *lbl_location;
static lv_obj_t *lbl_time;
static lv_obj_t *img_main_icon;
static lv_obj_t *lbl_main_temp;
static lv_obj_t *lbl_feels_like;

// Forecast UI Objects (Arrays)
static lv_obj_t *lbl_day[7];
static lv_obj_t *img_day_icon[7];
static lv_obj_t *lbl_day_high[7];
static lv_obj_t *lbl_day_low[7];

// ------------------- IMAGE ASSET DECLARATIONS -------------------
// This tells the compiler that the image data exists in the .c files you added.
LV_IMG_DECLARE(image_sunny); LV_IMG_DECLARE(image_clear_night); LV_IMG_DECLARE(image_mostly_sunny);
LV_IMG_DECLARE(image_mostly_clear_night); LV_IMG_DECLARE(image_partly_cloudy); LV_IMG_DECLARE(image_partly_cloudy_night);
LV_IMG_DECLARE(image_cloudy); LV_IMG_DECLARE(image_haze_fog_dust_smoke); LV_IMG_DECLARE(image_drizzle);
LV_IMG_DECLARE(image_sleet_hail); LV_IMG_DECLARE(image_scattered_showers_day); LV_IMG_DECLARE(image_scattered_showers_night);
LV_IMG_DECLARE(image_showers_rain); LV_IMG_DECLARE(image_heavy_rain); LV_IMG_DECLARE(image_wintry_mix_rain_snow);
LV_IMG_DECLARE(image_snow_showers_snow); LV_IMG_DECLARE(image_flurries); LV_IMG_DECLARE(image_heavy_snow);
LV_IMG_DECLARE(image_isolated_scattered_tstorms_day); LV_IMG_DECLARE(image_isolated_scattered_tstorms_night);
LV_IMG_DECLARE(image_strong_tstorms);

// ------------------- FORWARD DECLARATIONS -------------------
void update_weather_data();
void create_gui();
void update_gui(JsonDocument& doc);
const lv_img_dsc_t* get_weather_icon(int code, int is_day);
int day_of_week(int y, int m, int d);
void apModeCallback(WiFiManager *myWiFiManager);


// LVGL Display flush callback
void my_disp_flush(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
  uint32_t w = (area->x2 - area->x1 + 1);
  uint32_t h = (area->y2 - area->y1 + 1);
  tft.startWrite();
  tft.setAddrWindow(area->x1, area->y1, w, h);
  tft.pushColors((uint16_t *)px_map, w * h, true);
  tft.endWrite();
  lv_display_flush_ready(disp);
}

// LVGL Touch read callback
void my_touchpad_read(lv_indev_t *indev, lv_indev_data_t *data) {
  if (touchscreen.tirqTouched() && touchscreen.touched()) {
    TS_Point p = touchscreen.getPoint();
    data->point.x = map(p.y, 200, 3700, 0, SCREEN_WIDTH - 1);
    data->point.y = map(p.x, 200, 3800, SCREEN_HEIGHT - 1, 0);
    data->state = LV_INDEV_STATE_PR;
  } else {
    data->state = LV_INDEV_STATE_REL;
  }
}

// Clock update timer callback
static void clock_timer_cb(lv_timer_t *timer) {
    struct tm timeinfo;
    if (getLocalTime(&timeinfo)) {
        lv_label_set_text_fmt(lbl_time, "%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);
    }
}

// ----------------------------- SETUP -----------------------------
void setup() {
  Serial.begin(115200);
  pinMode(LCD_BACKLIGHT_PIN, OUTPUT);
  digitalWrite(LCD_BACKLIGHT_PIN, HIGH);

  // --- LVGL & Driver Initialization ---
  lv_init();
  tft.init();
  touchscreenSPI.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS);
  touchscreen.begin(touchscreenSPI);

  // --- CRITICAL ROTATION SETUP ---
  // Create LVGL display driver BEFORE setting rotation
  lv_display_t *disp = lv_display_create(SCREEN_WIDTH, SCREEN_HEIGHT);
  lv_display_set_flush_cb(disp, my_disp_flush);
  lv_display_set_buffers(disp, draw_buf, NULL, sizeof(draw_buf), LV_DISPLAY_RENDER_MODE_PARTIAL);

  // MANUALLY force the hardware rotation for stubborn displays
  tft.writecommand(TFT_MADCTL);
  tft.writedata(0x28); // This is the command for 270-degree landscape with BGR color filter

  // Set touch rotation to match
  touchscreen.setRotation(3);

  // Initialize LVGL touch input driver
  lv_indev_t *indev = lv_indev_create();
  lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(indev, my_touchpad_read);

  // --- WiFi Setup ---
  WiFiManager wm;
  wm.setAPCallback(apModeCallback);
  if (!wm.autoConnect("GeminiWeather-Setup")) {
    Serial.println("Failed to connect and hit timeout");
    ESP.restart();
  }
  Serial.println("Connected to WiFi!");

  // --- Create the User Interface ---
  create_gui();

  // --- Initial Data Fetch & Timers ---
  update_weather_data(); // Get first weather data
  lv_timer_create(clock_timer_cb, 1000, NULL); // Timer to update the clock every second
}


// ------------------------------ LOOP ------------------------------
void loop() {
  lv_timer_handler(); // Let LVGL handle its tasks

  // Non-blocking timer to update weather data periodically
  static uint32_t last_update = 0;
  if (millis() - last_update > UPDATE_INTERVAL_SECS * 1000) {
    last_update = millis();
    update_weather_data();
  }
  delay(5);
}


// ------------------- UI CREATION FUNCTION -------------------
void create_gui() {
  lv_obj_t *scr = lv_scr_act();
  lv_obj_set_style_bg_color(scr, lv_color_hex(0x0d3b66), 0);

  // --- LEFT PANEL (Current Weather) ---
  lv_obj_t *left_panel = lv_obj_create(scr);
  lv_obj_set_size(left_panel, 150, SCREEN_HEIGHT);
  lv_obj_align(left_panel, LV_ALIGN_LEFT_MID, 0, 0);
  lv_obj_set_style_bg_opa(left_panel, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(left_panel, 0, 0);

  lbl_location = lv_label_create(left_panel);
  lv_label_set_text(lbl_location, LOCATION_NAME);
  lv_obj_set_style_text_color(lbl_location, lv_color_white(), 0);
  lv_obj_set_style_text_font(lbl_location, &lv_font_montserrat_16, 0);
  lv_obj_align(lbl_location, LV_ALIGN_TOP_MID, 0, 10);
  
  lbl_time = lv_label_create(left_panel);
  lv_label_set_text(lbl_time, "00:00");
  lv_obj_set_style_text_color(lbl_time, lv_color_hex(0xf4d35e), 0);
  lv_obj_set_style_text_font(lbl_time, &lv_font_montserrat_20, 0);
  lv_obj_align(lbl_time, LV_ALIGN_TOP_MID, 0, 35);

  img_main_icon = lv_img_create(left_panel);
  lv_img_set_src(img_main_icon, &image_cloudy); // Placeholder
  lv_obj_align(img_main_icon, LV_ALIGN_CENTER, 0, -15);
  lv_img_set_zoom(img_main_icon, 320); // Make it a bit bigger

  lbl_main_temp = lv_label_create(left_panel);
  lv_label_set_text(lbl_main_temp, "--°");
  lv_obj_set_style_text_color(lbl_main_temp, lv_color_white(), 0);
  lv_obj_set_style_text_font(lbl_main_temp, &lv_font_montserrat_42, 0);
  lv_obj_align(lbl_main_temp, LV_ALIGN_BOTTOM_MID, -5, -35);

  lbl_feels_like = lv_label_create(left_panel);
  lv_label_set_text(lbl_feels_like, "Feels: --°");
  lv_obj_set_style_text_color(lbl_feels_like, lv_color_hex(0xcccccc), 0);
  lv_obj_set_style_text_font(lbl_feels_like, &lv_font_montserrat_14, 0);
  lv_obj_align(lbl_feels_like, LV_ALIGN_BOTTOM_MID, 0, -10);

  // --- RIGHT PANEL (Forecast) ---
  lv_obj_t *right_panel = lv_obj_create(scr);
  lv_obj_set_size(right_panel, SCREEN_WIDTH - 150, SCREEN_HEIGHT);
  lv_obj_align(right_panel, LV_ALIGN_RIGHT_MID, 0, 0);
  lv_obj_set_style_bg_color(right_panel, lv_color_hex(0x2a628f), 0);
  lv_obj_set_style_border_width(right_panel, 0, 0);
  lv_obj_set_style_pad_all(right_panel, 5, 0);
  lv_obj_set_flex_flow(right_panel, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(right_panel, LV_FLEX_ALIGN_SPACE_AROUND, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  for (int i = 0; i < 7; i++) {
    lv_obj_t *row = lv_obj_create(right_panel);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);

    lbl_day[i] = lv_label_create(row);
    lv_label_set_text(lbl_day[i], "...");
    lv_obj_set_style_text_color(lbl_day[i], lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl_day[i], &lv_font_montserrat_14, 0);
    lv_obj_align(lbl_day[i], LV_ALIGN_LEFT_MID, 0, 0);

    img_day_icon[i] = lv_img_create(row);
    lv_img_set_src(img_day_icon[i], &image_cloudy); // Placeholder icon
    lv_obj_align(img_day_icon[i], LV_ALIGN_CENTER, -10, 0);

    lbl_day_high[i] = lv_label_create(row);
    lv_label_set_text(lbl_day_high[i], "--°");
    lv_obj_set_style_text_color(lbl_day_high[i], lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl_day_high[i], &lv_font_montserrat_14, 0);
    lv_obj_align(lbl_day_high[i], LV_ALIGN_RIGHT_MID, 0, 0);

    lbl_day_low[i] = lv_label_create(row);
    lv_label_set_text(lbl_day_low[i], "--°");
    lv_obj_set_style_text_color(lbl_day_low[i], lv_color_hex(0xbbbbbb), 0);
    lv_obj_set_style_text_font(lbl_day_low[i], &lv_font_montserrat_14, 0);
    lv_obj_align_to(lbl_day_low[i], lbl_day_high[i], LV_ALIGN_OUT_LEFT_MID, -8, 0);
  }
}

// ------------------- WEATHER DATA FUNCTIONS -------------------
void update_weather_data() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi disconnected, skipping weather update.");
    return;
  }

  Serial.println("Fetching new weather data...");
  HTTPClient http;
  String url = String("http://api.open-meteo.com/v1/forecast?latitude=") + LATITUDE +
               "&longitude=" + LONGITUDE +
               "&current=temperature_2m,apparent_temperature,is_day,weather_code" +
               "&daily=weather_code,temperature_2m_max,temperature_2m_min&timezone=auto";

  http.begin(url);
  int httpCode = http.GET();

  if (httpCode == HTTP_CODE_OK) {
    DynamicJsonDocument doc(8192); // Allocate memory for JSON
    DeserializationError error = deserializeJson(doc, http.getStream());
    if (error) {
      Serial.print(F("deserializeJson() failed: "));
      Serial.println(error.c_str());
    } else {
      Serial.println("Weather data parsed successfully.");
      configTime(doc["utc_offset_seconds"], 0, "pool.ntp.org"); // Sync NTP
      update_gui(doc); // Update the screen with new data
    }
  } else {
    Serial.printf("HTTP GET failed, error: %s\n", http.errorToString(httpCode).c_str());
  }
  http.end();
}

void update_gui(JsonDocument& doc) {
  // --- Update Current Weather (Left Panel) ---
  float temp_now = doc["current"]["temperature_2m"];
  float feels_now = doc["current"]["apparent_temperature"];
  int code_now = doc["current"]["weather_code"];
  int is_day_now = doc["current"]["is_day"];
  
  lv_label_set_text_fmt(lbl_main_temp, "%.0f°", temp_now);
  lv_label_set_text_fmt(lbl_feels_like, "Feels: %.0f°", feels_now);
  lv_img_set_src(img_main_icon, get_weather_icon(code_now, is_day_now));

  // --- Update Forecast (Right Panel) ---
  JsonArray daily_time = doc["daily"]["time"];
  JsonArray daily_code = doc["daily"]["weather_code"];
  JsonArray daily_max = doc["daily"]["temperature_2m_max"];
  JsonArray daily_min = doc["daily"]["temperature_2m_min"];

  const char* weekdays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};

  for(int i = 0; i < 7; i++) {
    const char *date_str = daily_time[i];
    int year = atoi(date_str);
    int month = atoi(date_str + 5);
    int day = atoi(date_str + 8);
    int dow = day_of_week(year, month, day);

    lv_label_set_text(lbl_day[i], (i == 0) ? "Today" : weekdays[dow]);
    lv_label_set_text_fmt(lbl_day_high[i], "%.0f°", daily_max[i].as<float>());
    lv_label_set_text_fmt(lbl_day_low[i], "%.0f°", daily_min[i].as<float>());
    lv_img_set_src(img_day_icon[i], get_weather_icon(daily_code[i], 1)); // Forecast is always 'day'
  }
}

// ------------------- HELPER FUNCTIONS -------------------

// Callback for WiFiManager
void apModeCallback(WiFiManager *myWiFiManager) {
  lv_obj_t* scr = lv_scr_act();
  lv_obj_clean(scr);
  lv_obj_t* lbl = lv_label_create(scr);
  lv_label_set_text_fmt(lbl, "Setup WiFi\nConnect to AP: %s\nGo to 192.168.4.1", myWiFiManager->getConfigPortalSSID().c_str());
  lv_obj_center(lbl);
  Serial.println("Entered AP mode");
}

// Get the correct image pointer based on WMO weather code
const lv_img_dsc_t* get_weather_icon(int code, int is_day) {
  switch (code) {
    case  0: return is_day ? &image_sunny : &image_clear_night;
    case  1: return is_day ? &image_mostly_sunny : &image_mostly_clear_night;
    case  2: return is_day ? &image_partly_cloudy : &image_partly_cloudy_night;
    case  3: return &image_cloudy;
    case 45: case 48: return &image_haze_fog_dust_smoke;
    case 51: case 53: case 55: return &image_drizzle;
    case 56: case 57: return &image_sleet_hail;
    case 61: return is_day ? &image_scattered_showers_day : &image_scattered_showers_night;
    case 63: return &image_showers_rain;
    case 65: return &image_heavy_rain;
    case 66: case 67: return &image_wintry_mix_rain_snow;
    case 71: case 73: case 75: case 85: return &image_snow_showers_snow;
    case 77: return &image_flurries;
    case 80: case 81: return is_day ? &image_scattered_showers_day : &image_scattered_showers_night;
    case 82: return &image_heavy_rain;
    case 86: return &image_heavy_snow;
    case 95: return is_day ? &image_isolated_scattered_tstorms_day : &image_isolated_scattered_tstorms_night;
    case 96: case 99: return &image_strong_tstorms;
    default: return &image_cloudy;
  }
}

// Get day of the week (0=Sun, 1=Mon,...)
int day_of_week(int y, int m, int d) {
  static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
  if (m < 3) y -= 1;
  return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}
