#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <vector>
#include <Wire.h>
#include <U8g2lib.h>
#include "mbedtls/base64.h"

// OLED pins (same as main system)
#define OLED_SDA 1
#define OLED_SCL 2
#define OLED_RST 47
#define BTN_SCROLL_UP 14
#define BTN_SCROLL_DOWN 21
#define BTN_SCROLL_LEFT 40
#define BTN_SCROLL_RIGHT 41

// =====================================================
// USER CONFIG
// =====================================================
const char* WIFI_SSID = "Henry Teo";
const char* WIFI_PASS = "henrycute";

const char* SERVER_HOST = "accelertechnology.my";
const char* UPLOAD_PATH = "/api/casio-ai/upload-photo";
const char* SOLVE_PATH = "/api/casio-ai/solve";

const char* DEVICE_ID = "CASIO_AI_MACHINE_001";
const char* DEVICE_API_KEY = "21326a10-c7f8-4ca9-8e7c-d6f55c15d564";

const char* FIXED_PHOTO_URL =
  "https://k6b38ex5bcg3evfg.public.blob.vercel-storage.com/casio-ai/device/"
  "CASIO_AI_MACHINE_001/2026-05-20/q1/1-0-6d2e5631-b830-4dd7-a8e2-0c2add4794ec.jpg";

static constexpr uint32_t WIFI_TIMEOUT_MS = 20000;
static constexpr uint32_t HTTP_TIMEOUT_MS = 60000;
static constexpr uint32_t SOLVE_TIMEOUT_MS = 300000;
static constexpr int UPLOAD_RETRY_COUNT = 3;
static constexpr int DOWNLOAD_RETRY_COUNT = 5;
static constexpr uint32_t DOWNLOAD_RETRY_DELAY_MS = 900;
static constexpr uint32_t DOWNLOAD_OVERALL_TIMEOUT_MS = 90000;
static constexpr uint32_t BUTTON_DEBOUNCE_MS = 35;
static constexpr int H_SCROLL_STEP = 32;
static constexpr uint32_t OLED_CLEAR_SETTLE_MS = 8;

U8G2_SSD1305_128X32_NONAME_F_HW_I2C u8g2(U8G2_R0, OLED_RST);

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

struct DisplayBlock {
  int width = 0;
  int height = 0;
  std::vector<uint8_t> data;
};

Button btnUp;
Button btnDown;
Button btnLeft;
Button btnRight;

std::vector<DisplayBlock> gBlocks;
bool gViewerActive = false;
int gBlockIndex = 0;
int gXOffset = 0;

void drawTwoLines(const String& l1, const String& l2) {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(0, 12, l1.c_str());
  u8g2.drawStr(0, 28, l2.c_str());
  u8g2.sendBuffer();
}

void clearOledFrame() {
  u8g2.clearBuffer();
  u8g2.sendBuffer();
  delay(OLED_CLEAR_SETTLE_MS);
}

std::vector<String> wrapTwoLines(String raw) {
  raw.replace("\r", "");
  raw.replace("\n", " ");
  while (raw.indexOf("  ") >= 0) raw.replace("  ", " ");
  std::vector<String> out;
  String cur = "";
  for (size_t i = 0; i < raw.length(); i++) {
    cur += raw[i];
    if (cur.length() >= 21) {
      out.push_back(cur);
      cur = "";
      if (out.size() >= 2) break;
    }
  }
  if (out.size() < 2 && cur.length() > 0) out.push_back(cur);
  while (out.size() < 2) out.push_back("");
  return out;
}

bool decodeBase64ToBytes(const String& encoded, std::vector<uint8_t>& out) {
  size_t required = 0;
  int rc = mbedtls_base64_decode(
    nullptr, 0, &required,
    reinterpret_cast<const unsigned char*>(encoded.c_str()),
    encoded.length()
  );
  if (rc != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL && rc != 0) return false;
  if (required == 0) return false;

  out.assign(required, 0);
  size_t written = 0;
  rc = mbedtls_base64_decode(
    out.data(), out.size(), &written,
    reinterpret_cast<const unsigned char*>(encoded.c_str()),
    encoded.length()
  );
  if (rc != 0) return false;
  if (written != out.size()) out.resize(written);
  return true;
}

void invertBitmapBytes(std::vector<uint8_t>& bytes) {
  for (uint8_t& b : bytes) b = ~b;
}

bool parseDisplayBlocks(JsonVariant displayBlocksVar, std::vector<DisplayBlock>& outBlocks) {
  outBlocks.clear();
  if (!displayBlocksVar.is<JsonArray>()) return false;

  JsonArray arr = displayBlocksVar.as<JsonArray>();
  for (JsonVariant item : arr) {
    String type = String((const char*)(item["type"] | ""));
    String format = String((const char*)(item["format"] | ""));
    int width = item["width"] | 0;
    int height = item["height"] | 0;
    String b64 = String((const char*)(item["data"] | ""));

    if (type != "bitmap" || format != "1bit_xbm") continue;
    if (width <= 0 || height <= 0 || b64.length() == 0) continue;

    std::vector<uint8_t> decoded;
    if (!decodeBase64ToBytes(b64, decoded)) continue;

    int expected = ((width + 7) / 8) * height;
    if ((int)decoded.size() != expected) continue;

    DisplayBlock block;
    block.width = width;
    block.height = height;
    block.data = std::move(decoded);
    outBlocks.push_back(std::move(block));
  }

  return !outBlocks.empty();
}

int maxXFor(const DisplayBlock& b) {
  return max(0, b.width - 128);
}

void clampViewer() {
  if (gBlocks.empty()) return;
  if (gBlockIndex < 0) gBlockIndex = 0;
  if (gBlockIndex >= (int)gBlocks.size()) gBlockIndex = gBlocks.size() - 1;
  int maxX = maxXFor(gBlocks[gBlockIndex]);
  if (gXOffset < 0) gXOffset = 0;
  if (gXOffset > maxX) gXOffset = maxX;
}

void renderCurrentBlock() {
  if (gBlocks.empty()) return;
  clampViewer();
  const DisplayBlock& b = gBlocks[gBlockIndex];
  clearOledFrame();
  u8g2.clearBuffer();
  u8g2.setDrawColor(1);
  u8g2.drawXBMP(-gXOffset, 0, b.width, b.height, b.data.data());
  u8g2.sendBuffer();
  Serial.printf("[OLED] block %d/%u x=%d maxX=%d\n",
                gBlockIndex + 1, (unsigned)gBlocks.size(), gXOffset, maxXFor(b));
}

void moveViewerUp() {
  if (gBlocks.empty()) return;
  if (gBlockIndex > 0) {
    gBlockIndex--;
    gXOffset = 0;
  }
}

void moveViewerDown() {
  if (gBlocks.empty()) return;
  if (gBlockIndex < (int)gBlocks.size() - 1) {
    gBlockIndex++;
    gXOffset = 0;
  }
}

void moveViewerLeft() {
  if (gBlocks.empty()) return;
  if (gXOffset > 0) {
    gXOffset -= H_SCROLL_STEP;
    if (gXOffset < 0) gXOffset = 0;
    return;
  }
  if (gBlockIndex > 0) {
    gBlockIndex--;
    gXOffset = maxXFor(gBlocks[gBlockIndex]);
  }
}

void moveViewerRight() {
  if (gBlocks.empty()) return;
  int maxX = maxXFor(gBlocks[gBlockIndex]);
  if (gXOffset < maxX) {
    gXOffset += H_SCROLL_STEP;
    if (gXOffset > maxX) gXOffset = maxX;
    return;
  }
  if (gBlockIndex < (int)gBlocks.size() - 1) {
    gBlockIndex++;
    gXOffset = 0;
  }
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

bool connectWiFi() {
  drawTwoLines("Casio AI Test", "Connecting WiFi");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  unsigned long start = millis();
  Serial.printf("[WIFI] connecting to %s\n", WIFI_SSID);
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < WIFI_TIMEOUT_MS) {
    delay(250);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.printf("[WIFI] failed status=%d\n", (int)WiFi.status());
    drawTwoLines("WiFi failed", "check network");
    return false;
  }

  Serial.printf("[WIFI] connected ip=%s\n", WiFi.localIP().toString().c_str());
  drawTwoLines("WiFi connected", "Starting test");
  return true;
}

bool downloadJpegFromUrl(const String& url, std::vector<uint8_t>& out) {
  out.clear();
  Serial.printf("[DL] begin %s\n", url.c_str());
  drawTwoLines("Downloading", "fixed photo");

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(HTTP_TIMEOUT_MS / 1000);

  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setRedirectLimit(5);
  http.setTimeout(HTTP_TIMEOUT_MS);
  if (!http.begin(client, url)) return false;

  uint32_t t0 = millis();
  Serial.println("[DL] HTTP GET start");
  int code = http.GET();
  Serial.printf("[DL] HTTP code=%d\n", code);
  if (code < 200 || code >= 300) {
    Serial.printf("[DL] GET failed code=%d\n", code);
    drawTwoLines("Download failed", "HTTP " + String(code));
    http.end();
    return false;
  }

  int len = http.getSize();
  Serial.printf("[DL] content-length=%d\n", len);
  WiFiClient* stream = http.getStreamPtr();
  if (!stream) {
    http.end();
    return false;
  }

  if (len > 0) out.reserve((size_t)len);
  uint8_t buf[1024];
  unsigned long lastDataAt = millis();
  unsigned long lastProgressAt = 0;
  while (http.connected()) {
    if ((millis() - t0) > DOWNLOAD_OVERALL_TIMEOUT_MS) {
      Serial.println("[DL] overall timeout");
      break;
    }
    size_t avail = stream->available();
    if (avail > 0) {
      size_t readN = stream->readBytes(buf, min(avail, sizeof(buf)));
      if (readN > 0) {
        out.insert(out.end(), buf, buf + readN);
        lastDataAt = millis();
        if ((millis() - lastProgressAt) > 1000) {
          Serial.printf("[DL] progress bytes=%u\n", (unsigned)out.size());
          lastProgressAt = millis();
        }
        if (len > 0 && (int)out.size() >= len) break;
      }
    } else {
      if (!stream->connected()) break;
      if ((millis() - lastDataAt) > HTTP_TIMEOUT_MS) break;
      delay(2);
    }
  }
  http.end();

  Serial.printf("[TIME] download=%lums bytes=%u\n", (unsigned long)(millis() - t0), (unsigned)out.size());
  return !out.empty();
}

bool downloadJpegWithRetry(const String& url, std::vector<uint8_t>& out) {
  for (int attempt = 1; attempt <= DOWNLOAD_RETRY_COUNT; attempt++) {
    Serial.printf("[DL] attempt %d/%d\n", attempt, DOWNLOAD_RETRY_COUNT);
    if (downloadJpegFromUrl(url, out)) return true;

    WiFi.disconnect(false, false);
    delay(120);
    WiFi.reconnect();
    delay(DOWNLOAD_RETRY_DELAY_MS);
  }
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
  client.setTimeout(HTTP_TIMEOUT_MS / 1000);

  if (!client.connect(SERVER_HOST, 443)) return false;

  String path = String(UPLOAD_PATH) +
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
  unsigned long writeTick = millis();
  while (sent < jpegLen) {
    size_t want = min(chunk, jpegLen - sent);
    size_t done = 0;
    while (done < want) {
      size_t wrote = client.write(jpegData + sent + done, want - done);
      if (wrote == 0) {
        if ((millis() - writeTick) > HTTP_TIMEOUT_MS) {
          client.stop();
          return false;
        }
        delay(2);
        continue;
      }
      done += wrote;
      writeTick = millis();
    }
    sent += done;
    delay(1);
  }

  String rawResp;
  unsigned long lastDataAt = millis();
  while (true) {
    while (client.available()) {
      rawResp += (char)client.read();
      lastDataAt = millis();
    }
    if (!client.connected() && !client.available()) break;
    if ((millis() - lastDataAt) > HTTP_TIMEOUT_MS) {
      client.stop();
      return false;
    }
    delay(20);
  }
  client.stop();
  if (rawResp.length() == 0) return false;

  int headerEnd = rawResp.indexOf("\r\n\r\n");
  if (headerEnd < 0) return false;
  String statusLine = rawResp.substring(0, rawResp.indexOf('\n'));
  statusLine.trim();
  outStatusCode = parseHttpStatusCode(statusLine);
  outBody = rawResp.substring(headerEnd + 4);
  return outStatusCode > 0;
}

bool uploadPhotoToServer(
  const uint8_t* jpegData,
  size_t jpegLen,
  int questionId,
  int photoIndex,
  String& outPhotoId
) {
  for (int attempt = 1; attempt <= UPLOAD_RETRY_COUNT; attempt++) {
    int statusCode = -1;
    String responseBody;
    unsigned long t0 = millis();
    bool posted = postUploadBinaryOnce(jpegData, jpegLen, questionId, photoIndex, statusCode, responseBody);
    unsigned long dt = millis() - t0;
    Serial.printf("[TIME] upload_attempt_%d=%lums posted=%d code=%d\n", attempt, dt, posted ? 1 : 0, statusCode);

    if (posted && statusCode >= 200 && statusCode < 300) {
      JsonDocument doc;
      if (!deserializeJson(doc, responseBody) && (doc["ok"] | false)) {
        const char* photoId = doc["photo_id"] | "";
        if (photoId && strlen(photoId) > 0) {
          outPhotoId = String(photoId);
          return true;
        }
      }
      Serial.println("[UP] success code but invalid json body");
      Serial.println(responseBody);
    } else {
      Serial.printf("[UP] failed attempt=%d\n", attempt);
      if (responseBody.length() > 0) Serial.println(responseBody);
    }

    WiFi.disconnect(false, false);
    delay(120);
    WiFi.reconnect();
    delay(700);
  }
  return false;
}

bool postSolve(const String& payload, int& outCode, String& outBody) {
  outCode = -1;
  outBody = "";

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(SOLVE_TIMEOUT_MS / 1000);

  if (!client.connect(SERVER_HOST, 443)) {
    Serial.println("[SOLVE] TLS connect failed");
    return false;
  }

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
    if ((millis() - lastDataAt) > SOLVE_TIMEOUT_MS) {
      client.stop();
      return false;
    }
    delay(20);
  }
  client.stop();
  if (rawResp.length() == 0) return false;

  int headerEnd = rawResp.indexOf("\r\n\r\n");
  if (headerEnd < 0) return false;

  String statusLine = rawResp.substring(0, rawResp.indexOf('\n'));
  statusLine.trim();
  outCode = parseHttpStatusCode(statusLine);
  outBody = rawResp.substring(headerEnd + 4);
  return outCode > 0;
}

void setup() {
  Serial.begin(115200);
  delay(800);
  Serial.println();
  Serial.println("=== Casio AI Solve URL Test (Upload+Solve) ===");

  Wire.begin(OLED_SDA, OLED_SCL);
  // Keep OLED I2C conservative; long jumper wires can corrupt fast refreshes.
  Wire.setClock(100000);
  u8g2.setI2CAddress(0x3C * 2);
  u8g2.begin();
  u8g2.setContrast(12);

  btnUp.begin(BTN_SCROLL_UP);
  btnDown.begin(BTN_SCROLL_DOWN);
  btnLeft.begin(BTN_SCROLL_LEFT);
  btnRight.begin(BTN_SCROLL_RIGHT);

  drawTwoLines("Casio AI Test", "Booting...");

  if (!connectWiFi()) return;
  Serial.println("[STEP] wifi ok");

  std::vector<uint8_t> jpeg;
  if (!downloadJpegWithRetry(String(FIXED_PHOTO_URL), jpeg)) {
    Serial.println("[TEST] download failed");
    drawTwoLines("Download failed", "check URL/WiFi");
    return;
  }
  Serial.println("[STEP] download ok");
  drawTwoLines("Download OK", "Uploading...");

  const int questionId = 9001;
  String photoId;
  if (!uploadPhotoToServer(jpeg.data(), jpeg.size(), questionId, 0, photoId)) {
    Serial.println("[TEST] upload failed");
    drawTwoLines("Upload failed", "check server");
    return;
  }
  Serial.println("[STEP] upload ok");
  drawTwoLines("Upload OK", "Solving...");
  Serial.printf("[TEST] upload ok photo_id=%s\n", photoId.c_str());

  JsonDocument req;
  req["device_id"] = DEVICE_ID;
  req["question_id"] = questionId;
  req["photo_count"] = 1;
  req["mode"] = "oled-v2";
  req["context_tail"] = "";

  JsonArray ids = req["photo_ids"].to<JsonArray>();
  ids.add(photoId);

  String payload;
  serializeJson(req, payload);
  Serial.println("[TEST] solve payload:");
  Serial.println(payload);

  int code = -1;
  String body;
  unsigned long t0 = millis();
  bool transportOk = postSolve(payload, code, body);
  unsigned long dt = millis() - t0;
  Serial.printf("[TIME] solve=%lums transport_ok=%d code=%d\n", dt, transportOk ? 1 : 0, code);
  Serial.println("[TEST] solve raw body:");
  Serial.println(body);

  if (!transportOk || code < 200 || code >= 300) {
    Serial.println("[TEST] solve failed");
    drawTwoLines("Solve failed", "transport/http");
    return;
  }
  Serial.println("[STEP] solve transport ok");

  JsonDocument resp;
  DeserializationError err = deserializeJson(resp, body);
  if (err) {
    Serial.printf("[TEST] solve json parse failed: %s\n", err.c_str());
    drawTwoLines("Solve failed", "json parse");
    return;
  }

  bool apiOk = resp["ok"] | false;
  Serial.printf("[TEST] solve api ok=%d\n", apiOk ? 1 : 0);
  if (!apiOk) {
    Serial.printf("[TEST] solve error=%s\n", String((const char*)(resp["error"] | "unknown")).c_str());
    drawTwoLines("Solve error", String((const char*)(resp["error"] | "unknown")).substring(0, 21));
    return;
  }

  String answer = String((const char*)(resp["answer"] | ""));
  if (answer.length() == 0) answer = String((const char*)(resp["display_text"] | ""));

  Serial.println("=== AI FULL ANSWER BEGIN ===");
  Serial.println(answer);
  Serial.println("=== AI FULL ANSWER END ===");

  std::vector<DisplayBlock> blocks;
  bool hasBlocks = parseDisplayBlocks(resp["display_blocks"], blocks);
  Serial.printf("[TEST] display_blocks parsed=%d count=%u\n", hasBlocks ? 1 : 0, (unsigned)blocks.size());

  if (hasBlocks) {
    drawTwoLines("Answer ready", String((int)blocks.size()) + " pages");
    delay(600);
    gBlocks = std::move(blocks);
    gViewerActive = true;
    gBlockIndex = 0;
    gXOffset = 0;
    renderCurrentBlock();
  } else {
    std::vector<String> lines = wrapTwoLines(answer);
    drawTwoLines(lines[0], lines[1]);
  }
}

void loop() {
  if (!gViewerActive) {
    delay(1000);
    return;
  }

  bool changed = false;
  if (btnUp.fell()) {
    Serial.println("[BTN] UP");
    moveViewerUp();
    changed = true;
  }
  if (btnDown.fell()) {
    Serial.println("[BTN] DOWN");
    moveViewerDown();
    changed = true;
  }
  if (btnLeft.fell()) {
    Serial.println("[BTN] LEFT");
    moveViewerLeft();
    changed = true;
  }
  if (btnRight.fell()) {
    Serial.println("[BTN] RIGHT");
    moveViewerRight();
    changed = true;
  }
  if (changed) renderCurrentBlock();
  delay(20);
}

