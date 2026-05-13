#include <Arduino.h>
#include <Wire.h>
#include <U8g2lib.h>
#include "esp_camera.h"

// OLED pins
static constexpr int OLED_SDA = 1;
static constexpr int OLED_SCL = 2;
static constexpr int OLED_RST = 47;

// Buttons
static constexpr int BTN_CAMERA = 40;  // press to capture
static constexpr unsigned long AUTO_CAPTURE_INTERVAL_MS = 5000;

// Camera pins (ESP32-S3 CAM + OV5640 wiring from project)
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

static constexpr unsigned long BTN_DEBOUNCE_MS = 180;

U8G2_SSD1305_128X32_NONAME_F_HW_I2C u8g2(U8G2_R0, OLED_RST);

struct Button {
  uint8_t pin{};
  bool lastStable = HIGH;
  bool lastRead = HIGH;
  unsigned long lastChange = 0;

  void begin(uint8_t p) {
    pin = p;
    pinMode(pin, INPUT_PULLUP);
    lastStable = digitalRead(pin);
    lastRead = lastStable;
    lastChange = millis();
  }

  bool fell() {
    bool reading = digitalRead(pin);
    if (reading != lastRead) {
      lastRead = reading;
      lastChange = millis();
    }
    if ((millis() - lastChange) > BTN_DEBOUNCE_MS && reading != lastStable) {
      lastStable = reading;
      return lastStable == LOW;
    }
    return false;
  }
};

Button btnCamera;
int photoCount = 0;
unsigned long lastAutoCaptureMs = 0;

void drawTwoLines(const String& line1, const String& line2) {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(0, 12, line1.c_str());
  u8g2.drawStr(0, 28, line2.c_str());
  u8g2.sendBuffer();
}

bool initCamera() {
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
  config.frame_size = FRAMESIZE_XGA;
  config.jpeg_quality = 10;
  config.fb_count = 1;
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.grab_mode = CAMERA_GRAB_LATEST;

  if (!psramFound()) {
    Serial.println("[WARN] PSRAM not found, fallback to VGA/DRAM.");
    config.frame_size = FRAMESIZE_VGA;
    config.fb_location = CAMERA_FB_IN_DRAM;
  }

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("[ERR] Camera init failed: 0x%x\n", err);
    return false;
  }

  sensor_t* s = esp_camera_sensor_get();
  if (s) {
    s->set_framesize(s, FRAMESIZE_XGA);
    s->set_quality(s, 10);
    s->set_brightness(s, 0);
    s->set_contrast(s, 0);
    s->set_saturation(s, 0);
    s->set_whitebal(s, 1);
    s->set_exposure_ctrl(s, 1);
    s->set_gain_ctrl(s, 1);
  }

  Serial.println("[OK] Camera initialized.");
  return true;
}

void captureAndReport() {
  drawTwoLines("Capturing...", "Please wait");
  delay(200);

  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("[ERR] Capture failed: fb is null.");
    drawTwoLines("Capture failed", "Check camera/FPC");
    return;
  }

  photoCount++;
  Serial.printf("[OK] Photo #%d | bytes=%u | w=%u h=%u format=%u\n",
                photoCount, (unsigned)fb->len, fb->width, fb->height, fb->format);

  String line1 = "Photo #" + String(photoCount) + " OK";
  String line2 = "JPEG " + String((unsigned)fb->len) + " bytes";
  if (line2.length() > 21) {
    line2 = String((unsigned)(fb->len / 1024)) + "KB captured";
  }
  drawTwoLines(line1, line2);

  esp_camera_fb_return(fb);
}

void setup() {
  Serial.begin(115200);
  delay(600);
  Serial.println();
  Serial.println("=== Casio Camera Test ===");

  btnCamera.begin(BTN_CAMERA);

  Wire.begin(OLED_SDA, OLED_SCL);
  Wire.setClock(400000);
  u8g2.setI2CAddress(0x3C * 2);
  u8g2.begin();
  u8g2.setContrast(25);
  u8g2.enableUTF8Print();

  drawTwoLines("Camera Test", "Init...");

  if (!initCamera()) {
    drawTwoLines("Camera init ERR", "Check wiring");
    return;
  }

  drawTwoLines("Ready", "Auto capture...");
  Serial.println("[INFO] Auto capture mode enabled (no button needed).");
  delay(400);
  captureAndReport();
  lastAutoCaptureMs = millis();
}

void loop() {
  if (btnCamera.fell()) {  // optional manual trigger
    Serial.println("[BTN] CAMERA (manual)");
    captureAndReport();
  }

  if (millis() - lastAutoCaptureMs >= AUTO_CAPTURE_INTERVAL_MS) {
    Serial.println("[AUTO] Capture tick");
    captureAndReport();
    lastAutoCaptureMs = millis();
  }

  delay(30);
}
