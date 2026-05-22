#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include "esp_camera.h"
#include <U8g2lib.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <vector>
#include "mbedtls/base64.h"
#include "casio_config.h"

// =====================================================
// USER CONFIG
// =====================================================

// =====================================================
// PIN CONFIG
// =====================================================

#define OLED_SDA 1
#define OLED_SCL 2
#define OLED_RST 47

// Latest mapping requested by user.
#define BTN_SCROLL_UP 14
#define BTN_SCROLL_DOWN 21
#define BTN_SCROLL_LEFT 40
#define BTN_SCROLL_RIGHT 41
#define BTN_CAMERA 38
#define BTN_ENTER 39
#define BTN_PAGE 42

#define CAM_PIN_PWDN -1
#define CAM_PIN_RESET -1
#define CAM_PIN_XCLK 15
#define CAM_PIN_SIOD 4
#define CAM_PIN_SIOC 5
#define CAM_PIN_D7 16
#define CAM_PIN_D6 17
#define CAM_PIN_D5 18
#define CAM_PIN_D4 12
#define CAM_PIN_D3 10
#define CAM_PIN_D2 8
#define CAM_PIN_D1 9
#define CAM_PIN_D0 11
#define CAM_PIN_VSYNC 6
#define CAM_PIN_HREF 7
#define CAM_PIN_PCLK 13

// =====================================================
// SYSTEM CONFIG
// =====================================================

#define SCREEN_W 128
#define SCREEN_H 32
#define MAX_PENDING_PHOTOS 3
#define MAX_HISTORY_ITEMS 10
#define MAX_DRAFT_QUESTIONS 6
#define MAX_CONCURRENT_SOLVE_JOBS 6

#define BUTTON_DEBOUNCE_MS 22
#define OLED_CLEAR_SETTLE_MS 8
#define OLED_CONTRAST 0
#define WIFI_CONNECT_TIMEOUT_MS 15000
#define UPLOAD_HTTP_TIMEOUT_MS 60000
#define UPLOAD_RETRY_COUNT 3
#define UPLOAD_RETRY_DELAY_MS 700
#define SOLVE_WAIT_TIMEOUT_MS 300000UL
#define HTTPCLIENT_SOLVE_TIMEOUT_MS 60000
#define SOLVE_POLL_DELAY_MS 20
#define RESULT_FETCH_TOTAL_WAIT_MS 720000UL
#define RESULT_FETCH_RETRY_DELAY_MS 3000

#define SIDEBAR_SNIPPET_CHARS 8

// Camera tuning profile (aligned with camera_test/web tune)
#define CAM_WARMUP_FRAMES 3
#define CAM_WARMUP_DELAY_MS 180
#define CAM_SETTLE_AFTER_INIT_MS 500
#define CAM_CAPTURE_JPEG_QUALITY 10
#define CAM_WARMUP_JPEG_QUALITY 12
#define CAM_FALLBACK_JPEG_QUALITY 12
#define CAM_CAPTURE_RETRIES 3

// =====================================================
// OLED
// =====================================================

U8G2_SSD1305_128X32_NONAME_F_HW_I2C u8g2(U8G2_R0, OLED_RST);

// =====================================================
// TYPES
// =====================================================

struct Button {
  uint8_t pin;
  bool lastStable = HIGH;
  bool lastRead = HIGH;
  unsigned long lastChange = 0;
  unsigned long lastRepeat = 0;

  void begin(uint8_t p) {
    pin = p;
    pinMode(pin, INPUT_PULLUP);
    lastStable = digitalRead(pin);
    lastRead = lastStable;
    lastChange = millis();
    lastRepeat = 0;
  }

  bool fell() {
    bool reading = digitalRead(pin);
    if (reading != lastRead) {
      lastChange = millis();
      lastRead = reading;
    }

    if ((millis() - lastChange) >= BUTTON_DEBOUNCE_MS && reading != lastStable) {
      lastStable = reading;
      if (lastStable == LOW) lastRepeat = millis();
      return lastStable == LOW;
    }
    return false;
  }

  bool repeat(unsigned long firstDelayMs = 360, unsigned long repeatMs = 140) {
    if (lastStable != LOW) return false;
    unsigned long now = millis();
    if ((now - lastChange) < firstDelayMs) return false;
    if ((now - lastRepeat) < repeatMs) return false;
    lastRepeat = now;
    return true;
  }
};

enum ScreenMode {
  MODE_BOOT,
  MODE_READY,
  MODE_CAPTURE,
  MODE_THINKING,
  MODE_VIEW,
  MODE_SIDEBAR,
  MODE_ERROR
};

struct DisplayBlock {
  String kind = "text";
  int width = SCREEN_W;
  int height = SCREEN_H;
  std::vector<uint8_t> packedXbm;  // 1-bit row-major, xbm bit-order.
  int xOffset = 0;
  int yOffset = 0;
};

struct ChatItem {
  int id = 0;
  int photoCount = 0;
  bool thinking = false;
  String answer;
  std::vector<DisplayBlock> blocks;
  std::vector<String> fallbackLines;
  int blockIndex = 0;
  int fallbackLineOffset = 0;
};

struct SolveJob {
  int questionId = 0;
  int photoCount = 0;
  struct PendingPhoto {
    uint8_t* data = nullptr;
    size_t len = 0;
    uint16_t width = 0;
    uint16_t height = 0;
  };
  std::vector<PendingPhoto> photos;
};

struct DraftQuestion {
  int id = 0;
  std::vector<SolveJob::PendingPhoto> photos;
};

// =====================================================
// GLOBAL STATE
// =====================================================

Button btnUp;
Button btnDown;
Button btnLeft;
Button btnRight;
Button btnCamera;
Button btnEnter;
Button btnPage;

ScreenMode screenMode = MODE_BOOT;
String lastError = "";

std::vector<ChatItem> history;
int currentHistoryIndex = -1;
int sidebarIndex = 0;

int nextQuestionId = 1;
std::vector<DraftQuestion> draftQuestions;
int activeDraftIndex = -1;

bool captureBusy = false;
bool solveRunning = false;
int activeSolveJobs = 0;
bool cameraSessionOn = false;
bool answerBlankMode = false;
bool screenDirty = true;

int lastPromptTokens = -1;
int lastCompletionTokens = -1;
int lastTotalTokens = -1;

String lastOledLine1 = "";
String lastOledLine2 = "";
String lastBitmapKey = "";

SemaphoreHandle_t stateMutex = nullptr;

// =====================================================
// HELPERS
// =====================================================

void lockState() {
  if (stateMutex) xSemaphoreTake(stateMutex, portMAX_DELAY);
}

void unlockState() {
  if (stateMutex) xSemaphoreGive(stateMutex);
}

void markScreenDirty() {
  screenDirty = true;
}

void invalidateOledCache() {
  lastOledLine1 = "__invalidate__";
  lastOledLine2 = "";
  lastBitmapKey = "";
}

void setScreenMode(ScreenMode mode) {
  if (screenMode != mode) {
    screenMode = mode;
    markScreenDirty();
  }
}

void syncSolveRunningFlag() {
  solveRunning = activeSolveJobs > 0;
}

String safeShort(const String& in, int maxLen) {
  if ((int)in.length() <= maxLen) return in;
  return in.substring(0, maxLen);
}

String singleLine(const String& in) {
  String out = in;
  out.replace("\r", " ");
  out.replace("\n", " ");
  while (out.indexOf("  ") >= 0) out.replace("  ", " ");
  return out;
}

String answerPreview(const ChatItem& item, int maxChars) {
  String src = item.answer;
  if (src.length() == 0 && !item.fallbackLines.empty()) src = item.fallbackLines[0];
  src = singleLine(src);
  if (src.length() <= maxChars) return src;
  return src.substring(0, maxChars);
}

std::vector<String> wrapFallbackLines(String raw) {
  raw.replace("\r", "");
  std::vector<String> result;
  String current = "";

  for (size_t i = 0; i < raw.length(); i++) {
    char c = raw[i];
    if (c == '\n') {
      if (current.length() > 0) {
        result.push_back(current);
        current = "";
      }
      continue;
    }

    current += c;
    if (current.length() >= 21) {
      result.push_back(current);
      current = "";
    }
  }

  if (current.length() > 0) result.push_back(current);
  if (result.empty()) result.push_back("");
  return result;
}

void drawTwoLines(const String& line1, const String& line2) {
  if (line1 == lastOledLine1 && line2 == lastOledLine2) {
    return;
  }

  if (line1 != lastOledLine1 || line2 != lastOledLine2) {
    lastOledLine1 = line1;
    lastOledLine2 = line2;
    Serial.printf("[OLED] %s || %s\n", line1.c_str(), line2.c_str());
  }

  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(0, 12, line1.c_str());
  u8g2.drawStr(0, 28, line2.c_str());
  u8g2.sendBuffer();
}

void clearOledFrame() {
  u8g2.clearBuffer();
  u8g2.sendBuffer();
  delay(OLED_CLEAR_SETTLE_MS);
}

void renderAnswerBlankMode() {
  if (lastOledLine1 != "__blank_style__" || lastOledLine2 != "") {
    lastOledLine1 = "__blank_style__";
    lastOledLine2 = "";
    lastBitmapKey = "";
    Serial.println("[OLED] blank style mode");
    clearOledFrame();
  }
}

bool decodeBase64ToBytes(const String& encoded, std::vector<uint8_t>& out) {
  size_t required = 0;
  int rc = mbedtls_base64_decode(
    nullptr,
    0,
    &required,
    reinterpret_cast<const unsigned char*>(encoded.c_str()),
    encoded.length()
  );

  if (rc != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL && rc != 0) return false;
  if (required == 0) return false;

  out.assign(required, 0);
  size_t written = 0;
  rc = mbedtls_base64_decode(
    out.data(),
    out.size(),
    &written,
    reinterpret_cast<const unsigned char*>(encoded.c_str()),
    encoded.length()
  );

  if (rc != 0) return false;
  if (written != out.size()) out.resize(written);
  return true;
}

int parseHttpStatusCode(const String& statusLine) {
  int firstSpace = statusLine.indexOf(' ');
  if (firstSpace < 0) return -1;
  int secondSpace = statusLine.indexOf(' ', firstSpace + 1);
  String codeText = (secondSpace > firstSpace)
    ? statusLine.substring(firstSpace + 1, secondSpace)
    : statusLine.substring(firstSpace + 1);
  return codeText.toInt();
}

String rawPreview(const String& raw, size_t maxLen = 200) {
  String preview = raw.substring(0, min((int)raw.length(), (int)maxLen));
  preview.replace("\r", "\\r");
  preview.replace("\n", "\\n");
  return preview;
}

bool parseRawHttpResponse(
  const String& rawResp,
  const char* tag,
  int& outStatusCode,
  String& outBody
) {
  outStatusCode = -1;
  outBody = "";

  if (rawResp.length() == 0) {
    Serial.printf("[%s] empty raw response\n", tag);
    return false;
  }

  int headerEnd = rawResp.indexOf("\r\n\r\n");
  int bodyStart = headerEnd >= 0 ? headerEnd + 4 : -1;
  if (headerEnd < 0) {
    headerEnd = rawResp.indexOf("\n\n");
    bodyStart = headerEnd >= 0 ? headerEnd + 2 : -1;
  }

  if (headerEnd >= 0) {
    int statusEnd = rawResp.indexOf('\n');
    if (statusEnd < 0 || statusEnd > headerEnd) statusEnd = headerEnd;

    String statusLine = rawResp.substring(0, statusEnd);
    statusLine.trim();
    outStatusCode = parseHttpStatusCode(statusLine);
    outBody = rawResp.substring(bodyStart);

    Serial.printf("[%s] status=%d body_len=%u raw_len=%u\n",
                  tag, outStatusCode, (unsigned)outBody.length(), (unsigned)rawResp.length());
    if (outStatusCode <= 0) {
      Serial.printf("[%s] bad status line: %s\n", tag, statusLine.c_str());
      return false;
    }
    return true;
  }

  if (rawResp.startsWith("HTTP/")) {
    Serial.printf("[%s] malformed HTTP response, no header separator. raw[0..200]=%s\n",
                  tag, rawPreview(rawResp).c_str());
    return false;
  }

  // Some simple/non-standard responses can be raw JSON without HTTP headers.
  outStatusCode = 200;
  outBody = rawResp;
  Serial.printf("[%s] headerless body len=%u\n", tag, (unsigned)outBody.length());
  return true;
}

bool ensureWiFiConnected();

void showError(const String& error) {
  lastError = safeShort(error, 21);
  setScreenMode(MODE_ERROR);
}

void freePendingPhoto(SolveJob::PendingPhoto& p) {
  if (p.data) {
    free(p.data);
    p.data = nullptr;
  }
  p.len = 0;
  p.width = 0;
  p.height = 0;
}

void clearPendingPhotos(std::vector<SolveJob::PendingPhoto>& photos) {
  for (auto& p : photos) freePendingPhoto(p);
  photos.clear();
}

ChatItem* currentChatItem() {
  if (currentHistoryIndex < 0 || currentHistoryIndex >= (int)history.size()) return nullptr;
  return &history[currentHistoryIndex];
}

String statusLabel(const ChatItem& item) {
  if (item.thinking) {
    String src = item.answer;
    src.toLowerCase();
    if (src.indexOf("upload") >= 0) return "UPLD";
    if (src.indexOf("render") >= 0) return "REND";
    return "PROC";
  }
  if (item.answer.startsWith("Error:")) return "ERR";
  return "READY";
}

DraftQuestion* currentDraftQuestion() {
  if (activeDraftIndex < 0 || activeDraftIndex >= (int)draftQuestions.size()) return nullptr;
  return &draftQuestions[activeDraftIndex];
}

bool hasAnyDraftPhotos() {
  for (const auto& draft : draftQuestions) {
    if (!draft.photos.empty()) return true;
  }
  return false;
}

int totalDraftPhotoCount() {
  int total = 0;
  for (const auto& draft : draftQuestions) total += draft.photos.size();
  return total;
}

void ensureCaptureSession() {
  if (activeDraftIndex >= 0 && activeDraftIndex < (int)draftQuestions.size()) return;

  DraftQuestion draft;
  draft.id = nextQuestionId++;
  draftQuestions.push_back(std::move(draft));
  activeDraftIndex = draftQuestions.size() - 1;
  markScreenDirty();
}

void moveToNextDraftQuestion() {
  ensureCaptureSession();
  DraftQuestion* current = currentDraftQuestion();
  if (current && current->photos.empty()) {
    markScreenDirty();
    return;
  }

  if ((int)draftQuestions.size() >= MAX_DRAFT_QUESTIONS) {
    activeDraftIndex = draftQuestions.size() - 1;
    markScreenDirty();
    return;
  }

  DraftQuestion draft;
  draft.id = nextQuestionId++;
  draftQuestions.push_back(std::move(draft));
  activeDraftIndex = draftQuestions.size() - 1;
  markScreenDirty();
}

void trimHistory() {
  while ((int)history.size() > MAX_HISTORY_ITEMS) {
    history.erase(history.begin());
    if (currentHistoryIndex > 0) currentHistoryIndex--;
    if (sidebarIndex > 0) sidebarIndex--;
  }
}

// =====================================================
// DISPLAY RENDERING
// =====================================================

void renderBitmapBlock(const DisplayBlock& block) {
  String bitmapKey = String((uintptr_t)block.packedXbm.data()) + ":" +
                     String(block.width) + "x" + String(block.height) + ":" +
                     String(block.xOffset) + ":" + String(block.yOffset);
  if (bitmapKey == lastBitmapKey) {
    return;
  }

  lastBitmapKey = bitmapKey;
  lastOledLine1 = "__bitmap__";
  lastOledLine2 = "";
  u8g2.clearBuffer();
  u8g2.setDrawColor(1);

  int maxX = max(0, block.width - SCREEN_W);
  int xOffset = block.xOffset;
  if (xOffset < 0) xOffset = 0;
  if (xOffset > maxX) xOffset = maxX;

  int maxY = max(0, block.height - SCREEN_H);
  int yOffset = block.yOffset;
  if (yOffset < 0) yOffset = 0;
  if (yOffset > maxY) yOffset = maxY;

  int x = -xOffset;
  int y = -yOffset;
  const uint8_t* dataPtr = block.packedXbm.empty() ? nullptr : block.packedXbm.data();
  if (dataPtr && block.width > 0 && block.height > 0) {
    u8g2.drawXBMP(x, y, block.width, block.height, dataPtr);
  }
  u8g2.sendBuffer();
}

int maxXForBlock(const DisplayBlock& block) {
  return max(0, block.width - SCREEN_W);
}

int maxYForBlock(const DisplayBlock& block) {
  return max(0, block.height - SCREEN_H);
}

bool isSupportedBitmapHeight(int height) {
  return height == 16 || height == SCREEN_H || height == 64;
}

void renderSidebar() {
  if (history.empty()) {
    drawTwoLines("> New question", "CAM: photo");
    return;
  }

  if (sidebarIndex < 0) sidebarIndex = 0;
  if (sidebarIndex >= (int)history.size()) sidebarIndex = history.size() - 1;

  int top = (sidebarIndex / 2) * 2;
  if (top >= (int)history.size()) top = history.size() - 1;

  String line1 = "";
  String line2 = "";

  int idx1 = top;
  int idx2 = min((int)history.size() - 1, top + 1);

  line1 = String(idx1 == sidebarIndex ? ">" : " ") +
          "Q" + String(history[idx1].id) + " " +
          statusLabel(history[idx1]);
  if (idx2 != idx1) {
    line2 = String(idx2 == sidebarIndex ? ">" : " ") +
            "Q" + String(history[idx2].id) + " " +
            statusLabel(history[idx2]);
  } else {
    line2 = "ENTER=open";
  }

  if (line1 != lastOledLine1 || line2 != lastOledLine2) {
    lastOledLine1 = line1;
    lastOledLine2 = line2;
    Serial.printf("[OLED] %s || %s\n", line1.c_str(), line2.c_str());
  }

  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tf);

  bool firstSelected = idx1 == sidebarIndex;
  if (firstSelected) {
    u8g2.setDrawColor(1);
    u8g2.drawBox(0, 0, SCREEN_W, 15);
    u8g2.setDrawColor(0);
  } else {
    u8g2.setDrawColor(1);
  }
  u8g2.drawStr(0, 12, line1.c_str());

  bool secondSelected = idx2 != idx1 && idx2 == sidebarIndex;
  if (secondSelected) {
    u8g2.setDrawColor(1);
    u8g2.drawBox(0, 16, SCREEN_W, 16);
    u8g2.setDrawColor(0);
  } else {
    u8g2.setDrawColor(1);
  }
  u8g2.drawStr(0, 28, line2.c_str());
  u8g2.setDrawColor(1);
  u8g2.sendBuffer();
}

void renderCapture() {
  ensureCaptureSession();
  DraftQuestion* draft = currentDraftQuestion();
  int count = draft ? (int)draft->photos.size() : 0;
  int qid = draft ? draft->id : nextQuestionId;
  String bar = "Q" + String(qid) + " [" + String(count) + "/" + String(MAX_PENDING_PHOTOS) + "]";
  String line2 = (count >= MAX_PENDING_PHOTOS)
    ? "ENT send DIR next"
    : "CAM shot ENT send";
  drawTwoLines(bar, line2);
}

void renderReady() {
  drawTwoLines("CAM: new photo", "PAGE: sidebar");
}

void renderView() {
  ChatItem* item = currentChatItem();
  if (!item) {
    drawTwoLines("No answer yet", "CAM: capture");
    return;
  }

  if (item->thinking) {
    drawTwoLines("Q" + String(item->id), safeShort(item->answer, 21));
    return;
  }

  if (answerBlankMode) {
    renderAnswerBlankMode();
    return;
  }

  if (!item->blocks.empty()) {
    if (item->blockIndex < 0) item->blockIndex = 0;
    if (item->blockIndex >= (int)item->blocks.size()) item->blockIndex = item->blocks.size() - 1;
    renderBitmapBlock(item->blocks[item->blockIndex]);
    return;
  }

  if (!item->fallbackLines.empty()) {
    int maxOffset = max(0, (int)item->fallbackLines.size() - 2);
    if (item->fallbackLineOffset < 0) item->fallbackLineOffset = 0;
    if (item->fallbackLineOffset > maxOffset) item->fallbackLineOffset = maxOffset;

    String l1 = item->fallbackLines[item->fallbackLineOffset];
    String l2 = (item->fallbackLineOffset + 1 < (int)item->fallbackLines.size())
      ? item->fallbackLines[item->fallbackLineOffset + 1]
      : "";
    drawTwoLines(l1, l2);
    return;
  }

  drawTwoLines("No display blocks", "check server");
}

void renderScreen() {
  switch (screenMode) {
    case MODE_BOOT:
      drawTwoLines("Casio AI Machine", "Connecting WiFi");
      break;
    case MODE_READY:
      renderReady();
      break;
    case MODE_CAPTURE:
      renderCapture();
      break;
    case MODE_THINKING:
    case MODE_VIEW:
      renderView();
      break;
    case MODE_SIDEBAR:
      renderSidebar();
      break;
    case MODE_ERROR:
      drawTwoLines("ERROR", lastError);
      break;
  }
}

// =====================================================
// NETWORK + API
// =====================================================

bool connectWiFi() {
  drawTwoLines("Casio AI Machine", "Connecting WiFi");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  unsigned long startedAt = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - startedAt) < WIFI_CONNECT_TIMEOUT_MS) {
    delay(250);
  }
  return WiFi.status() == WL_CONNECTED;
}

bool ensureWiFiConnected() {
  if (WiFi.status() == WL_CONNECTED) return true;
  if (connectWiFi()) return true;
  showError("WiFi failed");
  return false;
}

bool postUploadBinaryOnce(
  const uint8_t* jpegData,
  size_t jpegLen,
  int questionId,
  int photoIndex,
  int& outStatusCode,
  String& outBody
) {
  outStatusCode = -1;
  outBody = "";

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(UPLOAD_HTTP_TIMEOUT_MS / 1000);

  if (!client.connect(SERVER_HOST, 443)) return false;

  String path = String(UPLOAD_PHOTO_PATH) +
                "?question_id=" + String(questionId) +
                "&index=" + String(photoIndex);

  String req;
  req.reserve(420);
  req += "POST ";
  req += path;
  req += " HTTP/1.0\r\n";
  req += "Host: ";
  req += SERVER_HOST;
  req += "\r\nContent-Type: image/jpeg\r\nConnection: close\r\n";
  req += "X-Device-Id: ";
  req += DEVICE_ID;
  req += "\r\nX-Device-Api-Key: ";
  req += DEVICE_API_KEY;
  req += "\r\nX-Question-Id: ";
  req += String(questionId);
  req += "\r\nX-Photo-Index: ";
  req += String(photoIndex);
  req += "\r\nContent-Length: ";
  req += String(jpegLen);
  req += "\r\n\r\n";

  client.print(req);

  size_t sent = 0;
  const size_t chunk = 1024;
  unsigned long writeStart = millis();
  while (sent < jpegLen) {
    size_t want = min(chunk, jpegLen - sent);
    size_t done = 0;
    while (done < want) {
      size_t wrote = client.write(jpegData + sent + done, want - done);
      if (wrote == 0) {
        if ((millis() - writeStart) > UPLOAD_HTTP_TIMEOUT_MS) {
          client.stop();
          return false;
        }
        delay(2);
        continue;
      }
      done += wrote;
      writeStart = millis();
    }
    sent += done;
    delay(1);
  }

  String rawResp = "";
  unsigned long lastDataAt = millis();
  while (true) {
    while (client.available()) {
      rawResp += (char)client.read();
      lastDataAt = millis();
    }

    if (!client.connected() && !client.available()) break;
    if ((millis() - lastDataAt) > UPLOAD_HTTP_TIMEOUT_MS) {
      client.stop();
      return false;
    }
    delay(SOLVE_POLL_DELAY_MS);
  }
  client.stop();

  return parseRawHttpResponse(rawResp, "UP", outStatusCode, outBody);
}

bool uploadPhotoToServer(
  uint8_t* jpegData,
  size_t jpegLen,
  int questionId,
  int photoIndex,
  String& outPhotoId
) {
  for (int attempt = 1; attempt <= UPLOAD_RETRY_COUNT; attempt++) {
    if (!ensureWiFiConnected()) return false;

    int statusCode = -1;
    String responseBody;
    bool posted = postUploadBinaryOnce(jpegData, jpegLen, questionId, photoIndex, statusCode, responseBody);

    if (posted && statusCode >= 200 && statusCode < 300) {
      JsonDocument json;
      if (!deserializeJson(json, responseBody) && (json["ok"] | false)) {
        const char* photoId = json["photo_id"] | "";
        if (photoId && strlen(photoId) > 0) {
          outPhotoId = String(photoId);
          return true;
        }
      }
      Serial.printf("[UP] invalid success body on attempt %d\n", attempt);
    } else {
      Serial.printf("[UP] attempt %d failed posted=%d code=%d len=%u\n",
                    attempt, posted ? 1 : 0, statusCode, (unsigned)jpegLen);
    }

    WiFi.disconnect(false, false);
    delay(120);
    WiFi.reconnect();
    delay(UPLOAD_RETRY_DELAY_MS);
  }

  Serial.println("upload failed after retries");
  return false;
}

bool postSolveWithLongWait(const String& payload, int& outCode, String& outBody) {
  outCode = -1;
  outBody = "";

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(SOLVE_WAIT_TIMEOUT_MS / 1000);

  if (!client.connect(SERVER_HOST, 443)) {
    Serial.println("[SOLVE] TLS connect failed");
    return false;
  }

  String request;
  request.reserve(512 + payload.length());
  request += "POST ";
  request += SOLVE_PATH;
  request += " HTTP/1.0\r\n";
  request += "Host: ";
  request += SERVER_HOST;
  request += "\r\nContent-Type: application/json\r\nConnection: close\r\n";
  request += "X-Device-Id: ";
  request += DEVICE_ID;
  request += "\r\nX-Device-Api-Key: ";
  request += DEVICE_API_KEY;
  request += "\r\nContent-Length: ";
  request += String(payload.length());
  request += "\r\n\r\n";

  client.print(request);
  client.print(payload);

  String rawResp;
  rawResp.reserve(4096);
  unsigned long lastDataAt = millis();
  unsigned long startedAt = millis();
  while (true) {
    while (client.available()) {
      rawResp += (char)client.read();
      lastDataAt = millis();
    }

    if (!client.connected() && !client.available()) break;
    if ((millis() - startedAt) > SOLVE_WAIT_TIMEOUT_MS) {
      Serial.printf("[SOLVE] raw wait timeout raw_len=%u\n", (unsigned)rawResp.length());
      client.stop();
      return false;
    }
    if ((millis() - lastDataAt) > SOLVE_WAIT_TIMEOUT_MS) {
      Serial.printf("[SOLVE] raw idle timeout raw_len=%u\n", (unsigned)rawResp.length());
      client.stop();
      return false;
    }
    delay(SOLVE_POLL_DELAY_MS);
  }
  client.stop();

  bool parsed = parseRawHttpResponse(rawResp, "SOLVE", outCode, outBody);
  Serial.printf("[SOLVE] raw code=%d body_len=%u raw_len=%u\n",
                outCode, (unsigned)outBody.length(), (unsigned)rawResp.length());
  return parsed && outCode > 0;
}

bool parseDisplayBlocksFromResponse(
  JsonVariant displayBlocksVar,
  std::vector<DisplayBlock>& outBlocks
) {
  outBlocks.clear();
  if (!displayBlocksVar.is<JsonArray>()) {
    Serial.println("[OLED] display_blocks missing/not array");
    return false;
  }

  JsonArray arr = displayBlocksVar.as<JsonArray>();
  int pageNo = 0;
  for (JsonVariant item : arr) {
    pageNo++;
    String type = String((const char*)(item["type"] | ""));
    if (type != "bitmap") {
      Serial.printf("[OLED] skip block %d: unsupported type=%s\n", pageNo, type.c_str());
      continue;
    }

    int width = item["width"] | 0;
    int height = item["height"] | 0;
    if (width <= 0 || !isSupportedBitmapHeight(height)) {
      Serial.printf("[OLED] skip block %d: bad size w=%d h=%d\n", pageNo, width, height);
      continue;
    }

    String format = String((const char*)(item["format"] | ""));
    if (format != "1bit_xbm") {
      Serial.printf("[OLED] skip block %d: unsupported format=%s\n", pageNo, format.c_str());
      continue;
    }

    String dataBase64 = String((const char*)(item["data"] | ""));
    if (dataBase64.length() == 0) {
      Serial.printf("[OLED] skip block %d: empty data\n", pageNo);
      continue;
    }

    std::vector<uint8_t> decoded;
    if (!decodeBase64ToBytes(dataBase64, decoded)) {
      Serial.printf("[OLED] skip block %d: base64 decode failed\n", pageNo);
      continue;
    }

    int expected = ((width + 7) / 8) * height;
    if ((int)decoded.size() != expected) {
      Serial.printf("[OLED] skip block %d: bytes=%u expected=%d\n",
                    pageNo, (unsigned)decoded.size(), expected);
      continue;
    }

    DisplayBlock block;
    block.kind = String((const char*)(item["kind"] | "text"));
    block.width = width;
    block.height = height;
    block.xOffset = 0;
    block.yOffset = 0;
    block.packedXbm = std::move(decoded);
    outBlocks.push_back(std::move(block));
  }

  Serial.printf("[OLED] display_blocks parsed=%u/%u\n",
                (unsigned)outBlocks.size(), (unsigned)arr.size());
  return !outBlocks.empty();
}

bool parseDisplayBlockItem(JsonVariant item, int pageNo, DisplayBlock& outBlock) {
  String type = String((const char*)(item["type"] | ""));
  if (type != "bitmap") {
    Serial.printf("[OLED] skip block %d: unsupported type=%s\n", pageNo, type.c_str());
    return false;
  }

  int width = item["width"] | 0;
  int height = item["height"] | 0;
  if (width <= 0 || !isSupportedBitmapHeight(height)) {
    Serial.printf("[OLED] skip block %d: bad size w=%d h=%d\n", pageNo, width, height);
    return false;
  }

  String format = String((const char*)(item["format"] | ""));
  if (format != "1bit_xbm") {
    Serial.printf("[OLED] skip block %d: unsupported format=%s\n", pageNo, format.c_str());
    return false;
  }

  String dataBase64 = String((const char*)(item["data"] | ""));
  if (dataBase64.length() == 0) {
    Serial.printf("[OLED] skip block %d: empty data\n", pageNo);
    return false;
  }

  std::vector<uint8_t> decoded;
  if (!decodeBase64ToBytes(dataBase64, decoded)) {
    Serial.printf("[OLED] skip block %d: base64 decode failed\n", pageNo);
    return false;
  }

  int expected = ((width + 7) / 8) * height;
  if ((int)decoded.size() != expected) {
    Serial.printf("[OLED] skip block %d: bytes=%u expected=%d\n",
                  pageNo, (unsigned)decoded.size(), expected);
    return false;
  }

  outBlock.kind = String((const char*)(item["kind"] | "text"));
  outBlock.width = width;
  outBlock.height = height;
  outBlock.xOffset = 0;
  outBlock.yOffset = 0;
  outBlock.packedXbm = std::move(decoded);
  return true;
}

bool fetchQuestionBlockOnce(int questionId, int blockIndex, int& outCode, String& outBody) {
  outCode = -1;
  outBody = "";

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(UPLOAD_HTTP_TIMEOUT_MS / 1000);

  HTTPClient http;
  http.setTimeout(UPLOAD_HTTP_TIMEOUT_MS);
  http.setReuse(false);

  String url = String("https://") + SERVER_HOST + QUESTION_BLOCK_PATH +
               "?question_id=" + String(questionId) +
               "&index=" + String(blockIndex);
  if (!http.begin(client, url)) {
    Serial.println("[BLOCK] HTTP begin failed");
    return false;
  }

  http.addHeader("Connection", "close");
  http.addHeader("X-Device-Id", DEVICE_ID);
  http.addHeader("X-Device-Api-Key", DEVICE_API_KEY);

  outCode = http.GET();
  outBody = http.getString();
  Serial.printf("[BLOCK] httpclient index=%d code=%d body_len=%u\n",
                blockIndex, outCode, (unsigned)outBody.length());
  http.end();

  return outCode > 0;
}

bool fetchQuestionBlockWithRetry(int questionId, int blockIndex, DisplayBlock& outBlock) {
  unsigned long startedAt = millis();
  int attempt = 1;
  while ((millis() - startedAt) <= RESULT_FETCH_TOTAL_WAIT_MS) {
    if (!ensureWiFiConnected()) return false;

    int code = -1;
    String body;
    bool transportOk = fetchQuestionBlockOnce(questionId, blockIndex, code, body);
    if (transportOk && code >= 200 && code < 300) {
      JsonDocument doc;
      DeserializationError err = deserializeJson(doc, body);
      if (!err && (doc["ok"] | false)) {
        if (parseDisplayBlockItem(doc["block"], blockIndex + 1, outBlock)) {
          return true;
        }
      } else {
        Serial.printf("[BLOCK] json failed index=%d err=%s body[0..120]=%s\n",
                      blockIndex, err ? err.c_str() : "api_not_ok", rawPreview(body, 120).c_str());
      }
    }

    attempt++;
    delay(RESULT_FETCH_RETRY_DELAY_MS);
  }
  return false;
}

bool fetchQuestionBlocksWithRetry(
  int questionId,
  int blockCount,
  std::vector<DisplayBlock>& outBlocks,
  const String& progressiveAnswer = ""
) {
  outBlocks.clear();
  if (blockCount <= 0) return false;
  outBlocks.reserve(blockCount);

  for (int index = 0; index < blockCount; index++) {
    DisplayBlock block;
    if (!fetchQuestionBlockWithRetry(questionId, index, block)) {
      Serial.printf("[BLOCK] failed to fetch block %d/%d\n", index + 1, blockCount);
      outBlocks.clear();
      return false;
    }
    outBlocks.push_back(std::move(block));

    if (progressiveAnswer.length() > 0) {
      lockState();
      for (auto& item : history) {
        if (item.id == questionId) {
          item.thinking = false;
          item.answer = progressiveAnswer;
          item.blocks = outBlocks;
          item.fallbackLines = wrapFallbackLines(progressiveAnswer);
          if (item.blockIndex < 0 || item.blockIndex >= (int)item.blocks.size()) {
            item.blockIndex = 0;
          }
          item.fallbackLineOffset = 0;
          if (currentHistoryIndex < 0 && screenMode == MODE_THINKING) {
            currentHistoryIndex = history.size() - 1;
            sidebarIndex = currentHistoryIndex;
            setScreenMode(MODE_VIEW);
          } else if (screenMode == MODE_THINKING && currentChatItem() && currentChatItem()->id == questionId) {
            setScreenMode(MODE_VIEW);
          }
          markScreenDirty();
          break;
        }
      }
      unlockState();
      Serial.printf("[BLOCK] progressive ready=%u/%d\n",
                    (unsigned)outBlocks.size(), blockCount);
    }
  }

  Serial.printf("[BLOCK] fetched display_blocks=%u/%d\n",
                (unsigned)outBlocks.size(), blockCount);
  return !outBlocks.empty();
}

bool parseSolveJsonBody(
  int questionId,
  const String& body,
  String& outAnswer,
  std::vector<DisplayBlock>& outBlocks
) {
  JsonDocument responseJson;
  DeserializationError err = deserializeJson(responseJson, body);
  if (err) {
    Serial.printf("[AI] json parse failed: %s body[0..200]=%s\n",
                  err.c_str(), rawPreview(body).c_str());
    outAnswer = "AI JSON parse";
    return false;
  }

  bool ok = responseJson["ok"] | false;
  if (!ok) {
    outAnswer = String((const char*)(responseJson["error"] | "AI failed"));
    return false;
  }

  outAnswer = String((const char*)(responseJson["answer"] | ""));
  if (outAnswer.length() == 0) {
    outAnswer = String((const char*)(responseJson["display_text"] | ""));
  }
  if (outAnswer.length() == 0) outAnswer = "AI done";

  bool hasInlineBlocks = parseDisplayBlocksFromResponse(responseJson["display_blocks"], outBlocks);
  int displayBlockCount = responseJson["display_block_count"] | 0;
  bool displayBlocksInline = responseJson["display_blocks_inline"] | true;
  if (!hasInlineBlocks && displayBlockCount > 0) {
    Serial.printf("[OLED] inline blocks unavailable inline=%d count=%d; fetching blocks\n",
                  displayBlocksInline ? 1 : 0, displayBlockCount);
    fetchQuestionBlocksWithRetry(questionId, displayBlockCount, outBlocks, outAnswer);
  }

  JsonVariant usage = responseJson["usage"];
  if (!usage.isNull()) {
    if (usage["input_tokens"].is<int>()) lastPromptTokens = usage["input_tokens"].as<int>();
    if (usage["output_tokens"].is<int>()) lastCompletionTokens = usage["output_tokens"].as<int>();
    if (usage["total_tokens"].is<int>()) lastTotalTokens = usage["total_tokens"].as<int>();
  }

  return true;
}

bool fetchQuestionResultOnce(int questionId, int& outCode, String& outBody) {
  outCode = -1;
  outBody = "";

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(UPLOAD_HTTP_TIMEOUT_MS / 1000);

  HTTPClient http;
  http.setTimeout(UPLOAD_HTTP_TIMEOUT_MS);
  http.setReuse(false);

  String url = String("https://") + SERVER_HOST + QUESTION_RESULT_PATH +
               "?question_id=" + String(questionId);
  if (!http.begin(client, url)) {
    Serial.println("[RESULT] HTTP begin failed");
    return false;
  }

  http.addHeader("Connection", "close");
  http.addHeader("X-Device-Id", DEVICE_ID);
  http.addHeader("X-Device-Api-Key", DEVICE_API_KEY);

  outCode = http.GET();
  outBody = http.getString();
  Serial.printf("[RESULT] httpclient code=%d body_len=%u\n",
                outCode, (unsigned)outBody.length());
  http.end();

  return outCode > 0;
}

bool fetchQuestionResultWithRetry(
  int questionId,
  String& outAnswer,
  std::vector<DisplayBlock>& outBlocks
) {
  unsigned long startedAt = millis();
  int attempt = 1;
  while ((millis() - startedAt) <= RESULT_FETCH_TOTAL_WAIT_MS) {
    if (!ensureWiFiConnected()) return false;

    int code = -1;
    String body;
    unsigned long t0 = millis();
    bool transportOk = fetchQuestionResultOnce(questionId, code, body);
    Serial.printf("[TIME] result_fetch_%d=%lums ok=%d code=%d\n",
                  attempt, (unsigned long)(millis() - t0), transportOk ? 1 : 0, code);

    if (transportOk) {
      if (code >= 200 && code < 300 && parseSolveJsonBody(questionId, body, outAnswer, outBlocks)) {
        Serial.println("[RESULT] fetched completed answer");
        return true;
      }

      JsonDocument statusDoc;
      DeserializationError err = deserializeJson(statusDoc, body);
      String status = err ? "" : String((const char*)(statusDoc["status"] | ""));
      String error = err ? "" : String((const char*)(statusDoc["error"] | ""));
      if (status == "FAILED" || code >= 500) {
        outAnswer = error.length() ? error : "AI failed";
        Serial.printf("[RESULT] terminal failure status=%s error=%s\n",
                      status.c_str(), error.c_str());
        return false;
      }
    }

    if (body.length() > 0) {
      Serial.printf("[RESULT] body[0..200]=%s\n", rawPreview(body).c_str());
    }
    attempt++;
    delay(RESULT_FETCH_RETRY_DELAY_MS);
  }
  return false;
}

bool requestSolveFromServer(
  int questionId,
  const std::vector<String>& photoIds,
  String& outAnswer,
  std::vector<DisplayBlock>& outBlocks
) {
  if (!ensureWiFiConnected()) return false;

  JsonDocument requestJson;
  requestJson["device_id"] = DEVICE_ID;
  requestJson["question_id"] = questionId;
  requestJson["photo_count"] = (int)photoIds.size();
  requestJson["mode"] = "oled-v2";

  JsonArray photoIdArray = requestJson["photo_ids"].to<JsonArray>();
  for (const String& id : photoIds) photoIdArray.add(id);

  String contextTail = "";
  lockState();
  int start = max(0, (int)history.size() - 3);
  for (int i = start; i < (int)history.size(); i++) {
    if (!history[i].thinking) {
      contextTail += "Q" + String(history[i].id) + ": ";
      contextTail += history[i].answer.substring(0, 300);
      contextTail += "\n";
    }
  }
  unlockState();
  requestJson["context_tail"] = contextTail;

  String payload;
  serializeJson(requestJson, payload);

  int statusCode = -1;
  String body;
  if (!postSolveWithLongWait(payload, statusCode, body)) {
    Serial.println("[AI] solve transport failed; trying result fetch fallback");
    if (fetchQuestionResultWithRetry(questionId, outAnswer, outBlocks)) return true;
    outAnswer = "AI timeout";
    return false;
  }

  if (statusCode < 200 || statusCode >= 300) {
    Serial.printf("[AI] solve HTTP %d; trying result fetch fallback\n", statusCode);
    if (fetchQuestionResultWithRetry(questionId, outAnswer, outBlocks)) return true;
    outAnswer = "AI HTTP " + String(statusCode);
    return false;
  }

  if (parseSolveJsonBody(questionId, body, outAnswer, outBlocks)) return true;

  Serial.println("[AI] solve body parse failed; trying result fetch fallback");
  if (fetchQuestionResultWithRetry(questionId, outAnswer, outBlocks)) return true;
  return false;
}

// =====================================================
// CAMERA
// =====================================================

bool initCamera() {
  camera_config_t config;
  bool hasPsram = psramFound();

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
  config.frame_size = hasPsram ? FRAMESIZE_SXGA : FRAMESIZE_XGA;
  config.jpeg_quality = CAM_CAPTURE_JPEG_QUALITY;
  config.fb_count = 1;
  config.fb_location = hasPsram ? CAMERA_FB_IN_PSRAM : CAMERA_FB_IN_DRAM;
  config.grab_mode = CAMERA_GRAB_LATEST;

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) return false;

  sensor_t* s = esp_camera_sensor_get();
  if (s) {
    s->set_framesize(s, hasPsram ? FRAMESIZE_SXGA : FRAMESIZE_XGA);
    s->set_quality(s, CAM_CAPTURE_JPEG_QUALITY);
    s->set_whitebal(s, 1);
    s->set_awb_gain(s, 1);
    s->set_wb_mode(s, 0);
    s->set_brightness(s, 1);
    s->set_contrast(s, 2);
    s->set_saturation(s, 0);
    s->set_exposure_ctrl(s, 1);
    s->set_gain_ctrl(s, 1);
    s->set_aec2(s, 1);
    s->set_ae_level(s, 2);
    s->set_gainceiling(s, GAINCEILING_2X);
    if (s->set_sharpness) s->set_sharpness(s, -2);
    if (s->set_denoise) s->set_denoise(s, 2);
    if (s->set_bpc) s->set_bpc(s, 1);
    if (s->set_wpc) s->set_wpc(s, 1);
    if (s->set_lenc) s->set_lenc(s, 1);
    if (s->set_raw_gma) s->set_raw_gma(s, 1);
    if (s->set_vflip) s->set_vflip(s, 1);
    if (s->set_hmirror) s->set_hmirror(s, 0);
  }

  Serial.printf("[CAM] init ok frame=%s q=%d b=%d c=%d s=%d ae=%d gc=2x sharp=-2 dn=2\n",
                hasPsram ? "SXGA" : "XGA",
                CAM_CAPTURE_JPEG_QUALITY, 1, 2, 0, 2);

  cameraSessionOn = true;
  return true;
}

void sensorStandby() {
  sensor_t* s = esp_camera_sensor_get();
  if (!s || !s->set_reg) return;

  // OV5640 software standby: reduce sensor activity before deinit.
  s->set_reg(s, 0x3008, 0xFF, 0x42);
  delay(80);
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

void stopCameraForCooling() {
  if (cameraSessionOn) {
    sensorStandby();
    esp_camera_deinit();
    cameraSessionOn = false;
  }
  quietCameraPins();
}

bool ensureCameraReady() {
  if (cameraSessionOn) return true;
  if (!initCamera()) return false;
  return true;
}

camera_fb_t* captureFrameWithRetryAndFallback(sensor_t* s) {
  for (int i = 0; i < CAM_CAPTURE_RETRIES; i++) {
    camera_fb_t* fb = esp_camera_fb_get();
    if (fb) return fb;
    Serial.printf("[CAM] final capture retry %d/%d failed\n", i + 1, CAM_CAPTURE_RETRIES);
    delay(120);
  }

  if (s && s->set_framesize) {
    Serial.println("[CAM] fallback to XGA q12 and retry capture");
    s->set_framesize(s, FRAMESIZE_XGA);
    if (s->set_quality) s->set_quality(s, CAM_FALLBACK_JPEG_QUALITY);
    delay(700);

    for (int i = 0; i < 2; i++) {
      camera_fb_t* warm = esp_camera_fb_get();
      if (warm) esp_camera_fb_return(warm);
      delay(180);
    }

    for (int i = 0; i < CAM_CAPTURE_RETRIES; i++) {
      camera_fb_t* fb = esp_camera_fb_get();
      if (fb) return fb;
      Serial.printf("[CAM] XGA retry %d/%d failed\n", i + 1, CAM_CAPTURE_RETRIES);
      delay(120);
    }
  }

  return nullptr;
}

bool captureOnePhotoToPsr() {
  if (captureBusy) return false;

  ensureCaptureSession();
  DraftQuestion* draft = currentDraftQuestion();
  if (!draft) return false;
  if ((int)draft->photos.size() >= MAX_PENDING_PHOTOS) return false;

  uint32_t t0 = millis();
  if (!ensureCameraReady()) {
    showError("Camera init fail");
    return false;
  }
  uint32_t tInit = millis() - t0;

  captureBusy = true;
  setScreenMode(MODE_CAPTURE);
  drawTwoLines("Capturing...", "Please wait");
  delay(CAM_SETTLE_AFTER_INIT_MS);
  uint32_t tWarmupStart = millis();

  sensor_t* s = esp_camera_sensor_get();

  // warm-up frames for AE/AWB settle: temporary low quality to save bandwidth/time.
  if (s && s->set_quality) s->set_quality(s, CAM_WARMUP_JPEG_QUALITY);
  for (int i = 0; i < CAM_WARMUP_FRAMES; i++) {
    camera_fb_t* warm = esp_camera_fb_get();
    if (warm) esp_camera_fb_return(warm);
    delay(CAM_WARMUP_DELAY_MS);
  }
  if (s && s->set_quality) {
    s->set_quality(s, CAM_CAPTURE_JPEG_QUALITY);
    delay(500);
  }
  uint32_t tWarmup = millis() - tWarmupStart;

  uint32_t tCaptureStart = millis();
  camera_fb_t* fb = captureFrameWithRetryAndFallback(s);
  uint32_t tCapture = millis() - tCaptureStart;
  if (!fb) {
    captureBusy = false;
    stopCameraForCooling();
    showError("Capture failed");
    return false;
  }

  SolveJob::PendingPhoto photo;
  photo.data = (uint8_t*)ps_malloc(fb->len);
  if (!photo.data) photo.data = (uint8_t*)malloc(fb->len);
  if (!photo.data) {
    esp_camera_fb_return(fb);
    captureBusy = false;
    stopCameraForCooling();
    showError("No PSRAM");
    return false;
  }
  memcpy(photo.data, fb->buf, fb->len);
  photo.len = fb->len;
  photo.width = fb->width;
  photo.height = fb->height;

  esp_camera_fb_return(fb);
  stopCameraForCooling();

  draft->photos.push_back(photo);

  uint32_t tTotal = millis() - t0;
  Serial.printf(
    "[TIME] cam_init=%lums warmup=%lums capture=%lums total=%lums\n",
    (unsigned long)tInit,
    (unsigned long)tWarmup,
    (unsigned long)tCapture,
    (unsigned long)tTotal
  );
  Serial.printf(
    "[CAM] saved #%d bytes=%u w=%u h=%u (pending in PSRAM)\n",
    (int)draft->photos.size(),
    (unsigned)photo.len,
    photo.width,
    photo.height
  );

  captureBusy = false;
  setScreenMode(MODE_CAPTURE);
  markScreenDirty();
  return true;
}

// =====================================================
// SOLVE FLOW
// =====================================================

void solveTask(void* param) {
  SolveJob* job = (SolveJob*)param;

  String answer = "";
  std::vector<DisplayBlock> blocks;
  std::vector<String> photoIds;
  photoIds.reserve(job->photos.size());
  bool ok = true;

  uint32_t uploadTotalStart = millis();
  for (int i = 0; i < (int)job->photos.size(); i++) {
    lockState();
    for (auto& item : history) {
      if (item.id == job->questionId && item.thinking) {
        item.answer = "Uploading " + String(i + 1) + "/" + String(job->photos.size());
        item.fallbackLines = wrapFallbackLines(item.answer);
        markScreenDirty();
        break;
      }
    }
    unlockState();

    auto& p = job->photos[i];
    uint32_t tUploadStart = millis();
    String photoId;
    bool uploaded = uploadPhotoToServer(
      p.data,
      p.len,
      job->questionId,
      i,
      photoId
    );
    uint32_t tUpload = millis() - tUploadStart;
    Serial.printf("[TIME] upload[%d]=%lums bytes=%u ok=%d\n", i, (unsigned long)tUpload, (unsigned)p.len, uploaded ? 1 : 0);
    freePendingPhoto(p);

    if (!uploaded) {
      ok = false;
      answer = "Upload failed";
      break;
    }
    photoIds.push_back(photoId);
  }
  clearPendingPhotos(job->photos);
  Serial.printf("[TIME] upload_total=%lums photos=%d\n", (unsigned long)(millis() - uploadTotalStart), (int)photoIds.size());
  if (!ok) {
    Serial.printf("[AI] upload stage failed: %s\n", answer.c_str());
  }

  if (ok) {
    lockState();
    for (auto& item : history) {
      if (item.id == job->questionId && item.thinking) {
        item.answer = "Thinking...";
        item.fallbackLines = wrapFallbackLines(item.answer);
        markScreenDirty();
        break;
      }
    }
    unlockState();

    uint32_t tSolveStart = millis();
    ok = requestSolveFromServer(job->questionId, photoIds, answer, blocks);
    Serial.printf("[TIME] solve=%lums ok=%d\n", (unsigned long)(millis() - tSolveStart), ok ? 1 : 0);
    if (ok) {
      Serial.println("=== AI FULL ANSWER BEGIN ===");
      Serial.println(answer);
      Serial.println("=== AI FULL ANSWER END ===");
    } else {
      Serial.printf("[AI] solve failed message: %s\n", answer.c_str());
    }
  }

  lockState();
  for (auto& item : history) {
    if (item.id == job->questionId) {
      item.thinking = false;
      if (ok) {
        item.answer = answer;
        item.blocks = std::move(blocks);
        item.fallbackLines = wrapFallbackLines(answer);
        item.blockIndex = 0;
        item.fallbackLineOffset = 0;
      } else {
        item.answer = "Error: " + answer;
        item.blocks.clear();
        item.fallbackLines = wrapFallbackLines(item.answer);
      }
      break;
    }
  }

  if (activeSolveJobs > 0) activeSolveJobs--;
  syncSolveRunningFlag();
  if (screenMode == MODE_THINKING && (currentHistoryIndex < 0 || currentHistoryIndex >= (int)history.size())) {
    currentHistoryIndex = history.size() - 1;
    sidebarIndex = currentHistoryIndex;
    setScreenMode(MODE_VIEW);
  } else if (screenMode == MODE_THINKING && currentChatItem() && currentChatItem()->id == job->questionId) {
    setScreenMode(MODE_VIEW);
  }
  markScreenDirty();
  unlockState();

  delete job;
  vTaskDelete(NULL);
}

bool startSolveJobForDraft(DraftQuestion& draft, bool selectIfFirst) {
  if (draft.photos.empty() || draft.id == 0) return false;
  if (activeSolveJobs >= MAX_CONCURRENT_SOLVE_JOBS) return false;

  ChatItem item;
  item.id = draft.id;
  item.photoCount = draft.photos.size();
  item.thinking = true;
  item.answer = "Uploading...";
  item.fallbackLines = wrapFallbackLines(item.answer);

  history.push_back(item);
  trimHistory();
  int newHistoryIndex = history.size() - 1;
  if (selectIfFirst || currentHistoryIndex < 0) {
    currentHistoryIndex = newHistoryIndex;
    sidebarIndex = currentHistoryIndex;
  }

  SolveJob* job = new SolveJob();
  job->questionId = draft.id;
  job->photoCount = draft.photos.size();
  job->photos = std::move(draft.photos);

  activeSolveJobs++;
  syncSolveRunningFlag();

  BaseType_t taskOk = xTaskCreatePinnedToCore(
    solveTask,
    "solveTask",
    20000,
    job,
    1,
    NULL,
    1
  );
  if (taskOk != pdPASS) {
    clearPendingPhotos(job->photos);
    delete job;
    if (activeSolveJobs > 0) activeSolveJobs--;
    syncSolveRunningFlag();
    showError("Task create fail");
    return false;
  }

  return true;
}

void startSolveFromDrafts() {
  if (!hasAnyDraftPhotos()) return;

  lockState();

  bool selectedFirstStarted = false;
  int startedJobs = 0;
  for (auto& draft : draftQuestions) {
    if (draft.photos.empty()) continue;
    bool started = startSolveJobForDraft(draft, !selectedFirstStarted);
    if (started) {
      selectedFirstStarted = true;
      startedJobs++;
    }
  }

  draftQuestions.clear();
  activeDraftIndex = -1;
  if (startedJobs > 0) {
    setScreenMode(MODE_THINKING);
  }
  markScreenDirty();

  unlockState();
}

// =====================================================
// BUTTON ACTIONS
// =====================================================

void actionUp() {
  answerBlankMode = false;
  if (screenMode == MODE_CAPTURE) {
    moveToNextDraftQuestion();
    return;
  }
  if (screenMode == MODE_SIDEBAR) {
    if (sidebarIndex > 0) sidebarIndex--;
    return;
  }

  if (screenMode != MODE_VIEW && screenMode != MODE_THINKING) return;
  ChatItem* item = currentChatItem();
  if (!item || item->thinking) return;

  if (!item->blocks.empty()) {
    DisplayBlock& currentBlock = item->blocks[item->blockIndex];
    if (currentBlock.yOffset > 0) {
      currentBlock.yOffset -= SCREEN_H;
      if (currentBlock.yOffset < 0) currentBlock.yOffset = 0;
      return;
    }
    if (item->blockIndex > 0) {
      int previousX = currentBlock.xOffset;
      int previousWidth = currentBlock.width;
      String previousKind = currentBlock.kind;
      item->blockIndex--;
      DisplayBlock& nextBlock = item->blocks[item->blockIndex];
      bool sameScrollablePair =
        nextBlock.width == previousWidth && nextBlock.kind == previousKind;
      nextBlock.xOffset = sameScrollablePair ? min(previousX, maxXForBlock(nextBlock)) : 0;
      nextBlock.yOffset = maxYForBlock(nextBlock);
    }
    return;
  }

  if (item->fallbackLineOffset > 0) item->fallbackLineOffset -= 2;
}

void actionDown() {
  answerBlankMode = false;
  if (screenMode == MODE_CAPTURE) {
    moveToNextDraftQuestion();
    return;
  }
  if (screenMode == MODE_SIDEBAR) {
    if (sidebarIndex < (int)history.size() - 1) sidebarIndex++;
    return;
  }

  if (screenMode != MODE_VIEW && screenMode != MODE_THINKING) return;
  ChatItem* item = currentChatItem();
  if (!item || item->thinking) return;

  if (!item->blocks.empty()) {
    DisplayBlock& currentBlock = item->blocks[item->blockIndex];
    int maxY = maxYForBlock(currentBlock);
    if (currentBlock.yOffset < maxY) {
      currentBlock.yOffset += SCREEN_H;
      if (currentBlock.yOffset > maxY) currentBlock.yOffset = maxY;
      return;
    }
    if (item->blockIndex < (int)item->blocks.size() - 1) {
      int previousX = currentBlock.xOffset;
      int previousWidth = currentBlock.width;
      String previousKind = currentBlock.kind;
      item->blockIndex++;
      DisplayBlock& nextBlock = item->blocks[item->blockIndex];
      bool sameScrollablePair =
        nextBlock.width == previousWidth && nextBlock.kind == previousKind;
      nextBlock.xOffset = sameScrollablePair ? min(previousX, maxXForBlock(nextBlock)) : 0;
      nextBlock.yOffset = 0;
    }
    return;
  }

  int maxOffset = max(0, (int)item->fallbackLines.size() - 2);
  if (item->fallbackLineOffset < maxOffset) item->fallbackLineOffset += 2;
  if (item->fallbackLineOffset > maxOffset) item->fallbackLineOffset = maxOffset;
}

void actionLeft() {
  answerBlankMode = false;
  if (screenMode == MODE_CAPTURE) {
    moveToNextDraftQuestion();
    return;
  }
  if (screenMode != MODE_VIEW && screenMode != MODE_THINKING) return;
  if (history.empty()) return;

  ChatItem* item = currentChatItem();
  if (item && !item->thinking && !item->blocks.empty()) {
    DisplayBlock& block = item->blocks[item->blockIndex];
    if (block.xOffset > 0) {
      block.xOffset -= 32;
      if (block.xOffset < 0) block.xOffset = 0;
      return;
    }
    if (item->blockIndex > 0) {
      int previousWidth = block.width;
      String previousKind = block.kind;
      item->blockIndex--;
      DisplayBlock& nextBlock = item->blocks[item->blockIndex];
      bool sameScrollablePair =
        nextBlock.width == previousWidth && nextBlock.kind == previousKind;
      nextBlock.xOffset = sameScrollablePair ? 0 : maxXForBlock(nextBlock);
    }
    return;
  }

  if (currentHistoryIndex > 0) {
    currentHistoryIndex--;
    sidebarIndex = currentHistoryIndex;
  }
}

void actionRight() {
  answerBlankMode = false;
  if (screenMode == MODE_CAPTURE) {
    moveToNextDraftQuestion();
    return;
  }
  if (screenMode != MODE_VIEW && screenMode != MODE_THINKING) return;
  if (history.empty()) return;

  ChatItem* item = currentChatItem();
  if (item && !item->thinking && !item->blocks.empty()) {
    DisplayBlock& block = item->blocks[item->blockIndex];
    int maxX = maxXForBlock(block);
    if (block.xOffset < maxX) {
      block.xOffset += 32;
      if (block.xOffset > maxX) block.xOffset = maxX;
      return;
    }
    if (item->blockIndex < (int)item->blocks.size() - 1) {
      int previousX = block.xOffset;
      int previousWidth = block.width;
      String previousKind = block.kind;
      item->blockIndex++;
      DisplayBlock& nextBlock = item->blocks[item->blockIndex];
      bool sameScrollablePair =
        nextBlock.width == previousWidth && nextBlock.kind == previousKind;
      nextBlock.xOffset = sameScrollablePair ? min(previousX, maxXForBlock(nextBlock)) : 0;
    }
    return;
  }

  if (currentHistoryIndex < (int)history.size() - 1) {
    currentHistoryIndex++;
    sidebarIndex = currentHistoryIndex;
  }
}

void actionCamera() {
  if (screenMode == MODE_ERROR) return;
  answerBlankMode = false;
  if (screenMode != MODE_CAPTURE) {
    ensureCaptureSession();
    setScreenMode(MODE_CAPTURE);
    return;
  }
  captureOnePhotoToPsr();
}

void actionEnter() {
  if (screenMode == MODE_SIDEBAR) {
    answerBlankMode = false;
    if (history.empty()) {
      setScreenMode(hasAnyDraftPhotos() ? MODE_CAPTURE : MODE_READY);
      return;
    }
    currentHistoryIndex = sidebarIndex;
    setScreenMode(MODE_VIEW);
    return;
  }

  if (screenMode == MODE_VIEW) {
    ChatItem* item = currentChatItem();
    if (item && !item->thinking) {
      answerBlankMode = !answerBlankMode;
      return;
    }
  }

  if (hasAnyDraftPhotos()) {
    answerBlankMode = false;
    startSolveFromDrafts();
  }
}

void actionPage() {
  if (screenMode == MODE_ERROR) return;
  answerBlankMode = false;

  if (screenMode == MODE_SIDEBAR) {
    if (!history.empty()) {
      if (currentHistoryIndex < 0 || currentHistoryIndex >= (int)history.size()) {
        currentHistoryIndex = history.size() - 1;
      }
      setScreenMode(MODE_VIEW);
    } else {
      setScreenMode(hasAnyDraftPhotos() ? MODE_CAPTURE : MODE_READY);
    }
    return;
  }

  if (history.empty()) {
    setScreenMode(hasAnyDraftPhotos() ? MODE_CAPTURE : MODE_READY);
    return;
  }

  if (currentHistoryIndex < 0 || currentHistoryIndex >= (int)history.size()) {
    currentHistoryIndex = history.size() - 1;
  }
  sidebarIndex = currentHistoryIndex;
  setScreenMode(MODE_SIDEBAR);
}

// =====================================================
// SETUP + LOOP
// =====================================================

void setup() {
  Serial.begin(115200);
  delay(700);

  stateMutex = xSemaphoreCreateMutex();

  btnUp.begin(BTN_SCROLL_UP);
  btnDown.begin(BTN_SCROLL_DOWN);
  btnLeft.begin(BTN_SCROLL_LEFT);
  btnRight.begin(BTN_SCROLL_RIGHT);
  btnCamera.begin(BTN_CAMERA);
  btnEnter.begin(BTN_ENTER);
  btnPage.begin(BTN_PAGE);

  Wire.begin(OLED_SDA, OLED_SCL);
  // Keep OLED I2C conservative; long jumper wires can corrupt fast refreshes.
  Wire.setClock(100000);

  u8g2.setI2CAddress(0x3C * 2);
  u8g2.begin();
  u8g2.setContrast(OLED_CONTRAST);
  u8g2.enableUTF8Print();

  screenMode = MODE_BOOT;
  markScreenDirty();
  renderScreen();
  delay(900);
  quietCameraPins();

  if (!connectWiFi()) {
    showError("WiFi failed");
    renderScreen();
    return;
  }

  setScreenMode(MODE_SIDEBAR);
  renderScreen();
}

void loop() {
  bool inputHandled = false;
  if (btnUp.fell() || btnUp.repeat()) {
    Serial.println("[BTN] UP");
    actionUp();
    inputHandled = true;
  }
  if (btnDown.fell() || btnDown.repeat()) {
    Serial.println("[BTN] DOWN");
    actionDown();
    inputHandled = true;
  }
  if (btnLeft.fell() || btnLeft.repeat()) {
    Serial.println("[BTN] PREV");
    actionLeft();
    inputHandled = true;
  }
  if (btnRight.fell() || btnRight.repeat()) {
    Serial.println("[BTN] NEXT");
    actionRight();
    inputHandled = true;
  }
  if (btnCamera.fell()) {
    Serial.println("[BTN] CAMERA");
    actionCamera();
    inputHandled = true;
  }
  if (btnEnter.fell()) {
    Serial.println("[BTN] ENTER");
    actionEnter();
    inputHandled = true;
  }
  if (btnPage.fell()) {
    Serial.println("[BTN] PAGE");
    actionPage();
    inputHandled = true;
  }

  if (inputHandled) {
    markScreenDirty();
  }

  if (screenMode == MODE_READY && hasAnyDraftPhotos()) {
    setScreenMode(MODE_CAPTURE);
  }
  if (screenDirty) {
    renderScreen();
    screenDirty = false;
  }
  delay(20);
}
