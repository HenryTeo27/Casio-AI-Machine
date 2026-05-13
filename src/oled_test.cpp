#include <Arduino.h>
#include <Wire.h>
#include <U8g2lib.h>

static constexpr int OLED_SDA = 1;
static constexpr int OLED_SCL = 2;
static constexpr int OLED_RST = 47;

U8G2_SSD1305_128X32_NONAME_F_HW_I2C u8g2(U8G2_R0, OLED_RST);

void drawTwoLines(const String& line1, const String& line2) {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(0, 12, line1.c_str());
  u8g2.drawStr(0, 28, line2.c_str());
  u8g2.sendBuffer();
}

void drawPixelPattern() {
  u8g2.clearBuffer();
  u8g2.drawFrame(0, 0, 128, 32);
  for (int x = 0; x < 128; x += 8) {
    u8g2.drawVLine(x, 0, 32);
  }
  for (int y = 0; y < 32; y += 8) {
    u8g2.drawHLine(0, y, 128);
  }
  u8g2.sendBuffer();
}

void drawContrastBars() {
  u8g2.clearBuffer();
  for (int i = 0; i < 8; i++) {
    int x = i * 16;
    u8g2.drawBox(x, 32 - (i + 1) * 4, 14, (i + 1) * 4);
  }
  u8g2.setFont(u8g2_font_5x7_tf);
  u8g2.drawStr(0, 7, "Contrast / fill");
  u8g2.sendBuffer();
}

void setup() {
  Serial.begin(115200);
  delay(600);
  Serial.println();
  Serial.println("=== OLED Test ===");

  Wire.begin(OLED_SDA, OLED_SCL);
  Wire.setClock(400000);

  u8g2.setI2CAddress(0x3C * 2);
  u8g2.begin();
  u8g2.enableUTF8Print();
  u8g2.setContrast(30);

  drawTwoLines("OLED Test", "SSD1305 128x32");
  Serial.println("[OK] OLED initialized");
  delay(1200);
}

void loop() {
  drawTwoLines("Casio AI Machine", "OLED module OK");
  delay(1200);

  drawTwoLines("Line test 123456", "abcdef +-*/= pi");
  delay(1200);

  drawPixelPattern();
  delay(1200);

  drawContrastBars();
  delay(1200);

  for (int contrast : {5, 15, 30, 60, 100}) {
    u8g2.setContrast(contrast);
    drawTwoLines("Contrast", String(contrast));
    Serial.printf("[INFO] Contrast=%d\n", contrast);
    delay(800);
  }

  u8g2.setContrast(30);
}
