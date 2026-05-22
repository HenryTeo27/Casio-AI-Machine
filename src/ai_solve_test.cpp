#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <vector>
#include <Wire.h>
#include <U8g2lib.h>
#include "mbedtls/base64.h"
#include "casio_config.h"

// OLED pins (same as main system)
#define OLED_SDA 1
#define OLED_SCL 2
#define OLED_RST 47
#define BTN_SCROLL_UP 14
#define BTN_SCROLL_DOWN 21
#define BTN_SCROLL_LEFT 40
#define BTN_SCROLL_RIGHT 41

static constexpr uint32_t WIFI_TIMEOUT_MS = 20000;
static constexpr uint32_t HTTP_TIMEOUT_MS = 60000;
static constexpr uint32_t SOLVE_TIMEOUT_MS = 300000;
static constexpr uint16_t HTTPCLIENT_SOLVE_TIMEOUT_MS = 60000;
static constexpr int UPLOAD_RETRY_COUNT = 3;
static constexpr int DOWNLOAD_RETRY_COUNT = 5;
static constexpr uint32_t DOWNLOAD_RETRY_DELAY_MS = 900;
static constexpr uint32_t DOWNLOAD_OVERALL_TIMEOUT_MS = 90000;
static constexpr uint32_t BUTTON_DEBOUNCE_MS = 35;
static constexpr int H_SCROLL_STEP = 32;
static constexpr uint32_t OLED_CLEAR_SETTLE_MS = 8;
static constexpr uint8_t OLED_CONTRAST = 0;
static constexpr uint32_t RESULT_FETCH_TOTAL_WAIT_MS = 720000;
static constexpr uint32_t RESULT_FETCH_RETRY_DELAY_MS = 3000;

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
int gYOffset = 0;

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
  if (!displayBlocksVar.is<JsonArray>()) {
    Serial.println("[OLED] display_blocks missing/not array");
    return false;
  }

  JsonArray arr = displayBlocksVar.as<JsonArray>();
  int pageNo = 0;
  for (JsonVariant item : arr) {
    pageNo++;
    String type = String((const char*)(item["type"] | ""));
    String format = String((const char*)(item["format"] | ""));
    int width = item["width"] | 0;
    int height = item["height"] | 0;
    String b64 = String((const char*)(item["data"] | ""));

    if (type != "bitmap") {
      Serial.printf("[OLED] skip block %d: unsupported type=%s\n", pageNo, type.c_str());
      continue;
    }
    if (format != "1bit_xbm") {
      Serial.printf("[OLED] skip block %d: unsupported format=%s\n", pageNo, format.c_str());
      continue;
    }
    if (width <= 0 || (height != 16 && height != 32 && height != 64)) {
      Serial.printf("[OLED] skip block %d: bad size w=%d h=%d\n", pageNo, width, height);
      continue;
    }
    if (b64.length() == 0) {
      Serial.printf("[OLED] skip block %d: empty data\n", pageNo);
      continue;
    }

    std::vector<uint8_t> decoded;
    if (!decodeBase64ToBytes(b64, decoded)) {
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
    block.width = width;
    block.height = height;
    block.data = std::move(decoded);
    outBlocks.push_back(std::move(block));
  }

  Serial.printf("[OLED] display_blocks parsed=%u/%u\n",
                (unsigned)outBlocks.size(), (unsigned)arr.size());
  return !outBlocks.empty();
}

bool parseDisplayBlockItem(JsonVariant item, int pageNo, DisplayBlock& outBlock) {
  String type = String((const char*)(item["type"] | ""));
  String format = String((const char*)(item["format"] | ""));
  int width = item["width"] | 0;
  int height = item["height"] | 0;
  String b64 = String((const char*)(item["data"] | ""));

  if (type != "bitmap") {
    Serial.printf("[OLED] skip block %d: unsupported type=%s\n", pageNo, type.c_str());
    return false;
  }
  if (format != "1bit_xbm") {
    Serial.printf("[OLED] skip block %d: unsupported format=%s\n", pageNo, format.c_str());
    return false;
  }
  if (width <= 0 || (height != 16 && height != 32 && height != 64)) {
    Serial.printf("[OLED] skip block %d: bad size w=%d h=%d\n", pageNo, width, height);
    return false;
  }
  if (b64.length() == 0) {
    Serial.printf("[OLED] skip block %d: empty data\n", pageNo);
    return false;
  }

  std::vector<uint8_t> decoded;
  if (!decodeBase64ToBytes(b64, decoded)) {
    Serial.printf("[OLED] skip block %d: base64 decode failed\n", pageNo);
    return false;
  }

  int expected = ((width + 7) / 8) * height;
  if ((int)decoded.size() != expected) {
    Serial.printf("[OLED] skip block %d: bytes=%u expected=%d\n",
                  pageNo, (unsigned)decoded.size(), expected);
    return false;
  }

  outBlock.width = width;
  outBlock.height = height;
  outBlock.data = std::move(decoded);
  return true;
}

int maxXFor(const DisplayBlock& b) {
  return max(0, b.width - 128);
}

int maxYFor(const DisplayBlock& b) {
  return max(0, b.height - 32);
}

void clampViewer() {
  if (gBlocks.empty()) return;
  if (gBlockIndex < 0) gBlockIndex = 0;
  if (gBlockIndex >= (int)gBlocks.size()) gBlockIndex = gBlocks.size() - 1;
  int maxX = maxXFor(gBlocks[gBlockIndex]);
  if (gXOffset < 0) gXOffset = 0;
  if (gXOffset > maxX) gXOffset = maxX;
  int maxY = maxYFor(gBlocks[gBlockIndex]);
  if (gYOffset < 0) gYOffset = 0;
  if (gYOffset > maxY) gYOffset = maxY;
}

void renderCurrentBlock() {
  if (gBlocks.empty()) return;
  clampViewer();
  const DisplayBlock& b = gBlocks[gBlockIndex];
  clearOledFrame();
  u8g2.clearBuffer();
  u8g2.setDrawColor(1);
  u8g2.drawXBMP(-gXOffset, -gYOffset, b.width, b.height, b.data.data());
  u8g2.sendBuffer();
  Serial.printf("[OLED] block %d/%u x=%d/%d y=%d/%d\n",
                gBlockIndex + 1, (unsigned)gBlocks.size(), gXOffset, maxXFor(b), gYOffset, maxYFor(b));
}

void moveViewerUp() {
  if (gBlocks.empty()) return;
  if (gYOffset > 0) {
    gYOffset -= 32;
    if (gYOffset < 0) gYOffset = 0;
    return;
  }
  if (gBlockIndex > 0) {
    int previousX = gXOffset;
    int previousWidth = gBlocks[gBlockIndex].width;
    gBlockIndex--;
    gXOffset = (gBlocks[gBlockIndex].width == previousWidth)
      ? min(previousX, maxXFor(gBlocks[gBlockIndex]))
      : 0;
    gYOffset = maxYFor(gBlocks[gBlockIndex]);
  }
}

void moveViewerDown() {
  if (gBlocks.empty()) return;
  int maxY = maxYFor(gBlocks[gBlockIndex]);
  if (gYOffset < maxY) {
    gYOffset += 32;
    if (gYOffset > maxY) gYOffset = maxY;
    return;
  }
  if (gBlockIndex < (int)gBlocks.size() - 1) {
    int previousX = gXOffset;
    int previousWidth = gBlocks[gBlockIndex].width;
    gBlockIndex++;
    gXOffset = (gBlocks[gBlockIndex].width == previousWidth)
      ? min(previousX, maxXFor(gBlocks[gBlockIndex]))
      : 0;
    gYOffset = 0;
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
    int previousWidth = gBlocks[gBlockIndex].width;
    gBlockIndex--;
    gXOffset = (gBlocks[gBlockIndex].width == previousWidth)
      ? 0
      : maxXFor(gBlocks[gBlockIndex]);
    gYOffset = maxYFor(gBlocks[gBlockIndex]);
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
    int previousX = gXOffset;
    int previousWidth = gBlocks[gBlockIndex].width;
    gBlockIndex++;
    gXOffset = (gBlocks[gBlockIndex].width == previousWidth)
      ? min(previousX, maxXFor(gBlocks[gBlockIndex]))
      : 0;
    gYOffset = 0;
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

  outStatusCode = 200;
  outBody = rawResp;
  Serial.printf("[%s] headerless body len=%u\n", tag, (unsigned)outBody.length());
  return true;
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
  return parseRawHttpResponse(rawResp, "UP", outStatusCode, outBody);
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
    if ((millis() - startedAt) > SOLVE_TIMEOUT_MS) {
      Serial.printf("[SOLVE] raw wait timeout raw_len=%u\n", (unsigned)rawResp.length());
      client.stop();
      return false;
    }
    if ((millis() - lastDataAt) > SOLVE_TIMEOUT_MS) {
      Serial.printf("[SOLVE] raw idle timeout raw_len=%u\n", (unsigned)rawResp.length());
      client.stop();
      return false;
    }
    delay(20);
  }
  client.stop();

  bool parsed = parseRawHttpResponse(rawResp, "SOLVE", outCode, outBody);
  Serial.printf("[SOLVE] raw code=%d body_len=%u raw_len=%u\n",
                outCode, (unsigned)outBody.length(), (unsigned)rawResp.length());
  return parsed && outCode > 0;
}

bool fetchQuestionResultOnce(int questionId, int& outCode, String& outBody) {
  outCode = -1;
  outBody = "";

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(HTTP_TIMEOUT_MS / 1000);

  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);
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

bool fetchQuestionResultWithRetry(int questionId, String& outBody, int& outCode) {
  unsigned long startedAt = millis();
  int attempt = 1;
  while ((millis() - startedAt) <= RESULT_FETCH_TOTAL_WAIT_MS) {
    int code = -1;
    String body;
    unsigned long t0 = millis();
    bool ok = fetchQuestionResultOnce(questionId, code, body);
    Serial.printf("[TIME] result_fetch_%d=%lums ok=%d code=%d\n",
                  attempt, (unsigned long)(millis() - t0), ok ? 1 : 0, code);
    if (ok) {
      JsonDocument statusDoc;
      DeserializationError err = deserializeJson(statusDoc, body);
      if (!err && (statusDoc["ok"] | false)) {
        outBody = body;
        outCode = code;
        return true;
      }

      String status = String((const char*)(statusDoc["status"] | ""));
      String error = String((const char*)(statusDoc["error"] | ""));
      Serial.printf("[RESULT] not ready attempt=%d status=%s error=%s\n",
                    attempt, status.c_str(), error.c_str());

      if (status == "FAILED" || code >= 500) {
        outBody = body;
        outCode = code;
        return true;
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

bool fetchQuestionBlockOnce(int questionId, int blockIndex, int& outCode, String& outBody) {
  outCode = -1;
  outBody = "";

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(HTTP_TIMEOUT_MS / 1000);

  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);
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
    int code = -1;
    String body;
    bool ok = fetchQuestionBlockOnce(questionId, blockIndex, code, body);
    if (ok && code >= 200 && code < 300) {
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

bool fetchQuestionBlocksWithRetry(int questionId, int blockCount, std::vector<DisplayBlock>& outBlocks) {
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
  }

  Serial.printf("[BLOCK] fetched display_blocks=%u/%d\n",
                (unsigned)outBlocks.size(), blockCount);
  return !outBlocks.empty();
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
  u8g2.setContrast(OLED_CONTRAST);

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
  jpeg.clear();
  jpeg.shrink_to_fit();
  Serial.printf("[MEM] jpeg freed before solve heap=%u psram=%u\n",
                (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getFreePsram());

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

  if (!transportOk || code == 202 || code < 200 || code >= 300) {
    Serial.println("[TEST] solve pending/failed, trying result fetch fallback");
    bool fetched = fetchQuestionResultWithRetry(questionId, body, code);
    if (!fetched) {
      Serial.println("[TEST] result fetch failed");
      drawTwoLines("Solve failed", "no result");
      return;
    }
    Serial.println("[TEST] result fetch ok body:");
    Serial.println(body);
  }
  Serial.println("[STEP] solve transport ok");

  JsonDocument resp;
  DeserializationError err = deserializeJson(resp, body);
  if (err) {
    Serial.printf("[TEST] solve json parse failed: %s\n", err.c_str());
    Serial.println("[TEST] trying result fetch after json parse failure");
    if (!fetchQuestionResultWithRetry(questionId, body, code)) {
      drawTwoLines("Solve failed", "json parse");
      return;
    }
    err = deserializeJson(resp, body);
    if (err) {
      Serial.printf("[TEST] fetched result json parse failed: %s\n", err.c_str());
      drawTwoLines("Solve failed", "json parse");
      return;
    }
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
  int displayBlockCount = resp["display_block_count"] | 0;
  bool displayBlocksInline = resp["display_blocks_inline"] | true;
  if (!hasBlocks && displayBlockCount > 0) {
    Serial.printf("[TEST] inline blocks unavailable inline=%d count=%d; fetching blocks\n",
                  displayBlocksInline ? 1 : 0, displayBlockCount);
    hasBlocks = fetchQuestionBlocksWithRetry(questionId, displayBlockCount, blocks);
  }
  Serial.printf("[TEST] display_blocks parsed=%d count=%u\n", hasBlocks ? 1 : 0, (unsigned)blocks.size());

  if (hasBlocks) {
    drawTwoLines("Answer ready", String((int)blocks.size()) + " pages");
    delay(600);
    gBlocks = std::move(blocks);
    gViewerActive = true;
    gBlockIndex = 0;
    gXOffset = 0;
    gYOffset = 0;
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
