#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <math.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <ArduinoJson.h>
#include "esp_sleep.h"
#include "esp_camera.h"
#include "casio_config.h"

const String SERVER_BASE_URL = String("https://") + SERVER_HOST;

// =====================================================
// PIN CONFIG
// =====================================================
static constexpr int OLED_SDA = 1;
static constexpr int OLED_SCL = 2;
static constexpr int OLED_RST = 47;

static constexpr int CAM_PIN_PWDN = -1;
static constexpr int CAM_PIN_RESET = -1;
static constexpr int CAM_PIN_XCLK = 15;
static constexpr int CAM_PIN_SIOD = 4;
static constexpr int CAM_PIN_SIOC = 5;

static constexpr int CAM_PIN_D7 = 16;
static constexpr int CAM_PIN_D6 = 17;
static constexpr int CAM_PIN_D5 = 18;
static constexpr int CAM_PIN_D4 = 12;
static constexpr int CAM_PIN_D3 = 10;
static constexpr int CAM_PIN_D2 = 8;
static constexpr int CAM_PIN_D1 = 9;
static constexpr int CAM_PIN_D0 = 11;

static constexpr int CAM_PIN_VSYNC = 6;
static constexpr int CAM_PIN_HREF = 7;
static constexpr int CAM_PIN_PCLK = 13;

// =====================================================
// SYSTEM CONFIG
// =====================================================
static constexpr int QUESTION_ID = 9001;
static constexpr int PHOTO_INDEX = 0;
static constexpr uint32_t START_CAPTURE_DELAY_MS = 0;
static constexpr uint32_t SENSOR_SETTLE_MS = 2500;
static constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 30000;
static constexpr uint32_t HTTP_TIMEOUT_MS = 20000;

U8G2_SSD1305_128X32_NONAME_F_HW_I2C u8g2(U8G2_R0, OLED_RST);
framesize_t activeFrameSize = FRAMESIZE_SVGA;
int activeJpegQuality = 8;

struct CapturedJpeg {
  uint8_t* data = nullptr;
  size_t len = 0;
  int width = 0;
  int height = 0;
};

void drawTwoLines(const String& l1, const String& l2) {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(0, 12, l1.c_str());
  u8g2.drawStr(0, 28, l2.c_str());
  u8g2.sendBuffer();
}

bool connectWiFi() {
  drawTwoLines("WiFi connecting", WIFI_SSID);
  WiFi.disconnect(true);
  delay(300);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_CONNECT_TIMEOUT_MS) {
    delay(250);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() != WL_CONNECTED) {
    wl_status_t status = WiFi.status();
    Serial.printf("[ERR] WiFi failed, status=%d\n", status);
    drawTwoLines("WiFi failed", "status=" + String((int)status));
    return false;
  }

  Serial.print("[OK] WiFi: ");
  Serial.println(WiFi.localIP());
  drawTwoLines("WiFi OK", WiFi.localIP().toString());
  delay(700);
  return true;
}

void applyTextProfile(int aeLevel, int brightness, gainceiling_t gainCeiling) {
  sensor_t* s = esp_camera_sensor_get();
  if (!s) return;

  s->set_quality(s, activeJpegQuality);
  s->set_whitebal(s, 1);
  s->set_awb_gain(s, 1);
  s->set_wb_mode(s, 0);
  s->set_exposure_ctrl(s, 1);
  s->set_aec2(s, 1);
  s->set_ae_level(s, aeLevel);
  s->set_gain_ctrl(s, 1);
  s->set_gainceiling(s, gainCeiling);
  s->set_brightness(s, brightness);
  s->set_contrast(s, 2);
  s->set_saturation(s, -1);
  if (s->set_sharpness) s->set_sharpness(s, -2);
  if (s->set_denoise) s->set_denoise(s, 2);
  if (s->set_bpc) s->set_bpc(s, 1);
  if (s->set_wpc) s->set_wpc(s, 1);
  if (s->set_lenc) s->set_lenc(s, 1);
  if (s->set_raw_gma) s->set_raw_gma(s, 1);
}

bool initCamera() {
  bool hasPsram = psramFound();
  activeFrameSize = hasPsram ? FRAMESIZE_UXGA : FRAMESIZE_SVGA;
  activeJpegQuality = 8;

  Serial.printf("[INFO] psramFound=%d freePsram=%u\n", hasPsram, ESP.getFreePsram());

  camera_config_t config{};
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = CAM_PIN_D0;
  config.pin_d1 = CAM_PIN_D1;
  config.pin_d2 = CAM_PIN_D2;
  config.pin_d3 = CAM_PIN_D3;
  config.pin_d4 = CAM_PIN_D4;
  config.pin_d5 = CAM_PIN_D5;
  config.pin_d6 = CAM_PIN_D6;
  config.pin_d7 = CAM_PIN_D7;
  config.pin_xclk = CAM_PIN_XCLK;
  config.pin_pclk = CAM_PIN_PCLK;
  config.pin_vsync = CAM_PIN_VSYNC;
  config.pin_href = CAM_PIN_HREF;
  config.pin_sccb_sda = CAM_PIN_SIOD;
  config.pin_sccb_scl = CAM_PIN_SIOC;
  config.pin_pwdn = CAM_PIN_PWDN;
  config.pin_reset = CAM_PIN_RESET;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size = activeFrameSize;
  config.jpeg_quality = activeJpegQuality;
  config.fb_count = 1;
  config.fb_location = hasPsram ? CAMERA_FB_IN_PSRAM : CAMERA_FB_IN_DRAM;
  config.grab_mode = CAMERA_GRAB_LATEST;

  if (!hasPsram) {
    Serial.println("[WARN] PSRAM not found, fallback SVGA/DRAM");
  }

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("[ERR] Camera init failed: 0x%x\n", err);
    drawTwoLines("Camera init ERR", "Check camera");
    return false;
  }

  sensor_t* s = esp_camera_sensor_get();
  if (s) {
    s->set_framesize(s, activeFrameSize);
    applyTextProfile(2, 1, GAINCEILING_2X);
  }

  Serial.println("[OK] Camera initialized");
  return true;
}

void sensorStandby() {
  sensor_t* s = esp_camera_sensor_get();
  if (!s || !s->set_reg) return;

  // OV5640 software standby. This reduces activity, but does not remove power.
  int res = s->set_reg(s, 0x3008, 0xFF, 0x42);
  Serial.printf("[INFO] Sensor standby reg result=%d\n", res);
  delay(100);
}

void quietCameraPins() {
  pinMode(CAM_PIN_XCLK, OUTPUT);
  digitalWrite(CAM_PIN_XCLK, LOW);

  const int inputPins[] = {
    CAM_PIN_SIOD, CAM_PIN_SIOC, CAM_PIN_D0, CAM_PIN_D1, CAM_PIN_D2,
    CAM_PIN_D3, CAM_PIN_D4, CAM_PIN_D5, CAM_PIN_D6, CAM_PIN_D7,
    CAM_PIN_VSYNC, CAM_PIN_HREF, CAM_PIN_PCLK
  };

  for (int pin : inputPins) {
    if (pin >= 0) pinMode(pin, INPUT);
  }
}

void coolDownAndSleep(const String& line1, const String& line2) {
  drawTwoLines(line1, line2);
  Serial.println("[INFO] Cooling down: WiFi off, OLED off, camera pins quiet, deep sleep.");
  delay(5000);

  u8g2.setPowerSave(1);
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  btStop();
  quietCameraPins();

  Serial.flush();
  esp_deep_sleep_start();
}

bool uploadPhotoToServer(const uint8_t* jpegData, size_t jpegLen, String& outPhotoId) {
  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(HTTP_TIMEOUT_MS / 1000);

  String url = String(SERVER_BASE_URL) + UPLOAD_PHOTO_PATH +
               "?question_id=" + String(QUESTION_ID) +
               "&index=" + String(PHOTO_INDEX);

  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);
  if (!http.begin(client, url)) {
    Serial.println("[ERR] Upload begin failed");
    return false;
  }

  http.addHeader("Content-Type", "image/jpeg");
  http.addHeader("X-Device-Id", DEVICE_ID);
  http.addHeader("X-Device-Api-Key", DEVICE_API_KEY);
  http.addHeader("X-Question-Id", String(QUESTION_ID));
  http.addHeader("X-Photo-Index", String(PHOTO_INDEX));

  int code = http.POST((uint8_t*)jpegData, jpegLen);
  String body = http.getString();
  http.end();

  Serial.printf("[INFO] upload code=%d\n", code);
  Serial.println(body);

  if (code < 200 || code >= 300) return false;

  JsonDocument doc;
  if (deserializeJson(doc, body)) return false;

  bool ok = doc["ok"] | false;
  const char* photoId = doc["photo_id"] | "";
  if (!ok || strlen(photoId) == 0) return false;

  outPhotoId = String(photoId);
  return true;
}

void freeCapturedJpeg(CapturedJpeg& jpeg) {
  if (jpeg.data) {
    free(jpeg.data);
    jpeg.data = nullptr;
  }
  jpeg.len = 0;
  jpeg.width = 0;
  jpeg.height = 0;
}

bool captureCandidate(const char* label, int aeLevel, int brightness, gainceiling_t gainCeiling, CapturedJpeg& out) {
  Serial.printf("[INFO] Capture candidate %s ae=%d brightness=%d gainCeiling=%d\n",
                label, aeLevel, brightness, (int)gainCeiling);
  drawTwoLines("Capturing", label);

  applyTextProfile(aeLevel, brightness, gainCeiling);
  delay(600);

  for (int i = 0; i < 4; i++) {
    camera_fb_t* warmup = esp_camera_fb_get();
    if (warmup) {
      Serial.printf("[INFO] Warmup %d bytes=%u\n", i + 1, (unsigned)warmup->len);
      esp_camera_fb_return(warmup);
    } else {
      Serial.printf("[WARN] Warmup %d failed\n", i + 1);
    }
    delay(180);
  }

  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("[WARN] Final capture failed");

    if (activeFrameSize != FRAMESIZE_SXGA) {
      Serial.println("[INFO] Falling back to SXGA and retrying capture");
      activeFrameSize = FRAMESIZE_SXGA;
      sensor_t* s = esp_camera_sensor_get();
      if (s) {
        s->set_framesize(s, activeFrameSize);
        applyTextProfile(aeLevel, brightness, gainCeiling);
      }
      delay(800);

      for (int i = 0; i < 3; i++) {
        camera_fb_t* retryWarmup = esp_camera_fb_get();
        if (retryWarmup) esp_camera_fb_return(retryWarmup);
        delay(180);
      }

      fb = esp_camera_fb_get();
    }

    if (!fb) {
      Serial.println("[ERR] Final capture failed after fallback");
      return false;
    }
  }

  out.data = (uint8_t*)ps_malloc(fb->len);
  if (!out.data) out.data = (uint8_t*)malloc(fb->len);
  if (!out.data) {
    Serial.println("[ERR] JPEG copy allocation failed");
    esp_camera_fb_return(fb);
    return false;
  }

  memcpy(out.data, fb->buf, fb->len);
  out.len = fb->len;
  out.width = fb->width;
  out.height = fb->height;

  Serial.printf("[OK] Candidate %s bytes=%u, w=%u h=%u\n",
                label, (unsigned)fb->len, fb->width, fb->height);

  esp_camera_fb_return(fb);
  return true;
}

void runOneShotFlow() {
  drawTwoLines("Wait 10 sec...", "Prepare target");
  Serial.println("[INFO] Waiting 10s before single capture...");
  delay(START_CAPTURE_DELAY_MS);

  drawTwoLines("Camera init", "Power active");
  if (!initCamera()) {
    esp_camera_deinit();
    coolDownAndSleep("Camera failed", "Sleeping");
    return;
  }

  delay(SENSOR_SETTLE_MS);

  CapturedJpeg best;

  if (!captureCandidate("clean UXGA", 2, 1, GAINCEILING_2X, best)) {
    esp_camera_deinit();
    coolDownAndSleep("Capture failed", "No frame buffer");
    return;
  }

  sensorStandby();
  esp_err_t deinitErr = esp_camera_deinit();
  quietCameraPins();
  Serial.printf("[INFO] Camera deinit before upload: 0x%x\n", (unsigned)deinitErr);

  Serial.printf("[OK] Selected JPEG bytes=%u, w=%d h=%d\n",
                (unsigned)best.len, best.width, best.height);
  drawTwoLines("Captured", String((unsigned)(best.len / 1024)) + "KB");

  String photoId;
  drawTwoLines("Uploading...", "upload-photo");
  bool uploadOk = uploadPhotoToServer(best.data, best.len, photoId);
  freeCapturedJpeg(best);

  if (!uploadOk) {
    coolDownAndSleep("Upload FAIL", "Check API/auth");
    return;
  }

  String line2 = "photo_id:" + photoId;
  if (line2.length() > 21) line2 = line2.substring(0, 21);
  coolDownAndSleep("Upload OK", line2);
}

void setup() {
  Serial.begin(115200);
  delay(600);
  Serial.println();
  Serial.println("=== Casio Camera OneShot Test ===");

  Wire.begin(OLED_SDA, OLED_SCL);
  Wire.setClock(400000);
  u8g2.setI2CAddress(0x3C * 2);
  u8g2.begin();
  u8g2.setContrast(25);
  u8g2.enableUTF8Print();

  drawTwoLines("OneShot Camera", "Init...");

  if (!connectWiFi()) {
    coolDownAndSleep("WiFi failed", "Sleeping");
    return;
  }

  runOneShotFlow();
}

void loop() {
  delay(1000);
}
