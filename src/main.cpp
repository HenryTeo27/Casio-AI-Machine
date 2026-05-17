#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include "esp_camera.h"
#include <U8g2lib.h>
#include <ArduinoJson.h>
#include <vector>
#include <Wire.h>

// =====================================================
// USER CONFIG
// =====================================================

const char* WIFI_SSID = "YOUR_WIFI_NAME";
const char* WIFI_PASS = "YOUR_WIFI_PASSWORD";

const char* SERVER_BASE_URL = "https://accelertechnology.my";

// 你给的 device API key。之后如果公开源码，记得换掉。
const char* DEVICE_API_KEY = "21326a10-c7f8-4ca9-8e7c-d6f55c15d564";
const char* DEVICE_ID = "CASIO_AI_MACHINE_001";

// Server endpoints
const char* UPLOAD_PHOTO_PATH = "/api/casio-ai/upload-photo";
const char* SOLVE_PATH = "/api/casio-ai/solve";

// =====================================================
// PIN CONFIG
// =====================================================

// OLED SSD1305 I2C
// 按我们之前接法：SDA=GPIO1, SCL=GPIO2
// 如果 GPIO2 的 LED_ON 影响 OLED，可改成其他空闲 GPIO。
#define OLED_SDA 1
#define OLED_SCL 2
#define OLED_RST 47

// Buttons
#define BTN_PAGE_UP 14
#define BTN_PAGE_DOWN 21
#define BTN_Q_PREV 38
#define BTN_Q_NEXT 39
#define BTN_CAMERA 40
#define BTN_OK 41
#define BTN_EXTRA 42  // 之后加第7个按钮才用

// Camera pins from your ESP32-S3 N16R8 CAM pinout
#define CAM_PIN_PWDN -1
#define CAM_PIN_RESET -1
#define CAM_PIN_XCLK 15
#define CAM_PIN_SIOD 4
#define CAM_PIN_SIOC 5

#define CAM_PIN_D7 16  // Y9
#define CAM_PIN_D6 17  // Y8
#define CAM_PIN_D5 18  // Y7
#define CAM_PIN_D4 12  // Y6
#define CAM_PIN_D3 10  // Y5
#define CAM_PIN_D2 8   // Y4
#define CAM_PIN_D1 9   // Y3
#define CAM_PIN_D0 11  // Y2

#define CAM_PIN_VSYNC 6
#define CAM_PIN_HREF 7
#define CAM_PIN_PCLK 13

// =====================================================
// SYSTEM CONFIG
// =====================================================

#define MAX_PENDING_PHOTOS 3
#define MAX_HISTORY_ITEMS 20
#define MAX_TOTAL_TEXT_CHARS 120000
#define MAX_ANSWER_CHARS 12000

#define SCREEN_CHARS_PER_LINE 21
#define SCREEN_LINES 2

#define BUTTON_DEBOUNCE_MS 180
#define WIFI_CONNECT_TIMEOUT_MS 15000
#define HTTP_TIMEOUT_MS 90000

// =====================================================
// OLED
// =====================================================

// 如果这个 constructor 编译失败，换成：
// U8G2_SSD1305_128X32_ADAFRUIT_F_HW_I2C
U8G2_SSD1305_128X32_NONAME_F_HW_I2C u8g2(U8G2_R0, OLED_RST);

// =====================================================
// TYPES
// =====================================================

struct JpegPhoto {
  uint8_t* data = nullptr;
  size_t len = 0;
};

struct ChatItem {
  int id = 0;
  int photoCount = 0;
  bool thinking = false;
  String title;
  String answer;
  std::vector<String> lines;
};

enum ScreenMode {
  MODE_BOOT,
  MODE_READY,
  MODE_CAPTURE,
  MODE_THINKING,
  MODE_CHAT,
  MODE_STATUS,
  MODE_ERROR
};

struct Button {
  uint8_t pin;
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
      lastChange = millis();
      lastRead = reading;
    }

    if ((millis() - lastChange) > BUTTON_DEBOUNCE_MS && reading != lastStable) {
      lastStable = reading;
      if (lastStable == LOW) return true;
    }

    return false;
  }
};

// =====================================================
// GLOBAL STATE
// =====================================================

Button btnPageUp;
Button btnPageDown;
Button btnQPrev;
Button btnQNext;
Button btnCamera;
Button btnOk;
Button btnExtra;

ScreenMode screenMode = MODE_BOOT;

std::vector<JpegPhoto> pendingPhotos;
std::vector<ChatItem> history;

int currentChatIndex = -1;
int currentLineOffset = 0;
int nextQuestionId = 1;

bool captureBusy = false;
bool solveRunning = false;
bool readyClearSelected = false;
int pageIndex = 0;  // 0=camera, 1=chat, 2=token/status

bool hasTokenUsage = false;
int lastPromptTokens = 0;
int lastCompletionTokens = 0;
int lastTotalTokens = 0;

String lastError = "";

SemaphoreHandle_t stateMutex;

// =====================================================
// UTIL
// =====================================================

void lockState() {
  if (stateMutex) xSemaphoreTake(stateMutex, portMAX_DELAY);
}

void unlockState() {
  if (stateMutex) xSemaphoreGive(stateMutex);
}

String jsonEscape(const String& s) {
  String out;
  out.reserve(s.length() + 16);

  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];

    if (c == '\"') out += "\\\"";
    else if (c == '\\') out += "\\\\";
    else if (c == '\n') out += "\\n";
    else if (c == '\r') {}
    else out += c;
  }

  return out;
}

void freePhoto(JpegPhoto& p) {
  if (p.data) {
    free(p.data);
    p.data = nullptr;
    p.len = 0;
  }
}

String compactText(String s) {
  s.replace("\r", "");
  s.replace("\t", " ");

  // Markdown cleanup
  s.replace("```", "");
  s.replace("**", "");
  s.replace("__", "");
  s.replace("###", "");
  s.replace("##", "");
  s.replace("#", "");

  // Common LaTeX/math replacements
  s.replace("\\times", "*");
  s.replace("\\cdot", "*");
  s.replace("\\div", "/");
  s.replace("\\sqrt", "sqrt");
  s.replace("\\leq", "<=");
  s.replace("\\geq", ">=");
  s.replace("\\neq", "!=");
  s.replace("\\pi", "pi");
  s.replace("\\theta", "theta");
  s.replace("\\alpha", "alpha");
  s.replace("\\beta", "beta");
  s.replace("\\Delta", "Delta");
  s.replace("\\frac", "frac");
  s.replace("\\left", "");
  s.replace("\\right", "");
  s.replace("$", "");

  while (s.indexOf("  ") >= 0) s.replace("  ", " ");

  if (s.length() > MAX_ANSWER_CHARS) {
    s = s.substring(0, MAX_ANSWER_CHARS);
    s += "\n...[truncated]";
  }

  return s;
}

std::vector<String> wrapTextToLines(String text) {
  text = compactText(text);

  std::vector<String> lines;
  String current = "";

  for (size_t i = 0; i < text.length(); i++) {
    char c = text[i];

    if (c == '\n') {
      if (current.length() > 0) {
        lines.push_back(current);
        current = "";
      }

      continue;
    }

    current += c;

    if (current.length() >= SCREEN_CHARS_PER_LINE) {
      lines.push_back(current);
      current = "";
    }
  }

  if (current.length() > 0) lines.push_back(current);
  if (lines.empty()) lines.push_back("");

  return lines;
}

void rebuildChatLines(ChatItem& item) {
  String full = "Q" + String(item.id) + " [" + String(item.photoCount) + " photo]";
  full += "\n";

  if (item.thinking) {
    full += "Thinking...";
  } else {
    full += item.answer;
  }

  item.lines = wrapTextToLines(full);
}

void trimHistoryIfNeeded() {
  while (history.size() > MAX_HISTORY_ITEMS) {
    history.erase(history.begin());
    if (currentChatIndex > 0) currentChatIndex--;
  }

  size_t total = 0;

  for (auto& item : history) {
    total += item.answer.length();
  }

  while (total > MAX_TOTAL_TEXT_CHARS && history.size() > 1) {
    total -= history.front().answer.length();
    history.erase(history.begin());

    if (currentChatIndex > 0) currentChatIndex--;
  }
}

void clearContext() {
  lockState();

  for (auto& p : pendingPhotos) {
    freePhoto(p);
  }

  pendingPhotos.clear();

  history.clear();
  currentChatIndex = -1;
  currentLineOffset = 0;
  nextQuestionId = 1;
  screenMode = MODE_READY;
  readyClearSelected = false;
  solveRunning = false;

  unlockState();
}

// =====================================================
// DISPLAY
// =====================================================

void drawTwoLines(const String& line1, const String& line2) {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(0, 12, line1.c_str());
  u8g2.drawStr(0, 28, line2.c_str());
  u8g2.sendBuffer();
}

void drawChatScreen() {
  lockState();

  if (history.empty() || currentChatIndex < 0 || currentChatIndex >= (int)history.size()) {
    unlockState();
    drawTwoLines("Ready: press CAM", readyClearSelected ? "OK = clear ctx" : "Down: clear ctx");
    return;
  }

  ChatItem& item = history[currentChatIndex];

  if (item.lines.empty()) rebuildChatLines(item);

  int maxOffset = max(0, (int)item.lines.size() - SCREEN_LINES);

  if (currentLineOffset > maxOffset) currentLineOffset = maxOffset;
  if (currentLineOffset < 0) currentLineOffset = 0;

  String l1 = item.lines[currentLineOffset];
  String l2 = "";

  if (currentLineOffset + 1 < (int)item.lines.size()) {
    l2 = item.lines[currentLineOffset + 1];
  }

  unlockState();

  drawTwoLines(l1, l2);
}

void drawCaptureScreen() {
  lockState();
  int count = pendingPhotos.size();
  unlockState();

  String boxes = "Photos: ";

  for (int i = 0; i < count; i++) {
    boxes += "#";
  }

  if (count == 0) boxes += "none";

  String hint;

  if (count >= MAX_PENDING_PHOTOS) {
    hint = "OK=send max";
  } else {
    hint = "CAM=more OK=send";
  }

  drawTwoLines(boxes, hint);
}

void drawStatusScreen() {
  lockState();
  int pending = (int)pendingPhotos.size();
  int chatCount = (int)history.size();
  bool usageReady = hasTokenUsage;
  int p = lastPromptTokens;
  int c = lastCompletionTokens;
  int t = lastTotalTokens;
  unlockState();

  String line1 = "Token/Status";
  String line2;

  if (usageReady) {
    line2 = "T" + String(t) + " P" + String(p) + " C" + String(c);
  } else {
    line2 = "T-- P" + String(pending) + " H" + String(chatCount);
  }

  drawTwoLines(line1, line2);
}

void renderScreen() {
  switch (screenMode) {
    case MODE_BOOT:
      drawTwoLines("Casio AI Machine", "Starting...");
      break;

    case MODE_READY:
      if (readyClearSelected) {
        drawTwoLines("Clear context?", "OK=yes CAM=no");
      } else {
        drawTwoLines("Ready: CAM photo", "Down: clear ctx");
      }
      break;

    case MODE_CAPTURE:
      drawCaptureScreen();
      break;

    case MODE_THINKING:
    case MODE_CHAT:
      drawChatScreen();
      break;

    case MODE_STATUS:
      drawStatusScreen();
      break;

    case MODE_ERROR:
      drawTwoLines("ERROR", lastError.substring(0, 21));
      break;
  }
}

// =====================================================
// WIFI
// =====================================================

bool connectWiFi() {
  drawTwoLines("WiFi connecting", WIFI_SSID);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  unsigned long start = millis();

  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_CONNECT_TIMEOUT_MS) {
    delay(250);
    Serial.print(".");
  }

  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("WiFi connected: ");
    Serial.println(WiFi.localIP());

    drawTwoLines("WiFi connected", WiFi.localIP().toString());
    delay(800);

    return true;
  }

  Serial.println("WiFi failed");

  lastError = "WiFi failed";
  screenMode = MODE_ERROR;

  return false;
}

// =====================================================
// CAMERA
// =====================================================

bool initCamera() {
  camera_config_t config;

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

  // 测试阶段用 XGA，清晰度/体积比较平衡。
  // 如果 OCR 不够清楚，改成 FRAMESIZE_UXGA。
  config.frame_size = FRAMESIZE_XGA;
  config.jpeg_quality = 10;
  config.fb_count = 1;
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.grab_mode = CAMERA_GRAB_LATEST;

  if (!psramFound()) {
    Serial.println("PSRAM not found. Camera may fail.");
    config.frame_size = FRAMESIZE_VGA;
    config.fb_location = CAMERA_FB_IN_DRAM;
  }

  esp_err_t err = esp_camera_init(&config);

  if (err != ESP_OK) {
    Serial.printf("Camera init failed: 0x%x\n", err);

    lastError = "Camera init fail";
    screenMode = MODE_ERROR;

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
    s->set_awb_gain(s, 1);
    s->set_exposure_ctrl(s, 1);
    s->set_aec2(s, 1);
    s->set_gain_ctrl(s, 1);
  }

  Serial.println("Camera initialized");

  return true;
}

bool capturePhoto() {
  if (captureBusy || solveRunning) return false;

  lockState();

  if ((int)pendingPhotos.size() >= MAX_PENDING_PHOTOS) {
    unlockState();
    return false;
  }

  unlockState();

  captureBusy = true;
  screenMode = MODE_CAPTURE;

  drawTwoLines("Camera focusing", "Please wait...");

  delay(500);  // 给 AF / AE 一点时间

  camera_fb_t* fb = esp_camera_fb_get();

  if (!fb) {
    captureBusy = false;
    lastError = "Capture failed";
    screenMode = MODE_ERROR;

    return false;
  }

  uint8_t* copy = (uint8_t*)ps_malloc(fb->len);

  if (!copy) {
    esp_camera_fb_return(fb);

    captureBusy = false;
    lastError = "No PSRAM";
    screenMode = MODE_ERROR;

    return false;
  }

  memcpy(copy, fb->buf, fb->len);

  JpegPhoto p;
  p.data = copy;
  p.len = fb->len;

  esp_camera_fb_return(fb);

  lockState();

  pendingPhotos.push_back(p);
  Serial.printf("Captured photo %d, size=%u bytes\n", pendingPhotos.size(), (unsigned)p.len);

  unlockState();

  captureBusy = false;
  screenMode = MODE_CAPTURE;

  return true;
}

// =====================================================
// SERVER API
// =====================================================

bool uploadPhotoToServer(const JpegPhoto& photo, int questionId, int index, String& outPhotoId) {
  if (WiFi.status() != WL_CONNECTED) {
    if (!connectWiFi()) return false;
  }

  WiFiClientSecure client;
  client.setInsecure();  // testing only. Production should use root CA.
  client.setTimeout(HTTP_TIMEOUT_MS / 1000);

  String url = String(SERVER_BASE_URL) + UPLOAD_PHOTO_PATH;
  url += "?question_id=" + String(questionId);
  url += "&index=" + String(index);

  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);

  if (!http.begin(client, url)) {
    Serial.println("HTTP begin upload failed");
    return false;
  }

  http.addHeader("Content-Type", "image/jpeg");
  http.addHeader("X-Device-Id", DEVICE_ID);
  http.addHeader("X-Device-Api-Key", DEVICE_API_KEY);
  http.addHeader("X-Question-Id", String(questionId));
  http.addHeader("X-Photo-Index", String(index));

  int code = http.POST(photo.data, photo.len);
  String body = http.getString();

  Serial.printf("Upload code=%d\n", code);
  Serial.println(body);

  http.end();

  if (code < 200 || code >= 300) return false;

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body);

  if (err) {
    Serial.println("Upload JSON parse failed");
    return false;
  }

  bool ok = doc["ok"] | false;
  const char* photoId = doc["photo_id"] | "";

  if (!ok || strlen(photoId) == 0) return false;

  outPhotoId = String(photoId);

  return true;
}

bool requestSolveFromServer(
  int questionId,
  int photoCount,
  const std::vector<String>& photoIds,
  String& outAnswer
) {
  if (WiFi.status() != WL_CONNECTED) {
    if (!connectWiFi()) return false;
  }

  WiFiClientSecure client;
  client.setInsecure();  // testing only
  client.setTimeout(HTTP_TIMEOUT_MS / 1000);

  String url = String(SERVER_BASE_URL) + SOLVE_PATH;

  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);

  if (!http.begin(client, url)) {
    Serial.println("HTTP begin solve failed");
    return false;
  }

  JsonDocument doc;
  doc["device_id"] = DEVICE_ID;
  doc["question_id"] = questionId;
  doc["photo_count"] = photoCount;
  doc["mode"] = "gpt-test";
  doc["model_hint"] = "gpt-5.4-mini-or-nano";

  JsonArray arr = doc["photo_ids"].to<JsonArray>();

  for (auto& id : photoIds) {
    arr.add(id);
  }

  // 给服务器一点最近 context。不要太长。
  String contextTail = "";

  lockState();

  int start = max(0, (int)history.size() - 3);

  for (int i = start; i < (int)history.size(); i++) {
    if (!history[i].thinking) {
      contextTail += "Q" + String(history[i].id) + ": ";
      contextTail += history[i].answer.substring(0, 800);
      contextTail += "\n";
    }
  }

  unlockState();

  doc["context_tail"] = contextTail;

  String payload;
  serializeJson(doc, payload);

  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-Device-Id", DEVICE_ID);
  http.addHeader("X-Device-Api-Key", DEVICE_API_KEY);

  int code = http.POST(payload);
  String body = http.getString();

  Serial.printf("Solve code=%d\n", code);
  Serial.println(body);

  http.end();

  if (code < 200 || code >= 300) return false;

  JsonDocument resp;
  DeserializationError err = deserializeJson(resp, body);

  if (err) {
    Serial.println("Solve JSON parse failed");
    outAnswer = body;
    return true;
  }

  bool ok = resp["ok"] | false;

  if (!ok) {
    outAnswer = String(resp["error"] | "AI failed");
    return false;
  }

  const char* displayText = resp["display_text"] | nullptr;
  const char* answer = resp["answer"] | nullptr;

  JsonVariant usage = resp["usage"];
  int promptTokens = -1;
  int completionTokens = -1;
  int totalTokens = -1;

  if (!usage.isNull()) {
    if (usage["prompt_tokens"].is<int>()) promptTokens = usage["prompt_tokens"].as<int>();
    else if (usage["input_tokens"].is<int>()) promptTokens = usage["input_tokens"].as<int>();

    if (usage["completion_tokens"].is<int>()) completionTokens = usage["completion_tokens"].as<int>();
    else if (usage["output_tokens"].is<int>()) completionTokens = usage["output_tokens"].as<int>();

    if (usage["total_tokens"].is<int>()) totalTokens = usage["total_tokens"].as<int>();
  }

  if (promptTokens < 0 && resp["prompt_tokens"].is<int>()) promptTokens = resp["prompt_tokens"].as<int>();
  if (completionTokens < 0 && resp["completion_tokens"].is<int>()) completionTokens = resp["completion_tokens"].as<int>();
  if (totalTokens < 0 && resp["total_tokens"].is<int>()) totalTokens = resp["total_tokens"].as<int>();
  if (totalTokens < 0 && promptTokens >= 0 && completionTokens >= 0) totalTokens = promptTokens + completionTokens;

  if (promptTokens >= 0 || completionTokens >= 0 || totalTokens >= 0) {
    lockState();
    hasTokenUsage = true;
    lastPromptTokens = max(0, promptTokens);
    lastCompletionTokens = max(0, completionTokens);
    lastTotalTokens = max(0, totalTokens);
    unlockState();
  }

  if (displayText && strlen(displayText) > 0) {
    outAnswer = String(displayText);
  } else if (answer && strlen(answer) > 0) {
    outAnswer = String(answer);
  } else {
    outAnswer = body;
  }

  return true;
}

// =====================================================
// SOLVE TASK
// =====================================================

struct SolveJob {
  int questionId;
  int photoCount;
  std::vector<JpegPhoto> photos;
};

void solveTask(void* param) {
  SolveJob* job = (SolveJob*)param;

  std::vector<String> photoIds;
  String finalAnswer = "";

  bool ok = true;

  for (int i = 0; i < (int)job->photos.size(); i++) {
    lockState();

    if (currentChatIndex >= 0 && currentChatIndex < (int)history.size()) {
      history[currentChatIndex].answer =
        "Uploading photo " + String(i + 1) + "/" + String(job->photos.size()) + "...";

      history[currentChatIndex].thinking = true;
      rebuildChatLines(history[currentChatIndex]);
    }

    unlockState();

    String photoId;

    if (!uploadPhotoToServer(job->photos[i], job->questionId, i, photoId)) {
      ok = false;
      finalAnswer = "Photo upload failed.";
      break;
    }

    photoIds.push_back(photoId);
  }

  if (ok) {
    lockState();

    if (currentChatIndex >= 0 && currentChatIndex < (int)history.size()) {
      history[currentChatIndex].answer = "Thinking...";
      history[currentChatIndex].thinking = true;
      rebuildChatLines(history[currentChatIndex]);
    }

    unlockState();

    if (!requestSolveFromServer(job->questionId, job->photoCount, photoIds, finalAnswer)) {
      ok = false;

      if (finalAnswer.length() == 0) {
        finalAnswer = "AI request failed.";
      }
    }
  }

  for (auto& p : job->photos) {
    freePhoto(p);
  }

  job->photos.clear();

  lockState();

  for (auto& item : history) {
    if (item.id == job->questionId) {
      item.thinking = false;
      item.answer = ok ? finalAnswer : ("Error: " + finalAnswer);
      rebuildChatLines(item);
      break;
    }
  }

  trimHistoryIfNeeded();

  for (int i = 0; i < (int)history.size(); i++) {
    if (history[i].id == job->questionId) {
      currentChatIndex = i;
      currentLineOffset = 0;
      break;
    }
  }

  solveRunning = false;
  pageIndex = 1;
  screenMode = MODE_CHAT;

  unlockState();

  delete job;
  vTaskDelete(NULL);
}

void startSolve() {
  if (solveRunning) return;

  lockState();

  if (pendingPhotos.empty()) {
    unlockState();
    return;
  }

  SolveJob* job = new SolveJob();

  job->questionId = nextQuestionId++;
  job->photoCount = pendingPhotos.size();
  job->photos.swap(pendingPhotos);

  ChatItem item;
  item.id = job->questionId;
  item.photoCount = job->photoCount;
  item.thinking = true;
  item.title = "Q" + String(job->questionId);
  item.answer = "Thinking...";

  rebuildChatLines(item);

  history.push_back(item);
  currentChatIndex = history.size() - 1;
  currentLineOffset = 0;

  solveRunning = true;
  screenMode = MODE_THINKING;

  unlockState();

  xTaskCreatePinnedToCore(
    solveTask,
    "solveTask",
    16000,
    job,
    1,
    NULL,
    1
  );
}

// =====================================================
// BUTTON ACTIONS
// =====================================================

void scrollPageUp() {
  if (screenMode == MODE_READY) {
    readyClearSelected = false;
    return;
  }

  if (screenMode == MODE_CAPTURE) return;

  lockState();

  currentLineOffset -= SCREEN_LINES;

  if (currentLineOffset < 0) {
    currentLineOffset = 0;
  }

  unlockState();

  screenMode = MODE_CHAT;
}

void scrollPageDown() {
  if (screenMode == MODE_READY) {
    readyClearSelected = true;
    return;
  }

  if (screenMode == MODE_CAPTURE) return;

  lockState();

  if (currentChatIndex >= 0 && currentChatIndex < (int)history.size()) {
    ChatItem& item = history[currentChatIndex];
    int maxOffset = max(0, (int)item.lines.size() - SCREEN_LINES);

    currentLineOffset += SCREEN_LINES;

    if (currentLineOffset > maxOffset) {
      currentLineOffset = maxOffset;
    }
  }

  unlockState();

  screenMode = MODE_CHAT;
}

void prevQuestion() {
  if (screenMode == MODE_CAPTURE) return;

  lockState();

  if (!history.empty()) {
    if (currentChatIndex > 0) {
      currentChatIndex--;
    }

    currentLineOffset = 0;
  }

  unlockState();

  pageIndex = 1;
  screenMode = history.empty() ? MODE_READY : MODE_CHAT;
}

void nextQuestion() {
  if (screenMode == MODE_CAPTURE) return;

  lockState();

  if (!history.empty()) {
    if (currentChatIndex < (int)history.size() - 1) {
      currentChatIndex++;
    }

    currentLineOffset = 0;
  }

  unlockState();

  pageIndex = 1;
  screenMode = history.empty() ? MODE_READY : MODE_CHAT;
}

void cyclePageWithExtraButton() {
  if (screenMode == MODE_BOOT || screenMode == MODE_ERROR) return;
  if (captureBusy || solveRunning) return;

  pageIndex = (pageIndex + 1) % 3;
  readyClearSelected = false;

  if (pageIndex == 0) {
    lockState();
    bool hasPending = !pendingPhotos.empty();
    unlockState();
    screenMode = hasPending ? MODE_CAPTURE : MODE_READY;
    return;
  }

  if (pageIndex == 1) {
    screenMode = MODE_CHAT;
    return;
  }

  screenMode = MODE_STATUS;
}

void cameraButtonAction() {
  if (readyClearSelected) {
    readyClearSelected = false;
    screenMode = MODE_READY;
    return;
  }

  if (solveRunning || captureBusy) return;

  pageIndex = 0;
  capturePhoto();
}

void okButtonAction() {
  if (screenMode == MODE_READY && readyClearSelected) {
    clearContext();
    drawTwoLines("Context cleared", "Ready");
    delay(800);
    return;
  }

  if (screenMode == MODE_CAPTURE) {
    startSolve();
    return;
  }

  lockState();

  bool hasPending = !pendingPhotos.empty();

  unlockState();

  if (hasPending) {
    startSolve();
  }
}

// =====================================================
// SETUP
// =====================================================

void setup() {
  Serial.begin(115200);
  delay(800);

  stateMutex = xSemaphoreCreateMutex();

  Serial.println();
  Serial.println("Casio AI Machine booting...");

  // Buttons
  btnPageUp.begin(BTN_PAGE_UP);
  btnPageDown.begin(BTN_PAGE_DOWN);
  btnQPrev.begin(BTN_Q_PREV);
  btnQNext.begin(BTN_Q_NEXT);
  btnCamera.begin(BTN_CAMERA);
  btnOk.begin(BTN_OK);
  btnExtra.begin(BTN_EXTRA);

  // OLED
  Wire.begin(OLED_SDA, OLED_SCL);
  Wire.setClock(400000);

  u8g2.setI2CAddress(0x3C * 2);
  u8g2.begin();
  u8g2.setContrast(25);  // OLED亮度低一点，比较低调
  u8g2.enableUTF8Print();

  screenMode = MODE_BOOT;
  pageIndex = 0;
  renderScreen();

  delay(1000);

  // Camera
  drawTwoLines("Init camera", "Please wait...");

  if (!initCamera()) {
    renderScreen();
    return;
  }

  // WiFi
  connectWiFi();

  screenMode = MODE_READY;
  pageIndex = 0;
  readyClearSelected = false;

  renderScreen();
}

// =====================================================
// LOOP
// =====================================================

void loop() {
  if (btnPageUp.fell()) {
    Serial.println("BTN PAGE UP");
    scrollPageUp();
  }

  if (btnPageDown.fell()) {
    Serial.println("BTN PAGE DOWN");
    scrollPageDown();
  }

  if (btnQPrev.fell()) {
    Serial.println("BTN Q PREV");
    prevQuestion();
  }

  if (btnQNext.fell()) {
    Serial.println("BTN Q NEXT");
    nextQuestion();
  }

  if (btnCamera.fell()) {
    Serial.println("BTN CAMERA");
    cameraButtonAction();
  }

  if (btnOk.fell()) {
    Serial.println("BTN OK");
    okButtonAction();
  }

  if (btnExtra.fell()) {
    Serial.println("BTN EXTRA");
    cyclePageWithExtraButton();
  }

  renderScreen();

  delay(120);
}
