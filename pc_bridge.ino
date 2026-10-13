/*
  AI_Chatbot_PCBridge
  --------------------

  Required libraries (Arduino Library Manager):
    - "WebSockets" by Markus Sattler (Links2004)
    - "ArduinoJson" by Benoit Blanchon, v7.x   (only for the small state
      messages now - audio itself is raw binary, no JSON/base64)
*/

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <math.h>
#include "lvgl.h"
#include "freertos/stream_buffer.h"

#include "bsp_config.h"
#include "esp32_s3_touch_amoled_1.43c.h"   // unified display + touch + audio codec driver
#include "ui.h"                             // SquareLine-generated LVGL 9.3 face

// ---------------------------------------------------------------------
// pc_bridge.py connection
// ---------------------------------------------------------------------
static char PC_HOST[16] = "";               // discovered automatically on the LAN
static const uint16_t PC_PORT = 8765;
static bool bridgeStarted = false;
static uint32_t lastBridgeDiscoveryMs = 0;

// ---------------------------------------------------------------------
// Audio formats - identical to v2's shared-bus setup, since the codec
// hardware and its constraints haven't changed. pc_bridge.py forwards
// our 16kHz mic audio to Gemini as-is, and Gemini's fixed 24kHz reply
// still needs the same downsample to our 16kHz speaker rate.
// ---------------------------------------------------------------------
#define MIC_SAMPLE_RATE     16000
#define MIC_HW_CHANNELS     2
#define SPK_SAMPLE_RATE     16000
#define SPK_CHANNELS        1
#define BITS_PER_SAMPLE     16

#define CHUNK_MS            30
#define MIC_FRAMES_PER_CHUNK (MIC_SAMPLE_RATE * CHUNK_MS / 1000)
#define MIC_RAW_BYTES        (MIC_FRAMES_PER_CHUNK * MIC_HW_CHANNELS * 2)
#define MIC_MONO_BYTES       (MIC_FRAMES_PER_CHUNK * 2)

static esp_codec_dev_handle_t speaker    = NULL;
static esp_codec_dev_handle_t microphone = NULL;
static uint8_t *micRawBuf  = NULL;
static int16_t *micMonoBuf = NULL;

WebSocketsClient webSocket;
static SemaphoreHandle_t wsMutex;
static volatile bool bridgeConnected = false;
static volatile bool isRecording = false;
static volatile bool isSpeaking  = false;
static volatile bool isWakeListening = false;     // PC heard "Hey Jarvis" and is listening
static volatile bool dropAudio = false;            // interrupted: discard reply audio until a new reply starts
static volatile bool isThinking  = false;          // PC said "thinking" (button released, waiting for reply)
static volatile uint8_t  audioLevel   = 0;         // 0..100, loudness of what the speaker is playing
static volatile uint32_t audioLevelMs = 0;         // millis() when audioLevel was last updated
static TaskHandle_t micTaskHandle = NULL;
static TaskHandle_t playbackTaskHandle = NULL;
static StreamBufferHandle_t audioStreamBuffer = NULL;
#define AUDIO_STREAM_BUFFER_BYTES (32 * 1024)

static bool buttonState    = HIGH;
static bool lastRawReading = HIGH;
static uint32_t lastDebounceMs = 0;
static const uint32_t DEBOUNCE_MS = 40;

// ---------------------------------------------------------------------
// Weather (Open-Meteo, no API key required)
// ---------------------------------------------------------------------
static char weatherCity[48] = "Locating...";
static float weatherLatitude = 0.0f;
static float weatherLongitude = 0.0f;
static bool weatherLocationReady = false;
static const uint32_t WEATHER_REFRESH_MS = 10UL * 60UL * 1000UL;
static uint32_t lastWeatherFetch = 0;
static uint32_t weatherRetryMs = 30000;

// ---------------------------------------------------------------------
// SquareLine screens and settings state
// ---------------------------------------------------------------------
static lv_obj_t *eyeL = NULL;
static lv_obj_t *eyeR = NULL;
static lv_obj_t *faceStatusLabel = NULL;
static lv_obj_t *cityLabel = NULL;
static lv_obj_t *weatherTempLabel = NULL;
static lv_obj_t *weatherDescLabel = NULL;
// Arc reactor face (drawn with LVGL arcs - no images, almost no RAM)
#define REACTOR_SEGS       10
#define REACTOR_PITCH_DEG  36      // 360 / REACTOR_SEGS
#define REACTOR_SPAN_DEG   28      // angular width of one segment
#define REACTOR_OUTER_D    400     // ring outer diameter
#define REACTOR_RING_W     64      // ring thickness
#define REACTOR_BORDER     8       // blue outline thickness
#define REACTOR_CORE_D     176     // center circle
#define REACTOR_HALO_D     232     // glow behind the center circle
#define REACTOR_COL_EDGE   0x1FB1E6
#define REACTOR_COL_FILL   0xCFF6FF
static lv_obj_t *faceHalo = NULL;
static lv_obj_t *faceCenter = NULL;
static lv_obj_t *reactorEdge[REACTOR_SEGS] = {NULL};
static lv_obj_t *reactorFill[REACTOR_SEGS] = {NULL};
static Preferences wifiPreferences;
static DNSServer wifiDnsServer;
static WebServer wifiSetupServer(80);
static String savedWifiSsid;
static String savedWifiPassword;
static bool wifiConnectionPending = false;
static uint32_t wifiConnectStartedMs = 0;
static volatile bool wifiProvisioningRequested = false;
static bool wifiProvisioningActive = false;
static bool wifiPortalHandlersRegistered = false;
static char wifiSetupSsid[24] = "";
static char wifiSetupPassword[16] = "";
static int speakerVolume = 90;
static int screenBrightness = 65;
static char wifiStatusCache[72] = "";

static void updateWifiStatusLocked(const char *text) {
  if (!ui_WifiStatus || strcmp(wifiStatusCache, text) == 0) return;
  if (bsp_display_lock(100) != ESP_OK) return;
  strlcpy(wifiStatusCache, text, sizeof(wifiStatusCache));
  lv_label_set_text(ui_WifiStatus, wifiStatusCache);
  bsp_display_unlock();
}

static const char WIFI_SETUP_PAGE[] PROGMEM = R"rawliteral(
<!doctype html><html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Robot Wi-Fi setup</title><style>
*{box-sizing:border-box}body{margin:0;min-height:100vh;display:grid;place-items:center;background:#10191c;color:#edf5f7;font:16px "Segoe UI",sans-serif}
main{width:min(100% - 32px,420px);padding:28px;border:1px solid #304247;border-radius:12px;background:#192629}
h1{margin:0 0 8px;font-size:24px}p{color:#afc0c3;line-height:1.5;margin:0 0 20px}
label{display:block;margin:16px 0 7px;font-size:14px}input{width:100%;height:46px;padding:10px 12px;border:1px solid #496066;border-radius:7px;background:#0e1719;color:#f4fbfc;font:inherit}
input:focus{outline:2px solid #55d1bd;outline-offset:1px}button{width:100%;height:46px;margin-top:22px;border:0;border-radius:7px;background:#55d1bd;color:#06201d;font:inherit;font-weight:700}
</style></head><body><main><h1>Connect the ESP32 to Wi-Fi</h1>
<p>Enter your home network. The device saves it and reconnects automatically.</p>
<form action="/save" method="post"><label for="ssid">Wi-Fi name</label>
<input id="ssid" name="ssid" maxlength="32" autocomplete="username" required autofocus>
<label for="password">Password</label><input id="password" name="password" type="password" maxlength="63" autocomplete="new-password">
<button type="submit">Save and connect</button></form></main></body></html>
)rawliteral";

static void handleWifiSetupPage() {
  wifiSetupServer.send_P(200, "text/html; charset=utf-8", WIFI_SETUP_PAGE);
}

static void handleWifiSetupSave() {
  String ssid = wifiSetupServer.arg("ssid");
  String password = wifiSetupServer.arg("password");
  if (ssid.length() == 0 || ssid.length() > 32 || password.length() > 63) {
    wifiSetupServer.send(400, "text/plain; charset=utf-8", "Invalid Wi-Fi name or password length.");
    return;
  }

  savedWifiSsid = ssid;
  savedWifiPassword = password;
  wifiPreferences.begin("network", false);
  wifiPreferences.putString("ssid", savedWifiSsid);
  wifiPreferences.putString("password", savedWifiPassword);
  wifiPreferences.end();

  wifiSetupServer.send(200, "text/html; charset=utf-8",
      "<!doctype html><meta name='viewport' content='width=device-width'><title>Connecting</title>"
      "<body style='font:16px sans-serif;background:#10191c;color:#edf5f7;padding:24px'>"
      "<h2>Credentials saved</h2><p>The ESP32 is trying this network. If the password is wrong, reconnect to its setup hotspot and try again.</p></body>");

  WiFi.disconnect(false, false);
  WiFi.mode(WIFI_AP_STA);
  WiFi.begin(savedWifiSsid.c_str(), savedWifiPassword.c_str());
  wifiConnectionPending = true;
  wifiConnectStartedMs = millis();
  weatherLocationReady = false;
  strlcpy(weatherCity, "Locating...", sizeof(weatherCity));
  lastWeatherFetch = 0;
  updateWifiStatusLocked("Connecting to new network...");
  if (cityLabel && bsp_display_lock(200) == ESP_OK) {
    lv_label_set_text(cityLabel, weatherCity);
    bsp_display_unlock();
  }
}

static void startWifiProvisioning() {
  if (wifiProvisioningActive) return;

  String mac = WiFi.macAddress();
  mac.replace(":", "");
  String suffix = mac.substring(mac.length() - 6);
  snprintf(wifiSetupSsid, sizeof(wifiSetupSsid), "Malaka-%s", suffix.c_str());
  strlcpy(wifiSetupPassword, "12345678", sizeof(wifiSetupPassword));

  WiFi.disconnect(false, true);
  WiFi.mode(WIFI_AP_STA);
  if (!WiFi.softAP(wifiSetupSsid, wifiSetupPassword)) {
    Serial.println("Wi-Fi setup hotspot failed to start.");
    updateWifiStatusLocked("Setup hotspot failed");
    return;
  }

  if (!wifiPortalHandlersRegistered) {
    wifiSetupServer.on("/", HTTP_GET, handleWifiSetupPage);
    wifiSetupServer.on("/save", HTTP_POST, handleWifiSetupSave);
    wifiSetupServer.onNotFound([]() {
      wifiSetupServer.sendHeader("Location", "http://192.168.4.1/");
      wifiSetupServer.send(302, "text/plain", "Redirecting to Wi-Fi setup");
    });
    wifiPortalHandlersRegistered = true;
  }

  wifiDnsServer.start(53, "*", WiFi.softAPIP());
  wifiSetupServer.begin();
  wifiProvisioningActive = true;

  char status[72];
  snprintf(status, sizeof(status), "Join %s\nPW: %s\n192.168.4.1", wifiSetupSsid, wifiSetupPassword);
  updateWifiStatusLocked(status);
  Serial.printf("Wi-Fi setup hotspot: %s, page: http://192.168.4.1/\n", wifiSetupSsid);
}

static void stopWifiProvisioning() {
  wifiDnsServer.stop();
  wifiSetupServer.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  wifiProvisioningActive = false;
}

void connectWiFi() {
  wifiPreferences.begin("network", true);
  savedWifiSsid = wifiPreferences.getString("ssid", "");
  savedWifiPassword = wifiPreferences.getString("password", "");
  wifiPreferences.end();

  if (savedWifiSsid.length() == 0) {
    Serial.println("No saved Wi-Fi network; starting setup hotspot.");
    updateWifiStatusLocked("Wi-Fi setup required");
    startWifiProvisioning();
    return;
  }

  Serial.printf("Connecting to Wi-Fi \"%s\"", savedWifiSsid.c_str());
  updateWifiStatusLocked("Connecting...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(savedWifiSsid.c_str(), savedWifiPassword.c_str());
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
    Serial.print(".");
    if (millis() - start > 20000) {
      Serial.println("\nWi-Fi connect timed out.");
      wifiConnectionPending = true;
      wifiConnectStartedMs = millis();
      startWifiProvisioning();
      return;
    }
  }
  Serial.printf("\nWi-Fi connected. IP: %s\n", WiFi.localIP().toString().c_str());
  wifiConnectionPending = false;
  updateWifiStatusLocked("Wi-Fi connected");
}

// ---------------------------------------------------------------------
// Audio codec init - unchanged from v2
// ---------------------------------------------------------------------
bool initAudio() {
  codec_i2c_use_external_bus(0, bsp_i2c_get_handle());
  set_codec_board_type("S3_AMOLED_1_43C");

  codec_init_cfg_t codec_cfg = {};
  codec_cfg.in_mode    = CODEC_I2S_MODE_TDM;
  codec_cfg.out_mode   = CODEC_I2S_MODE_TDM;
  codec_cfg.in_use_tdm = false;
  codec_cfg.reuse_dev  = false;
  if (init_codec(&codec_cfg) != 0) {
    Serial.println("init_codec() failed.");
    return false;
  }

  speaker    = get_playback_handle();
  microphone = get_record_handle();
  if (!speaker || !microphone) {
    Serial.println("Failed to get speaker/microphone handles.");
    return false;
  }

  esp_codec_dev_sample_info_t mic_fs = {};
  mic_fs.sample_rate     = MIC_SAMPLE_RATE;
  mic_fs.channel         = MIC_HW_CHANNELS;
  mic_fs.bits_per_sample = BITS_PER_SAMPLE;
  if (esp_codec_dev_open(microphone, &mic_fs) != ESP_CODEC_DEV_OK) {
    Serial.println("Failed to open microphone.");
    return false;
  }
  esp_codec_dev_set_in_gain(microphone, 35);

  esp_codec_dev_sample_info_t spk_fs = {};
  spk_fs.sample_rate     = SPK_SAMPLE_RATE;
  spk_fs.channel         = SPK_CHANNELS;
  spk_fs.bits_per_sample = BITS_PER_SAMPLE;
  if (esp_codec_dev_open(speaker, &spk_fs) != ESP_CODEC_DEV_OK) {
    Serial.println("Failed to open speaker.");
    return false;
  }
  esp_codec_dev_set_out_vol(speaker, speakerVolume);

  Serial.println("Audio codec ready.");
  return true;
}

// ---------------------------------------------------------------------
// Downsample Gemini's 24kHz reply (forwarded verbatim by pc_bridge.py)
// to our 16kHz speaker rate - identical math to v2.
// ---------------------------------------------------------------------
#define RESAMPLE_MAX_SAMPLES 8192
static int16_t resampleWork[RESAMPLE_MAX_SAMPLES];
static int16_t downsampleOutBuf[RESAMPLE_MAX_SAMPLES];  // reused, no per-chunk malloc
static int16_t resampleLeftover[2];
static int resampleLeftoverCount = 0;

static size_t downsample24to16(const int16_t *in, size_t inCount, int16_t *out) {
  size_t total = resampleLeftoverCount + inCount;
  if (total > RESAMPLE_MAX_SAMPLES) total = RESAMPLE_MAX_SAMPLES;

  for (int i = 0; i < resampleLeftoverCount; i++) resampleWork[i] = resampleLeftover[i];
  size_t copyIn = total - resampleLeftoverCount;
  memcpy(resampleWork + resampleLeftoverCount, in, copyIn * sizeof(int16_t));

  size_t outCount = 0, i = 0;
  while (i + 3 <= total) {
    out[outCount++] = resampleWork[i];
    out[outCount++] = (int16_t)(((int32_t)resampleWork[i + 1] + resampleWork[i + 2]) / 2);
    i += 3;
  }

  resampleLeftoverCount = total - i;
  for (size_t k = 0; k < resampleLeftoverCount; k++) resampleLeftover[k] = resampleWork[i + k];

  return outCount;
}

// ---------------------------------------------------------------------
// UI - three SquareLine screens, navigated by horizontal swipe.
// ---------------------------------------------------------------------
static void screenGestureEvent(lv_event_t *event) {
  if (lv_event_get_code(event) != LV_EVENT_GESTURE) return;
  lv_indev_t *input = lv_indev_active();
  if (!input) return;

  lv_dir_t direction = lv_indev_get_gesture_dir(input);
  lv_obj_t *activeScreen = lv_screen_active();
  lv_obj_t *targetScreen = NULL;
  if (direction == LV_DIR_LEFT) {
    if (activeScreen == ui_Screen1) targetScreen = ui_Screen2;
    else if (activeScreen == ui_Screen2) targetScreen = ui_Screen3;
  } else if (direction == LV_DIR_RIGHT) {
    if (activeScreen == ui_Screen3) targetScreen = ui_Screen2;
    else if (activeScreen == ui_Screen2) targetScreen = ui_Screen1;
  }
  if (targetScreen) lv_screen_load_anim(targetScreen,
      direction == LV_DIR_LEFT ? LV_SCR_LOAD_ANIM_MOVE_LEFT : LV_SCR_LOAD_ANIM_MOVE_RIGHT,
      250, 0, false);
}

static void settingsSliderEvent(lv_event_t *event) {
  lv_obj_t *slider = (lv_obj_t *)lv_event_get_target(event);
  int value = lv_slider_get_value(slider);
  if (slider == ui_VolumeSlider) {
    speakerVolume = value;
    if (speaker) esp_codec_dev_set_out_vol(speaker, speakerVolume);
  } else if (slider == ui_BrightnessSlider) {
    screenBrightness = value;
    bsp_display_brightness_set(screenBrightness);
  }
}

static void openWifiPageEvent(lv_event_t *event) {
  (void)event;
  wifiProvisioningRequested = true;
}

// One arc that draws only its background track between angle a0..a1 (degrees,
// relative to `rot`). Not clickable, so touches/swipes fall through to the screen.
static lv_obj_t *makeReactorArc(lv_obj_t *parent, int size, int width, uint32_t color,
                                int a0, int a1, int rot) {
  lv_obj_t *arc = lv_arc_create(parent);
  lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
  lv_obj_clear_flag(arc, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_size(arc, size, size);
  lv_obj_center(arc);
  lv_obj_set_style_pad_all(arc, 0, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(arc, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(arc, 0, LV_PART_MAIN);
  lv_arc_set_rotation(arc, rot);
  lv_arc_set_bg_angles(arc, a0, a1);
  lv_obj_set_style_arc_width(arc, width, LV_PART_MAIN);
  lv_obj_set_style_arc_color(arc, lv_color_hex(color), LV_PART_MAIN);
  lv_obj_set_style_arc_rounded(arc, false, LV_PART_MAIN);
  lv_obj_set_style_arc_opa(arc, LV_OPA_TRANSP, LV_PART_INDICATOR);   // hide the value arc
  return arc;
}

static void createUI() {
  ui_init();
  eyeL = ui_eyeL;
  eyeR = ui_eyeR;
  faceStatusLabel = ui_faceStatusLabel;
  cityLabel = ui_cityLabel;
  weatherTempLabel = ui_weatherTempLabel;
  weatherDescLabel = ui_weatherDescLabel;
  lv_label_set_text(cityLabel, weatherCity);
  lv_label_set_text(weatherTempLabel, "--°C");
  lv_label_set_text(weatherDescLabel, "Loading...");

  lv_obj_t *screens[] = { ui_Screen1, ui_Screen2, ui_Screen3 };
  for (lv_obj_t *screen : screens) {
    lv_obj_add_event_cb(screen, screenGestureEvent, LV_EVENT_GESTURE, NULL);
  }
  lv_obj_t *gestureObjects[] = {
    eyeL, eyeR, faceStatusLabel, cityLabel, weatherTempLabel, weatherDescLabel,
    ui_ChangeWifi, ui_WifiStatus, ui_VolumeSlider, ui_BrightnessSlider
  };
  for (lv_obj_t *object : gestureObjects) {
    if (object) lv_obj_add_flag(object, LV_OBJ_FLAG_GESTURE_BUBBLE);
  }

  lv_slider_set_range(ui_VolumeSlider, 0, 100);
  lv_slider_set_value(ui_VolumeSlider, speakerVolume, LV_ANIM_OFF);
  lv_slider_set_range(ui_BrightnessSlider, 0, 100);
  lv_slider_set_value(ui_BrightnessSlider, screenBrightness, LV_ANIM_OFF);
  lv_obj_add_event_cb(ui_VolumeSlider, settingsSliderEvent, LV_EVENT_VALUE_CHANGED, NULL);
  lv_obj_add_event_cb(ui_BrightnessSlider, settingsSliderEvent, LV_EVENT_VALUE_CHANGED, NULL);
  lv_obj_add_event_cb(ui_ChangeWifi, openWifiPageEvent, LV_EVENT_CLICKED, NULL);
  lv_label_set_text(ui_WifiStatus, "Wi-Fi not connected");

  lv_obj_t *screen = ui_Screen1;
  lv_obj_set_style_bg_color(screen, lv_color_black(), 0);   // AMOLED: true black makes the glow pop
  lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

  // glow behind the core (flat translucent circle - cheaper than LVGL shadows)
  faceHalo = lv_obj_create(screen);
  lv_obj_remove_style_all(faceHalo);
  lv_obj_clear_flag(faceHalo, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_size(faceHalo, REACTOR_HALO_D, REACTOR_HALO_D);
  lv_obj_center(faceHalo);
  lv_obj_set_style_radius(faceHalo, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(faceHalo, lv_color_hex(REACTOR_COL_EDGE), 0);
  lv_obj_set_style_bg_opa(faceHalo, 40, 0);

  // center circle
  faceCenter = lv_obj_create(screen);
  lv_obj_remove_style_all(faceCenter);
  lv_obj_clear_flag(faceCenter, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(faceCenter, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_size(faceCenter, REACTOR_CORE_D, REACTOR_CORE_D);
  lv_obj_center(faceCenter);
  lv_obj_set_style_radius(faceCenter, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(faceCenter, lv_color_hex(REACTOR_COL_FILL), 0);
  lv_obj_set_style_bg_opa(faceCenter, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(faceCenter, lv_color_hex(REACTOR_COL_EDGE), 0);
  lv_obj_set_style_border_width(faceCenter, 10, 0);
  lv_obj_set_style_border_opa(faceCenter, LV_OPA_COVER, 0);

  // 10 ring segments; segment 0 is centered at 12 o'clock (270 deg in LVGL)
  const int baseRot = 270 - REACTOR_SPAN_DEG / 2;
  for (int i = 0; i < REACTOR_SEGS; ++i) {
    int rot = (baseRot + i * REACTOR_PITCH_DEG) % 360;
    reactorEdge[i] = makeReactorArc(screen, REACTOR_OUTER_D, REACTOR_RING_W,
                                    REACTOR_COL_EDGE, 0, REACTOR_SPAN_DEG, rot);
    reactorFill[i] = makeReactorArc(screen, REACTOR_OUTER_D - 2 * REACTOR_BORDER,
                                    REACTOR_RING_W - 2 * REACTOR_BORDER,
                                    REACTOR_COL_FILL, 3, REACTOR_SPAN_DEG - 3, rot);
  }

  if (eyeL) lv_obj_add_flag(eyeL, LV_OBJ_FLAG_HIDDEN);
  if (eyeR) lv_obj_add_flag(eyeR, LV_OBJ_FLAG_HIDDEN);
  if (faceStatusLabel) {
    lv_obj_add_flag(faceStatusLabel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_text_color(faceStatusLabel, lv_color_hex(0x9FEFFF), 0);
  }
}

// Arc reactor animation, ~30 fps:
//   idle      slow breathing glow
//   listening bright, quick pulse (BOOT held)
//   thinking  light chases around the ring (button released, waiting for Gemini)
//   talking   VU meter from the top down, driven by the audio actually playing
static void updateFaceState() {
  static uint32_t lastTickMs = 0;
  static float shownLevel = 0.0f;
  static uint8_t segCache[REACTOR_SEGS];
  static uint8_t haloCache = 0;
  static bool cacheValid = false;

  if (!faceCenter || !faceHalo) return;
  uint32_t now = millis();
  if (now - lastTickMs < 33) return;
  lastTickMs = now;
  if (bsp_display_lock(50) != ESP_OK) return;
  if (lv_screen_active() != ui_Screen1) {        // face not visible, skip the work
    bsp_display_unlock();
    return;
  }

  int state = (isRecording || isWakeListening) ? 1 : (isSpeaking ? 2 : (isThinking ? 3 : 0));
  uint8_t seg[REACTOR_SEGS];
  uint8_t halo = 40;
  float t = (float)now;

  if (state == 0) {                               // idle
    float b = 0.5f + 0.5f * sinf(t * 0.0025f);
    for (int i = 0; i < REACTOR_SEGS; ++i) seg[i] = 120 + (uint8_t)(b * 40);
    halo = 25 + (uint8_t)(b * 45);
  } else if (state == 1) {                        // listening
    float p = 0.5f + 0.5f * sinf(t * 0.012f);
    for (int i = 0; i < REACTOR_SEGS; ++i) seg[i] = 200 + (uint8_t)(p * 55);
    halo = 90 + (uint8_t)(p * 100);
  } else if (state == 3) {                        // thinking
    static const uint8_t trail[5] = {255, 190, 130, 80, 45};
    int head = (now / 80) % REACTOR_SEGS;
    for (int i = 0; i < REACTOR_SEGS; ++i) {
      int d = (head - i + REACTOR_SEGS) % REACTOR_SEGS;
      seg[i] = d < 5 ? trail[d] : 45;
    }
    halo = 35;
  } else {                                        // talking
    float target;
    if (now - audioLevelMs < 250) {
      target = (float)audioLevel;
    } else {                                      // no fresh level -> fake rhythm
      target = 45.0f + 30.0f * sinf(t * 0.018f) + 20.0f * sinf(t * 0.043f);
    }
    shownLevel += (target - shownLevel) * 0.4f;
    int n = (int)(shownLevel * 6.0f / 100.0f + 0.5f);          // 0..6 lit pairs
    for (int i = 0; i < REACTOR_SEGS; ++i) {
      int d = i <= REACTOR_SEGS / 2 ? i : REACTOR_SEGS - i;    // 0 = top, 5 = bottom
      seg[i] = d < n ? 255 : 50;
    }
    float h = 40.0f + shownLevel * 1.8f;
    halo = h > 255.0f ? 255 : (uint8_t)h;
  }

  for (int i = 0; i < REACTOR_SEGS; ++i) {
    if (!cacheValid || segCache[i] != seg[i]) {
      segCache[i] = seg[i];
      lv_obj_set_style_arc_opa(reactorEdge[i], seg[i], LV_PART_MAIN);
      lv_obj_set_style_arc_opa(reactorFill[i], seg[i], LV_PART_MAIN);
    }
  }
  if (!cacheValid || haloCache != halo) {
    haloCache = halo;
    lv_obj_set_style_bg_opa(faceHalo, halo, 0);
  }
  cacheValid = true;

  bsp_display_unlock();
}

static const char *weatherCodeToText(int code) {
  if (code == 0) return "Clear sky";
  if (code <= 2) return "Partly cloudy";
  if (code == 3) return "Overcast";
  if (code == 45 || code == 48) return "Fog";
  if (code >= 51 && code <= 67) return "Rain";
  if (code >= 71 && code <= 77) return "Snow";
  if (code >= 80 && code <= 82) return "Rain showers";
  if (code >= 95) return "Thunderstorm";
  return "Unknown";
}

static void setWeatherMessage(const char *message) {
  if (weatherDescLabel && bsp_display_lock(200) == ESP_OK) {
    lv_label_set_text(weatherDescLabel, message);
    bsp_display_unlock();
  }
}

static bool fetchWeatherLocation() {
  if (WiFi.status() != WL_CONNECTED) return false;

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(8000);
  if (!http.begin(client, "https://ipwho.is/")) return false;

  int responseCode = http.GET();
  if (responseCode != HTTP_CODE_OK) {
    Serial.printf("Location: HTTP GET failed (%d)\n", responseCode);
    http.end();
    return false;
  }

  StaticJsonDocument<1024> doc;
  DeserializationError error = deserializeJson(doc, http.getStream());
  http.end();
  if (error || !(doc["success"] | false) || doc["city"].isNull() ||
      doc["latitude"].isNull() || doc["longitude"].isNull()) {
    Serial.printf("Location: lookup failed%s%s\n",
                  error ? ": " : "", error ? error.c_str() : "");
    return false;
  }

  float latitude = doc["latitude"].as<float>();
  float longitude = doc["longitude"].as<float>();
  const char *city = doc["city"];
  if (!city || city[0] == '\0' || latitude < -90.0f || latitude > 90.0f ||
      longitude < -180.0f || longitude > 180.0f) {
    Serial.println("Location: response contained invalid coordinates or city.");
    return false;
  }

  weatherLatitude = latitude;
  weatherLongitude = longitude;
  strlcpy(weatherCity, city, sizeof(weatherCity));
  weatherLocationReady = true;

  if (cityLabel && bsp_display_lock(200) == ESP_OK) {
    lv_label_set_text(cityLabel, weatherCity);
    bsp_display_unlock();
  }
  Serial.printf("Location resolved by IP: %s (%.4f, %.4f)\n",
                weatherCity, weatherLatitude, weatherLongitude);
  return true;
}

static bool fetchWeather() {
  if (WiFi.status() != WL_CONNECTED) {
    setWeatherMessage("Waiting for Wi-Fi");
    return false;
  }
  if (!weatherLocationReady && !fetchWeatherLocation()) {
    if (cityLabel && bsp_display_lock(200) == ESP_OK) {
      lv_label_set_text(cityLabel, "Location unavailable");
      lv_label_set_text(weatherDescLabel, "Check network");
      bsp_display_unlock();
    }
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(8000);
  char url[224];
  snprintf(url, sizeof(url),
           "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f&current=temperature_2m,weather_code&timezone=auto",
           weatherLatitude, weatherLongitude);

  if (!http.begin(client, url)) {
    setWeatherMessage("Weather unavailable");
    return false;
  }
  int responseCode = http.GET();
  if (responseCode != HTTP_CODE_OK) {
    Serial.printf("Weather: HTTP GET failed (%d)\n", responseCode);
    http.end();
    setWeatherMessage("Weather unavailable");
    return false;
  }

  String responseBody = http.getString();
  http.end();
  StaticJsonDocument<1024> doc;
  DeserializationError error = deserializeJson(doc, responseBody);
  if (error) {
    Serial.printf("Weather: JSON parse failed: %s\n", error.c_str());
    Serial.printf("Weather response body (%u bytes): %s\n",
                  (unsigned)responseBody.length(), responseBody.c_str());
    setWeatherMessage("Weather unavailable");
    return false;
  }

  JsonObject current = doc["current"];
  if (current.isNull() || current["temperature_2m"].isNull() ||
      current["weather_code"].isNull()) {
    Serial.println("Weather: response is missing current conditions.");
    Serial.print("Weather response: ");
    serializeJson(doc, Serial);
    Serial.println();
    setWeatherMessage("Weather unavailable");
    return false;
  }

  float temperature = current["temperature_2m"].as<float>();
  int weatherCode = current["weather_code"].as<int>();
  char temperatureText[16];
  snprintf(temperatureText, sizeof(temperatureText), "%.0f°C", temperature);

  if (bsp_display_lock(200) == ESP_OK) {
    lv_label_set_text(weatherTempLabel, temperatureText);
    lv_label_set_text(weatherDescLabel, weatherCodeToText(weatherCode));
    bsp_display_unlock();
  }
  Serial.printf("Weather updated: %s, %s\n", temperatureText, weatherCodeToText(weatherCode));
  return true;
}

// ---------------------------------------------------------------------
// Talking to pc_bridge.py: raw binary PCM for audio, small JSON only
// for control messages. Much lighter than the base64/JSON-wrapped
// audio v2 needed to speak Gemini's own wire protocol directly.
// ---------------------------------------------------------------------
static void sendListenStart() {
  StaticJsonDocument<64> doc;
  doc["type"] = "listen_start";
  String out;
  serializeJson(doc, out);
  xSemaphoreTakeRecursive(wsMutex, portMAX_DELAY);
  webSocket.sendTXT(out);
  xSemaphoreGiveRecursive(wsMutex);
}

static void sendListenEnd() {
  StaticJsonDocument<64> doc;
  doc["type"] = "listen_end";
  String out;
  serializeJson(doc, out);
  xSemaphoreTakeRecursive(wsMutex, portMAX_DELAY);
  webSocket.sendTXT(out);
  xSemaphoreGiveRecursive(wsMutex);
}

static void sendMicChunk(const uint8_t *pcm, size_t len) {
  xSemaphoreTakeRecursive(wsMutex, portMAX_DELAY);
  webSocket.sendBIN(pcm, len);
  xSemaphoreGiveRecursive(wsMutex);
}

// ---------------------------------------------------------------------
// Mic streaming task - sends raw PCM chunks directly, no base64/JSON
// wrapping needed since pc_bridge.py speaks binary WebSocket frames.
// ---------------------------------------------------------------------
// The mic streams the whole time the bridge is connected, so pc_bridge.py can hear
// "Hey Jarvis" - even while the speaker is playing, which lets you interrupt him.
// Nothing reaches Gemini until the PC detects the wake word or you hold BOOT.
static bool micShouldStream() {
  return isRecording || bridgeConnected;
}

// Cut off whatever is playing: the playback task discards what is buffered and
// handleBridgeAudio ignores late frames until the next reply starts.
static void interruptPlayback() {
  dropAudio = true;
  isSpeaking = false;
  isThinking = false;
  audioLevel = 0;
}

void MicStreamTask(void *arg) {
  for (;;) {
    if (!micShouldStream()) {
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }

    esp_codec_dev_read(microphone, micRawBuf, MIC_RAW_BYTES);

    int16_t *stereo = (int16_t *)micRawBuf;
    for (int i = 0; i < MIC_FRAMES_PER_CHUNK; i++) {
      int32_t l = stereo[i * 2];
      int32_t r = stereo[i * 2 + 1];
      micMonoBuf[i] = (int16_t)((l + r) / 2);
    }

    sendMicChunk((uint8_t *)micMonoBuf, MIC_MONO_BYTES);
  }
}

// ---------------------------------------------------------------------
// Playback task - the only thing that calls esp_codec_dev_write(), so
// the blocking I2S write never stalls webSocket.loop(). Blocks here
// waiting for data instead of spinning.
// ---------------------------------------------------------------------
#define PLAYBACK_READ_BYTES 2048
void PlaybackTask(void *arg) {
  static uint8_t buf[PLAYBACK_READ_BYTES] __attribute__((aligned(4)));
  for (;;) {
    size_t got = xStreamBufferReceive(audioStreamBuffer, buf, PLAYBACK_READ_BYTES, pdMS_TO_TICKS(300));
    if (got > 0) {
      // loudness of this chunk for the face's VU meter (every 4th sample is plenty)
      const int16_t *pcm = (const int16_t *)buf;
      size_t n = got / 2;
      int64_t acc = 0;
      size_t cnt = 0;
      for (size_t i = 0; i < n; i += 4, ++cnt) acc += (int32_t)pcm[i] * pcm[i];
      if (cnt) {
        float rms = sqrtf((float)acc / (float)cnt);
        float lvl = rms * 100.0f / 3500.0f;       // 3500 = "full scale" for speech, tune to taste
        audioLevel = lvl > 100.0f ? 100 : (uint8_t)lvl;
        audioLevelMs = millis();
      }
      if (!dropAudio) esp_codec_dev_write(speaker, buf, got);   // interrupted -> discard
    } else if (xStreamBufferIsEmpty(audioStreamBuffer)) {
      // no audio arrived for a while - reply is done playing
      isSpeaking = false;
      audioLevel = 0;
    }
  }
}

// ---------------------------------------------------------------------
// Handling messages from pc_bridge.py
// ---------------------------------------------------------------------
static void handleBridgeText(uint8_t *payload, size_t length) {
  StaticJsonDocument<256> doc;
  if (deserializeJson(doc, payload, length)) return;
  const char *type = doc["type"];
  if (!type) return;

  if (strcmp(type, "state") == 0) {
    const char *state = doc["state"];
    if (!state) return;
    isWakeListening = (strcmp(state, "listening") == 0);   // wake word OR button turn
    isThinking      = (strcmp(state, "thinking") == 0);
    if (strcmp(state, "talking") == 0) { dropAudio = false; isSpeaking = true; }   // new reply
    // "idle" clears everything; isRecording is still driven by the button.
  } else if (strcmp(type, "stop_audio") == 0) {
    interruptPlayback();                       // PC heard "Hey Jarvis" while he was talking
  } else if (strcmp(type, "talk_end") == 0) {
    isSpeaking = false;
    isThinking = false;
    isWakeListening = false;
  }
}

static void handleBridgeAudio(uint8_t *payload, size_t length) {
  // payload is raw PCM16 mono @ 24kHz from Gemini, forwarded verbatim
  // by pc_bridge.py - downsample to our 16kHz speaker rate, then hand
  // it off to PlaybackTask via a stream buffer instead of writing to
  // the codec here. This callback runs inside webSocket.loop(), so a
  // blocking esp_codec_dev_write() here would stall network reads for
  // as long as that chunk takes to play - which is exactly what was
  // causing the growing lag.
  if (dropAudio) return;          // interrupted: ignore late frames of the old reply
  isSpeaking = true;
  size_t sampleCount = length / 2;
  size_t outSamples = downsample24to16((int16_t *)payload, sampleCount, downsampleOutBuf);
  if (outSamples == 0) return;

  size_t outBytes = outSamples * sizeof(int16_t);
  size_t sent = xStreamBufferSend(audioStreamBuffer, downsampleOutBuf, outBytes, pdMS_TO_TICKS(200));
  if (sent < outBytes) {
    Serial.println("Playback buffer full, dropped audio - PlaybackTask can't keep up.");
  }
}

void webSocketEvent(WStype_t type, uint8_t *payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED:
      Serial.println("Connected to pc_bridge.py.");
      bridgeConnected = true;
      break;
    case WStype_DISCONNECTED:
      Serial.println("Disconnected from pc_bridge.py.");
      bridgeConnected = false;
      isSpeaking = false;
      isThinking = false;
      isWakeListening = false;
      break;
    case WStype_TEXT:
      handleBridgeText(payload, length);
      break;
    case WStype_BIN:
      handleBridgeAudio(payload, length);
      break;
    case WStype_ERROR:
      Serial.printf("WebSocket error: %s\n", payload ? (const char *)payload : "(no detail)");
      break;
    default:
      break;
  }
}

static bool probeBridgeHost(const char *host) {
  WiFiClient probe;
  probe.setTimeout(500);
  if (!probe.connect(host, PC_PORT)) return false;
  delay(50);
  probe.stop();
  return true;
}

static void discoverBridgeHost() {
  if (WiFi.status() != WL_CONNECTED || PC_HOST[0] != '\0') return;

  uint32_t now = millis();
  if (now - lastBridgeDiscoveryMs < 2500) return;
  lastBridgeDiscoveryMs = now;

  IPAddress network = WiFi.localIP() & WiFi.subnetMask();
  Serial.printf("Searching LAN for pc_bridge.py on %u.%u.%u.0/24...\n",
                network[0], network[1], network[2]);

  for (int lastOctet = 2; lastOctet < 254; ++lastOctet) {
    IPAddress candidate = network;
    candidate[3] = static_cast<uint8_t>(lastOctet);
    if (candidate == WiFi.localIP()) continue;

    char host[16];
    snprintf(host, sizeof(host), "%u.%u.%u.%u",
             candidate[0], candidate[1], candidate[2], candidate[3]);

    if (probeBridgeHost(host)) {
      snprintf(PC_HOST, sizeof(PC_HOST), "%s", host);
      Serial.printf("Discovered pc_bridge.py at %s:%u\n", PC_HOST, PC_PORT);
      return;
    }
  }

  Serial.println("pc_bridge.py not found on LAN yet; retrying...");
}

static void pollBridgeDiscovery() {
  if (WiFi.status() != WL_CONNECTED) return;

  if (PC_HOST[0] == '\0') {
    discoverBridgeHost();
    return;
  }

  if (!bridgeStarted) {
    bridgeStarted = true;
    webSocket.begin(PC_HOST, PC_PORT, "/");
    webSocket.onEvent(webSocketEvent);
    webSocket.setReconnectInterval(3000);
    Serial.printf("Connecting to pc_bridge.py at %s:%u\n", PC_HOST, PC_PORT);
  }
}

void connectBridge() {
  // Bridge target is discovered dynamically on the current LAN.
  // This avoids hard-coding a PC IP that changes when you move between Wi-Fi networks.
  pollBridgeDiscovery();
}

// ---------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println("\n=== AI_Chatbot_PCBridge starting ===");

  pinMode(BUTTON_0_GPIO_PIN, INPUT_PULLUP);
  pinMode(BAT_LED_GPIO_PIN, OUTPUT);

  bsp_broolesia_display_init();
  bsp_display_brightness_set(screenBrightness);
  if (bsp_display_lock(-1) == ESP_OK) {
    createUI();
    bsp_display_unlock();
  }

  connectWiFi();
  digitalWrite(BAT_LED_GPIO_PIN, WiFi.status() == WL_CONNECTED ? HIGH : LOW);

  micRawBuf  = (uint8_t *)heap_caps_malloc(MIC_RAW_BYTES, MALLOC_CAP_SPIRAM);
  micMonoBuf = (int16_t *)heap_caps_malloc(MIC_MONO_BYTES, MALLOC_CAP_SPIRAM);
  if (!micRawBuf || !micMonoBuf) {
    Serial.println("Failed to allocate PSRAM audio buffers - halting.");
    while (true) delay(1000);
  }

  if (!initAudio()) {
    Serial.println("Audio init failed - halting.");
    while (true) delay(1000);
  }

  wsMutex = xSemaphoreCreateRecursiveMutex();
  connectBridge();
  audioStreamBuffer = xStreamBufferCreate(AUDIO_STREAM_BUFFER_BYTES, 1);
  if (!audioStreamBuffer) {
    Serial.println("Failed to create audio stream buffer - halting.");
    while (true) delay(1000);
  }
  xTaskCreatePinnedToCore(MicStreamTask, "MicStreamTask", 8 * 1024, NULL, 5, &micTaskHandle, 1);
  xTaskCreatePinnedToCore(PlaybackTask, "PlaybackTask", 4 * 1024, NULL, 5, &playbackTaskHandle, 0);

  Serial.println("Setup done. Hold BOOT and talk.");
}

static void pollWifiStatus() {
  static uint32_t lastPollMs = 0;
  uint32_t now = millis();
  if (now - lastPollMs < 500) return;
  lastPollMs = now;

  if (wifiProvisioningRequested) {
    wifiProvisioningRequested = false;
    startWifiProvisioning();
  }

  if (wifiProvisioningActive) {
    wifiDnsServer.processNextRequest();
    wifiSetupServer.handleClient();
  }

  if (WiFi.status() == WL_CONNECTED) {
    wifiConnectionPending = false;
    if (wifiProvisioningActive) stopWifiProvisioning();
    updateWifiStatusLocked("Wi-Fi connected");
  } else if (wifiConnectionPending && now - wifiConnectStartedMs > 20000) {
    wifiConnectionPending = false;
    updateWifiStatusLocked("Connection failed; setup hotspot is still on");
  }
}

void loop() {
  pollBridgeDiscovery();
  if (bridgeStarted) {
    xSemaphoreTakeRecursive(wsMutex, portMAX_DELAY);
    webSocket.loop();
    xSemaphoreGiveRecursive(wsMutex);
  }

  bool reading = digitalRead(BUTTON_0_GPIO_PIN);
  if (reading != lastRawReading) {
    lastDebounceMs = millis();
  }
  if ((millis() - lastDebounceMs) > DEBOUNCE_MS && reading != buttonState) {
    buttonState = reading;
    if (buttonState == LOW && bridgeConnected && !isRecording) {
      Serial.println("Recording... (release BOOT when done)");
      if (isSpeaking || isThinking) interruptPlayback();   // BOOT while he talks = interrupt
      isRecording = true;
      isThinking = false;
      sendListenStart();
    } else if (buttonState == HIGH && isRecording) {
      isRecording = false;
      sendListenEnd();
      Serial.println("Sent. Waiting for reply...");
    }
  }
  lastRawReading = reading;

  updateFaceState();
  pollWifiStatus();

  bool weatherDue = millis() > 20000 &&
                    (lastWeatherFetch == 0 || millis() - lastWeatherFetch >= weatherRetryMs);
  if (weatherDue && !isRecording && !isSpeaking && !isThinking && !isWakeListening) {
    lastWeatherFetch = millis();
    weatherRetryMs = fetchWeather() ? WEATHER_REFRESH_MS : 30000;
  }

  static uint32_t lastCheck = 0;
  if (millis() - lastCheck > 2000) {
    lastCheck = millis();
    digitalWrite(BAT_LED_GPIO_PIN, (WiFi.status() == WL_CONNECTED && bridgeConnected) ? HIGH : LOW);
  }
}
