#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include "esp_camera.h"
#include <U8g2lib.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <vector>
#include "mbedtls/base64.h"

// =====================================================
// USER CONFIG
// =====================================================

const char* WIFI_SSID = "Henry Teo";
const char* WIFI_PASS = "henrycute";

const char* SERVER_HOST = "accelertechnology.my";

const char* DEVICE_API_KEY = "21326a10-c7f8-4ca9-8e7c-d6f55c15d564";
const char* DEVICE_ID = "CASIO_AI_MACHINE_001";

const char* UPLOAD_PHOTO_PATH = "/api/casio-ai/upload-photo";
const char* SOLVE_PATH = "/api/casio-ai/solve";

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

#define BUTTON_DEBOUNCE_MS 35
#define OLED_CLEAR_SETTLE_MS 8
#define OLED_CONTRAST 0
#define WIFI_CONNECT_TIMEOUT_MS 15000
#define UPLOAD_HTTP_TIMEOUT_MS 60000
#define UPLOAD_RETRY_COUNT 3
#define UPLOAD_RETRY_DELAY_MS 700
#define SOLVE_WAIT_TIMEOUT_MS 300000UL
#define SOLVE_POLL_DELAY_MS 20

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

    if ((millis() - lastChange) >= BUTTON_DEBOUNCE_MS && reading != lastStable) {
      lastStable = reading;
      return lastStable == LOW;
    }
    return false;
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

int activeQuestionId = 0;
int nextQuestionId = 1;
std::vector<SolveJob::PendingPhoto> pendingPhotos;

bool captureBusy = false;
bool solveRunning = false;
bool cameraSessionOn = false;
bool answerBlankMode = false;

int lastPromptTokens = -1;
int lastCompletionTokens = -1;
int lastTotalTokens = -1;

String lastOledLine1 = "";
String lastOledLine2 = "";

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

bool ensureWiFiConnected();

void showError(const String& error) {
  lastError = safeShort(error, 21);
  screenMode = MODE_ERROR;
}

void ensureCaptureSession() {
  if (activeQuestionId == 0) {
    activeQuestionId = nextQuestionId++;
  }
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
  clearOledFrame();
  u8g2.clearBuffer();
  u8g2.setDrawColor(1);

  int maxX = max(0, block.width - SCREEN_W);
  int xOffset = block.xOffset;
  if (xOffset < 0) xOffset = 0;
  if (xOffset > maxX) xOffset = maxX;

  int x = -xOffset;
  const uint8_t* dataPtr = block.packedXbm.empty() ? nullptr : block.packedXbm.data();
  if (dataPtr && block.width > 0 && block.height > 0) {
    u8g2.drawXBMP(x, 0, block.width, block.height, dataPtr);
  }
  u8g2.sendBuffer();
}

int maxXForBlock(const DisplayBlock& block) {
  return max(0, block.width - SCREEN_W);
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
          answerPreview(history[idx1], SIDEBAR_SNIPPET_CHARS);
  if (idx2 != idx1) {
    line2 = String(idx2 == sidebarIndex ? ">" : " ") +
            "Q" + String(history[idx2].id) + " " +
            answerPreview(history[idx2], SIDEBAR_SNIPPET_CHARS);
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
  int count = (int)pendingPhotos.size();
  String bar = "[" + String(count) + "/" + String(MAX_PENDING_PHOTOS) + "] ";
  if (count == 0) bar += "no photo";
  else bar += "captured";

  String line2 = (count >= MAX_PENDING_PHOTOS) ? "ENTER=send max" : "CAM=shot ENTER=send";
  drawTwoLines(bar, line2);
}

void renderReady() {
  drawTwoLines("CAM: take photo", "ENTER: send to AI");
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

  if (rawResp.length() == 0) return false;

  int headerEnd = rawResp.indexOf("\r\n\r\n");
  if (headerEnd >= 0) {
    String statusLine = rawResp.substring(0, rawResp.indexOf('\n'));
    statusLine.trim();
    outStatusCode = parseHttpStatusCode(statusLine);
    outBody = rawResp.substring(headerEnd + 4);
    if (outStatusCode <= 0) return false;
    return true;
  }

  // Fallback for non-standard/simple responses: treat payload as body.
  outStatusCode = 200;
  outBody = rawResp;
  return true;
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

  if (!client.connect(SERVER_HOST, 443)) return false;

  String req;
  req.reserve(512 + payload.length());
  req += "POST ";
  req += SOLVE_PATH;
  req += " HTTP/1.0\r\n";
  req += "Host: ";
  req += SERVER_HOST;
  req += "\r\nContent-Type: application/json\r\nConnection: close\r\n";
  req += "X-Device-Id: ";
  req += DEVICE_ID;
  req += "\r\nX-Device-Api-Key: ";
  req += DEVICE_API_KEY;
  req += "\r\nContent-Length: ";
  req += String(payload.length());
  req += "\r\n\r\n";

  client.print(req);
  client.print(payload);

  String rawResp = "";
  unsigned long lastDataAt = millis();
  while (true) {
    while (client.available()) {
      rawResp += (char)client.read();
      lastDataAt = millis();
    }

    if (!client.connected() && !client.available()) break;
    if ((millis() - lastDataAt) > SOLVE_WAIT_TIMEOUT_MS) {
      client.stop();
      return false;
    }
    delay(SOLVE_POLL_DELAY_MS);
  }
  client.stop();

  if (rawResp.length() == 0) return false;

  int headerEnd = rawResp.indexOf("\r\n\r\n");
  if (headerEnd >= 0) {
    String statusLine = rawResp.substring(0, rawResp.indexOf('\n'));
    statusLine.trim();
    outCode = parseHttpStatusCode(statusLine);
    outBody = rawResp.substring(headerEnd + 4);
    if (outCode <= 0) return false;
    return true;
  }

  // Fallback for simple/non-standard responses.
  outCode = 200;
  outBody = rawResp;
  return true;
}

bool parseDisplayBlocksFromResponse(
  JsonVariant displayBlocksVar,
  std::vector<DisplayBlock>& outBlocks
) {
  outBlocks.clear();
  if (!displayBlocksVar.is<JsonArray>()) return false;

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
    if (width <= 0 || height != SCREEN_H) {
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
    block.packedXbm = std::move(decoded);
    outBlocks.push_back(std::move(block));
  }

  Serial.printf("[OLED] display_blocks parsed=%u/%u\n",
                (unsigned)outBlocks.size(), (unsigned)arr.size());
  return !outBlocks.empty();
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
    outAnswer = "AI timeout";
    return false;
  }

  if (statusCode < 200 || statusCode >= 300) {
    outAnswer = "AI HTTP " + String(statusCode);
    return false;
  }

  JsonDocument responseJson;
  if (deserializeJson(responseJson, body)) {
    outAnswer = body;
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

  parseDisplayBlocksFromResponse(responseJson["display_blocks"], outBlocks);

  JsonVariant usage = responseJson["usage"];
  if (!usage.isNull()) {
    if (usage["input_tokens"].is<int>()) lastPromptTokens = usage["input_tokens"].as<int>();
    if (usage["output_tokens"].is<int>()) lastCompletionTokens = usage["output_tokens"].as<int>();
    if (usage["total_tokens"].is<int>()) lastTotalTokens = usage["total_tokens"].as<int>();
  }

  return true;
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
  if (captureBusy || solveRunning) return false;
  if ((int)pendingPhotos.size() >= MAX_PENDING_PHOTOS) return false;

  ensureCaptureSession();
  uint32_t t0 = millis();
  if (!ensureCameraReady()) {
    showError("Camera init fail");
    return false;
  }
  uint32_t tInit = millis() - t0;

  captureBusy = true;
  screenMode = MODE_CAPTURE;
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

  pendingPhotos.push_back(photo);

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
    (int)pendingPhotos.size(),
    (unsigned)photo.len,
    photo.width,
    photo.height
  );

  captureBusy = false;
  screenMode = MODE_CAPTURE;
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

  solveRunning = false;
  screenMode = MODE_VIEW;
  unlockState();

  delete job;
  vTaskDelete(NULL);
}

void startSolveFromPending() {
  if (solveRunning) return;
  if (pendingPhotos.empty() || activeQuestionId == 0) return;

  lockState();

  ChatItem item;
  item.id = activeQuestionId;
  item.photoCount = pendingPhotos.size();
  item.thinking = true;
  item.answer = "Uploading...";
  item.fallbackLines = wrapFallbackLines(item.answer);

  history.push_back(item);
  trimHistory();
  currentHistoryIndex = history.size() - 1;
  sidebarIndex = currentHistoryIndex;

  SolveJob* job = new SolveJob();
  job->questionId = activeQuestionId;
  job->photoCount = pendingPhotos.size();
  job->photos = std::move(pendingPhotos);

  activeQuestionId = 0;
  solveRunning = true;
  screenMode = MODE_THINKING;

  unlockState();

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
    solveRunning = false;
    showError("Task create fail");
  }
}

// =====================================================
// BUTTON ACTIONS
// =====================================================

void actionUp() {
  answerBlankMode = false;
  if (screenMode == MODE_SIDEBAR) {
    if (sidebarIndex > 0) sidebarIndex--;
    return;
  }

  if (screenMode != MODE_VIEW && screenMode != MODE_THINKING) return;
  ChatItem* item = currentChatItem();
  if (!item || item->thinking) return;

  if (!item->blocks.empty()) {
    if (item->blockIndex > 0) {
      item->blockIndex--;
      item->blocks[item->blockIndex].xOffset = 0;
    }
    return;
  }

  if (item->fallbackLineOffset > 0) item->fallbackLineOffset -= 2;
}

void actionDown() {
  answerBlankMode = false;
  if (screenMode == MODE_SIDEBAR) {
    if (sidebarIndex < (int)history.size() - 1) sidebarIndex++;
    return;
  }

  if (screenMode != MODE_VIEW && screenMode != MODE_THINKING) return;
  ChatItem* item = currentChatItem();
  if (!item || item->thinking) return;

  if (!item->blocks.empty()) {
    if (item->blockIndex < (int)item->blocks.size() - 1) {
      item->blockIndex++;
      item->blocks[item->blockIndex].xOffset = 0;
    }
    return;
  }

  int maxOffset = max(0, (int)item->fallbackLines.size() - 2);
  if (item->fallbackLineOffset < maxOffset) item->fallbackLineOffset += 2;
  if (item->fallbackLineOffset > maxOffset) item->fallbackLineOffset = maxOffset;
}

void actionLeft() {
  answerBlankMode = false;
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
      item->blockIndex--;
      item->blocks[item->blockIndex].xOffset = maxXForBlock(item->blocks[item->blockIndex]);
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
      item->blockIndex++;
      item->blocks[item->blockIndex].xOffset = 0;
    }
    return;
  }

  if (currentHistoryIndex < (int)history.size() - 1) {
    currentHistoryIndex++;
    sidebarIndex = currentHistoryIndex;
  }
}

void actionCamera() {
  if (screenMode == MODE_ERROR || solveRunning) return;
  answerBlankMode = false;
  if (screenMode != MODE_CAPTURE) {
    screenMode = MODE_CAPTURE;
    return;
  }
  captureOnePhotoToPsr();
}

void actionEnter() {
  if (screenMode == MODE_SIDEBAR) {
    answerBlankMode = false;
    if (history.empty()) {
      screenMode = MODE_READY;
      return;
    }
    currentHistoryIndex = sidebarIndex;
    screenMode = MODE_VIEW;
    return;
  }

  if (screenMode == MODE_VIEW) {
    ChatItem* item = currentChatItem();
    if (item && !item->thinking) {
      answerBlankMode = !answerBlankMode;
      return;
    }
  }

  if (!pendingPhotos.empty()) {
    answerBlankMode = false;
    startSolveFromPending();
  }
}

void actionPage() {
  if (screenMode == MODE_ERROR || solveRunning) return;
  answerBlankMode = false;

  if (screenMode == MODE_SIDEBAR) {
    if (!history.empty()) {
      if (currentHistoryIndex < 0 || currentHistoryIndex >= (int)history.size()) {
        currentHistoryIndex = history.size() - 1;
      }
      screenMode = MODE_VIEW;
    } else {
      screenMode = pendingPhotos.empty() ? MODE_READY : MODE_CAPTURE;
    }
    return;
  }

  if (history.empty()) {
    screenMode = pendingPhotos.empty() ? MODE_READY : MODE_CAPTURE;
    return;
  }

  if (currentHistoryIndex < 0 || currentHistoryIndex >= (int)history.size()) {
    currentHistoryIndex = history.size() - 1;
  }
  sidebarIndex = currentHistoryIndex;
  screenMode = MODE_SIDEBAR;
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
  renderScreen();
  delay(900);
  quietCameraPins();

  if (!connectWiFi()) {
    showError("WiFi failed");
    renderScreen();
    return;
  }

  screenMode = MODE_SIDEBAR;
  renderScreen();
}

void loop() {
  if (btnUp.fell()) {
    Serial.println("[BTN] UP");
    actionUp();
  }
  if (btnDown.fell()) {
    Serial.println("[BTN] DOWN");
    actionDown();
  }
  if (btnLeft.fell()) {
    Serial.println("[BTN] PREV");
    actionLeft();
  }
  if (btnRight.fell()) {
    Serial.println("[BTN] NEXT");
    actionRight();
  }
  if (btnCamera.fell()) {
    Serial.println("[BTN] CAMERA");
    actionCamera();
  }
  if (btnEnter.fell()) {
    Serial.println("[BTN] ENTER");
    actionEnter();
  }
  if (btnPage.fell()) {
    Serial.println("[BTN] PAGE");
    actionPage();
  }

  if (screenMode == MODE_READY && !pendingPhotos.empty()) {
    screenMode = MODE_CAPTURE;
  }
  renderScreen();
  delay(20);
}
