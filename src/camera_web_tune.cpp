#include <Arduino.h>
#include <DNSServer.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Wire.h>
#include <U8g2lib.h>
#include "esp_camera.h"

// =====================================================
// USER CONFIG
// =====================================================
const char* WIFI_SSID = "Henry Teo";
const char* WIFI_PASS = "henrycute";
const char* AP_SSID = "CasioCamTune";
const char* AP_PASS = "12345678";

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

U8G2_SSD1305_128X32_NONAME_F_HW_I2C u8g2(U8G2_R0, OLED_RST);
WebServer server(80);
DNSServer dnsServer;

framesize_t currentFrameSize = FRAMESIZE_UXGA;
int currentQuality = 4;
int currentBrightness = 1;
int currentContrast = 2;
int currentSaturation = -1;
int currentSharpness = -2;
int currentDenoise = 2;
int currentAeLevel = 2;
gainceiling_t currentGainCeiling = GAINCEILING_2X;

void drawTwoLines(const String& l1, const String& l2) {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(0, 12, l1.c_str());
  u8g2.drawStr(0, 28, l2.c_str());
  u8g2.sendBuffer();
}

bool connectWiFi() {
  drawTwoLines("WiFi AP mode", AP_SSID);
  WiFi.disconnect(true);
  delay(300);
  WiFi.mode(WIFI_AP);
  WiFi.setSleep(false);

  IPAddress apIp(192, 168, 4, 1);
  IPAddress gateway(192, 168, 4, 1);
  IPAddress subnet(255, 255, 255, 0);
  WiFi.softAPConfig(apIp, gateway, subnet);

  bool apOk = WiFi.softAP(AP_SSID, AP_PASS, 6, false, 4);
  if (!apOk) {
    Serial.println("[ERR] AP start failed");
    drawTwoLines("AP failed", "restart ESP");
    return false;
  }

  Serial.print("[OK] AP open: http://");
  Serial.println(WiFi.softAPIP());
  drawTwoLines("Open browser", WiFi.softAPIP().toString());
  return true;
}

void applyPreset() {
  sensor_t* s = esp_camera_sensor_get();
  if (!s) return;

  s->set_framesize(s, currentFrameSize);
  s->set_quality(s, currentQuality);
  s->set_whitebal(s, 1);
  s->set_awb_gain(s, 1);
  s->set_wb_mode(s, 0);
  s->set_exposure_ctrl(s, 1);
  s->set_aec2(s, 1);
  s->set_ae_level(s, currentAeLevel);
  s->set_gain_ctrl(s, 1);
  s->set_gainceiling(s, currentGainCeiling);
  s->set_brightness(s, currentBrightness);
  s->set_contrast(s, currentContrast);
  s->set_saturation(s, currentSaturation);
  if (s->set_sharpness) s->set_sharpness(s, currentSharpness);
  if (s->set_denoise) s->set_denoise(s, currentDenoise);
  if (s->set_bpc) s->set_bpc(s, 1);
  if (s->set_wpc) s->set_wpc(s, 1);
  if (s->set_lenc) s->set_lenc(s, 1);
  if (s->set_raw_gma) s->set_raw_gma(s, 1);
  s->set_vflip(s, 1);
  s->set_hmirror(s, 1);
}

bool initCamera() {
  bool hasPsram = psramFound();
  if (!hasPsram) currentFrameSize = FRAMESIZE_SVGA;
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
  config.frame_size = currentFrameSize;
  config.jpeg_quality = currentQuality;
  config.fb_count = 1;
  config.fb_location = hasPsram ? CAMERA_FB_IN_PSRAM : CAMERA_FB_IN_DRAM;
  config.grab_mode = CAMERA_GRAB_LATEST;

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("[ERR] Camera init failed: 0x%x\n", err);
    drawTwoLines("Camera failed", "init error");
    return false;
  }

  applyPreset();
  Serial.println("[OK] Camera initialized");
  return true;
}

void sensorStandby() {
  sensor_t* s = esp_camera_sensor_get();
  if (!s || !s->set_reg) return;
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

void shutdownCamera() {
  sensorStandby();
  esp_err_t err = esp_camera_deinit();
  quietCameraPins();
  Serial.printf("[INFO] Camera shutdown complete: 0x%x\n", (unsigned)err);
}

String htmlPage() {
  return R"rawliteral(
<!doctype html>
<html>
<head>
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Casio Camera Tune</title>
<style>
body{margin:0;font-family:ui-sans-serif,system-ui;background:#111;color:#eee}
main{display:grid;grid-template-columns:minmax(0,1fr) 330px;gap:16px;padding:16px}
img{width:100%;height:auto;background:#000}
.panel{display:grid;gap:10px;align-content:start}
label{display:grid;gap:4px;font-size:13px;color:#ddd}
input,select,button{font:inherit}
input[type=range]{width:100%}
button,select{padding:8px;background:#222;color:#eee;border:1px solid #555}
.row{display:grid;grid-template-columns:1fr auto;gap:8px;align-items:center}
@media(max-width:850px){main{grid-template-columns:1fr}.panel{grid-row:1}}
</style>
</head>
<body>
<main>
  <section>
    <img id="view" alt="Click Capture once">
  </section>
  <section class="panel">
    <button onclick="snap()">Capture once</button>
    <label>Resolution
      <select id="framesize" onchange="setv('framesize', this.value)">
        <option value="13">UXGA 1600x1200</option>
        <option value="12">SXGA 1280x1024</option>
        <option value="10">XGA 1024x768</option>
        <option value="9">SVGA 800x600</option>
        <option value="8">VGA 640x480</option>
      </select>
    </label>
    <div id="sliders"></div>
  </section>
</main>
<script>
const defs=[
 ['quality','JPEG quality (lower is better)',4,20,4],
 ['brightness','Brightness',-2,2,1],
 ['contrast','Contrast',-2,2,2],
 ['saturation','Saturation',-2,2,-1],
 ['sharpness','Sharpness',-2,2,-2],
 ['denoise','Denoise',0,2,2],
 ['ae_level','AE level',-2,2,2],
 ['gainceiling','Gain ceiling',0,6,0]
];
const box=document.getElementById('sliders');
defs.forEach(([k,n,min,max,val])=>{
  const el=document.createElement('label');
  el.innerHTML=`<div class="row"><span>${n}</span><b id="${k}v">${val}</b></div><input type="range" min="${min}" max="${max}" value="${val}" oninput="${k}v.textContent=this.value" onchange="setv('${k}',this.value)">`;
  box.appendChild(el);
});
function snap(){document.getElementById('view').src='/capture.jpg?t='+Date.now();}
async function setv(k,v){await fetch(`/control?var=${k}&val=${v}`);}
</script>
</body>
</html>
)rawliteral";
}

void handleRoot() {
  server.send(200, "text/html", htmlPage());
}

void handleNotFound() {
  server.sendHeader("Location", String("http://") + WiFi.softAPIP().toString(), true);
  server.send(302, "text/plain", "");
}

void handleCapture() {
  Serial.println("[INFO] Manual capture requested");
  if (!initCamera()) {
    shutdownCamera();
    server.send(503, "text/plain", "camera init failed");
    return;
  }

  delay(800);
  for (int i = 0; i < 3; i++) {
    camera_fb_t* warmup = esp_camera_fb_get();
    if (warmup) {
      Serial.printf("[INFO] Warmup %d bytes=%u\n", i + 1, (unsigned)warmup->len);
      esp_camera_fb_return(warmup);
    } else {
      Serial.printf("[WARN] Warmup %d failed\n", i + 1);
    }
    delay(160);
  }

  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) {
    shutdownCamera();
    server.send(503, "text/plain", "capture failed");
    return;
  }

  uint8_t* copy = (uint8_t*)ps_malloc(fb->len);
  if (!copy) copy = (uint8_t*)malloc(fb->len);
  if (!copy) {
    esp_camera_fb_return(fb);
    shutdownCamera();
    server.send(500, "text/plain", "no memory");
    return;
  }

  size_t copyLen = fb->len;
  memcpy(copy, fb->buf, fb->len);
  Serial.printf("[OK] Captured bytes=%u, w=%u h=%u\n",
                (unsigned)fb->len, fb->width, fb->height);

  esp_camera_fb_return(fb);
  shutdownCamera();

  server.sendHeader("Cache-Control", "no-store");
  server.setContentLength(copyLen);
  server.send(200, "image/jpeg", "");
  WiFiClient client = server.client();
  client.write(copy, copyLen);
  free(copy);
}

void handleControl() {
  if (!server.hasArg("var") || !server.hasArg("val")) {
    server.send(400, "text/plain", "missing var/val");
    return;
  }

  String var = server.arg("var");
  int val = server.arg("val").toInt();

  int res = 0;
  if (var == "framesize") {
    currentFrameSize = (framesize_t)val;
  } else if (var == "quality") {
    currentQuality = val;
  } else if (var == "brightness") {
    currentBrightness = val;
  } else if (var == "contrast") {
    currentContrast = val;
  } else if (var == "saturation") {
    currentSaturation = val;
  } else if (var == "sharpness") {
    currentSharpness = val;
  } else if (var == "denoise") {
    currentDenoise = val;
  } else if (var == "ae_level") {
    currentAeLevel = val;
  } else if (var == "gainceiling") {
    currentGainCeiling = (gainceiling_t)val;
  } else {
    server.send(404, "text/plain", "unknown var");
    return;
  }

  Serial.printf("[CTRL] saved %s=%d result=%d\n", var.c_str(), val, res);
  server.send(200, "text/plain", "ok");
}

void setup() {
  Serial.begin(115200);
  delay(600);
  Serial.println();
  Serial.println("=== Casio Camera Web Tune ===");

  Wire.begin(OLED_SDA, OLED_SCL);
  Wire.setClock(400000);
  u8g2.setI2CAddress(0x3C * 2);
  u8g2.begin();
  u8g2.setContrast(25);

  if (!connectWiFi()) return;

  dnsServer.start(53, "*", WiFi.softAPIP());
  server.on("/", HTTP_GET, handleRoot);
  server.on("/capture.jpg", HTTP_GET, handleCapture);
  server.on("/control", HTTP_GET, handleControl);
  server.onNotFound(handleNotFound);
  server.begin();

  Serial.print("[OK] Tuning UI AP: http://");
  Serial.println(WiFi.softAPIP());
}

void loop() {
  dnsServer.processNextRequest();
  server.handleClient();
  static unsigned long lastClientLog = 0;
  if (millis() - lastClientLog > 5000) {
    lastClientLog = millis();
    Serial.printf("[INFO] AP clients=%d, open http://%s\n",
                  WiFi.softAPgetStationNum(),
                  WiFi.softAPIP().toString().c_str());
  }
  delay(2);
}
