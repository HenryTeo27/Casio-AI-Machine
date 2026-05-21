#include <Arduino.h>

static constexpr int BTN_PAGE_UP = 14;
static constexpr int BTN_PAGE_DOWN = 21;
static constexpr int BTN_Q_PREV = 40;
static constexpr int BTN_Q_NEXT = 41;
static constexpr int BTN_CAMERA = 38;
static constexpr int BTN_OK = 39;
static constexpr int BTN_EXTRA = 42;

static constexpr unsigned long DEBOUNCE_MS = 35;

struct Button {
  const char* name;
  uint8_t pin;
  bool lastStable = HIGH;
  bool lastRead = HIGH;
  unsigned long lastChange = 0;
  uint32_t count = 0;

  Button(const char* n, uint8_t p) : name(n), pin(p) {}

  void begin() {
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

    if ((millis() - lastChange) >= DEBOUNCE_MS && reading != lastStable) {
      lastStable = reading;
      if (lastStable == LOW) {
        count++;
        return true;
      }
    }

    return false;
  }
};

Button buttons[] = {
  {"PAGE UP", BTN_PAGE_UP},
  {"PAGE DOWN", BTN_PAGE_DOWN},
  {"Q PREV", BTN_Q_PREV},
  {"Q NEXT", BTN_Q_NEXT},
  {"CAMERA", BTN_CAMERA},
  {"OK", BTN_OK},
  {"PAGE SWITCH", BTN_EXTRA},
};

void setup() {
  Serial.begin(115200);
  delay(600);
  Serial.println();
  Serial.println("=== Button Test (Serial Only) ===");

  for (auto& button : buttons) {
    button.begin();
    Serial.printf("[INFO] %s pin=%u initial=%s\n",
                  button.name,
                  button.pin,
                  digitalRead(button.pin) == LOW ? "LOW" : "HIGH");
  }

  Serial.println("[INFO] Press any key...");
}

void loop() {
  for (auto& button : buttons) {
    if (button.fell()) {
      Serial.printf("[BTN] %s pin=%u count=%lu\n",
                    button.name,
                    button.pin,
                    (unsigned long)button.count);
    }
  }

  delay(20);
}
