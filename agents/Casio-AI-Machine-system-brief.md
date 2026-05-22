# Casio AI Machine — Product & System Brief

## 1. Project Goal

Casio AI Machine is a small handheld AI study device inspired by old Casio scientific calculators.

Core idea:

```text
Press camera button
→ take one or more photos of a question
→ upload to server
→ server calls AI model
→ AI returns concise explanation
→ device displays answer on a tiny 128×32 OLED screen
```

The device is designed for:

- quick photo-based study assistance
- reviewing previous AI responses during one session
- low-profile calculator-like interaction
- simple button-based navigation
- server-mediated AI calls

It is **not** designed to run AI locally on the ESP32. The ESP32 handles input/output only.

---

## 2. Current Hardware Direction

Main hardware:

```text
ESP32-S3 N16R8 CAM development board
OV5640 DVP AF camera module
2.23 inch SSD1305/SSD1309 128×32 OLED
6 tactile buttons
1 self-locking power button
3.7V LiPo battery pack
5V charge/discharge boost module
```

The full wiring is documented separately in:

```text
electricity-circuit-graph.md
```

This file does not repeat the full wiring diagram.

---

## 3. Main User Flow

### Boot

When the self-locking power button is turned on:

```text
Casio AI Machine
Starting...
```

Then:

```text
Ready: CAM photo
Down: clear ctx
```

The user can either:

- press camera button to take a photo
- press down, then OK, to clear context

---

## 4. Buttons and UX

Current buttons:

| Button | Purpose |
|---|---|
| Previous Page | scroll up within current AI response |
| Next Page | scroll down within current AI response |
| Previous Question | jump to previous question/chat item |
| Next Question | jump to next question/chat item |
| Camera | take photo |
| OK | confirm/send/clear |
| Optional extra button | future page switch: model page, token page, settings page, etc. |

### Navigation Rules

#### Page Up / Page Down

Used to scroll through long AI replies on the small 128×32 screen.

The screen only shows roughly two text lines at once, so long answers are wrapped into line chunks.

#### Previous Question / Next Question

Used to switch between stored questions in the current session.

Rules:

```text
If already on first question and user presses previous:
→ stay on first question, reset to first line.

If already on last question and user presses next:
→ stay on last question, reset to first line.
```

#### Camera Button

When pressed:

```text
Device captures photo
→ moves to capture screen
→ stores photo in pending photo buffer
```

If the user presses camera repeatedly while capture is still busy:

```text
Only one capture should happen.
Extra presses are ignored.
```

#### OK Button

In capture mode:

```text
OK → send pending photos to server
```

In clear context mode:

```text
OK → clear local session context
```

---

## 5. Photo Capture Flow

The device supports multiple photos per question.

Example:

```text
Press CAM → photo 1
Press CAM → photo 2
Press OK  → send both photos as one question
```

Current code limit:

```text
MAX_PENDING_PHOTOS = 3
```

This can be changed later.

### Capture Screen

After taking photos, OLED displays:

```text
Photos: #
CAM=more OK=send
```

or:

```text
Photos: ###
OK=send max
```

The current UI uses `#` as photo blocks.

---

## 6. AI Response Flow

After pressing OK:

```text
Device creates new question item
→ UI shows Thinking...
→ uploads photos to server
→ server calls AI
→ server returns answer
→ ESP stores answer locally
→ display jumps to first line of latest answer
```

While waiting for AI:

- current item shows `Thinking...`
- user may still scroll previous stored answers if supported by future UI refinements
- once answer arrives, the latest question becomes active

---

## 7. Local Session Storage

The device stores chat history in RAM during current power session.

Current behavior:

```text
Power off → session is lost
Power on → starts fresh
```

This is intentional for now.

Reason:

- simpler logic
- avoids Flash wear
- reduces privacy concern
- enough for study session usage

The code currently stores:

```text
question id
photo count
thinking state
answer text
wrapped display lines
```

Current limits:

```text
MAX_HISTORY_ITEMS = 20
MAX_TOTAL_TEXT_CHARS = 120000
MAX_ANSWER_CHARS = 12000
```

If memory becomes too full, the system trims older history.

---

## 8. Text Display Strategy

The OLED is extremely small:

```text
128×32 pixels
approximately 2 lines of readable text
```

Therefore, AI output must be simplified.

### Current ESP-side Cleanup

The ESP code performs basic cleanup:

- removes common Markdown syntax
- removes code fences
- simplifies common LaTeX symbols
- converts some LaTeX commands into ASCII-like text
- wraps text into short lines

Examples:

```text
\times → *
\cdot  → *
\div   → /
\sqrt  → sqrt
\leq   → <=
\geq   → >=
\pi    → pi
```

### Recommended Server-side Formatting

The server should return a `display_text` field that is already optimized for the OLED.

Good display format:

```text
Ans: x = 2
Use formula F=ma.
Step1: ...
Step2: ...
```

Avoid returning raw Markdown or complex LaTeX.

Bad display format:

```text
$$\int_0^\infty e^{-x^2}\,dx = \frac{\sqrt{\pi}}{2}$$
```

Better display format:

```text
Integral 0..inf e^(-x^2) dx
= sqrt(pi)/2
```

The AI answer can be long on the server, but the ESP should prefer the compact `display_text`.

---

## 9. Server Role

The server is the bridge between ESP32 and AI providers.

The ESP32 should **not** call OpenAI/Gemini/Kimi directly.

Reasons:

- API keys must not be stored on the ESP32
- easier to change AI provider
- easier to log usage
- easier to cache images/results
- easier to debug
- easier to add credit/quota logic later

Current intended server base:

```text
https://your-server.example.com
```

---

## 10. Device Authentication

Current prototype uses a device API key stored in firmware:

```text
DEVICE_ID
DEVICE_API_KEY
```

Current prototype device API key:

```text
<your-device-api-key>
```

This is acceptable for private prototype testing, but not secure for public devices.

If this product becomes public, upgrade to:

```text
device_id
device_secret
timestamp
request signature / HMAC
server-side quota
device revoke list
```

Never put actual OpenAI/Gemini/OpenRouter API keys inside ESP firmware.

---

## 11. Server API Contract

### 11.1 Upload Photo

Endpoint:

```http
POST /api/casio-ai/upload-photo?question_id=1&index=0
```

Headers:

```http
Content-Type: image/jpeg
X-Device-Id: YOUR_DEVICE_ID
X-Device-Api-Key: <device-api-key>
X-Question-Id: 1
X-Photo-Index: 0
```

Body:

```text
raw JPEG bytes
```

Expected response:

```json
{
  "ok": true,
  "photo_id": "photo_abc123"
}
```

Purpose:

```text
ESP uploads raw image.
Server stores it in Blob/KV/cache/storage.
Server returns photo_id.
```

---

### 11.2 Solve Question

Endpoint:

```http
POST /api/casio-ai/solve
```

Headers:

```http
Content-Type: application/json
X-Device-Id: YOUR_DEVICE_ID
X-Device-Api-Key: <device-api-key>
```

Body:

```json
{
  "device_id": "YOUR_DEVICE_ID",
  "question_id": 1,
  "photo_count": 2,
  "mode": "gpt-test",
  "model_hint": "gpt-5.4-mini-or-nano",
  "photo_ids": ["photo_abc123", "photo_def456"],
  "context_tail": "Q1 previous answer..."
}
```

Expected response:

```json
{
  "ok": true,
  "answer": "Full AI answer. Can be long.",
  "display_text": "Compact answer optimized for 128x32 OLED.",
  "usage": {
    "input_tokens": 1234,
    "output_tokens": 2000,
    "total_tokens": 3234
  }
}
```

On error:

```json
{
  "ok": false,
  "error": "AI request failed"
}
```

---

## 12. Server AI Prompt Recommendation

The server should ask the model to produce two outputs:

1. full answer
2. compact display text

Suggested instruction:

```text
You are a study assistant for a tiny 128x32 handheld OLED device.
The user submitted one or more photos of a question.

Return:
1. A clear full answer for storage.
2. A compact display_text optimized for a 2-line scrolling OLED.

Rules for display_text:
- Use plain ASCII where possible.
- Avoid Markdown.
- Avoid LaTeX.
- Use short lines.
- Break solution into short steps.
- Keep formulas simple: x^2, sqrt(x), a/b, integral 0..1.
- Explain enough for learning, not just final answer.
```

---

## 13. OpenAI Usage Direction

For this prototype:

```text
Use server-side direct model calls.
Do not use Agent framework.
Do not use Assistant API.
```

Recommended approach:

```text
OpenAI Responses API
vision input
structured or semi-structured JSON output
```

Current development preference:

```text
Testing: cheaper GPT-5.4 mini/nano style model if available
Final personal-use mode: GPT-5.5
```

The ESP32 should not know which exact AI provider is used. It only sends photos and receives answer text.

---

## 14. ESP32 Code Structure

The current `main.cpp` roughly contains:

| Section | Purpose |
|---|---|
| USER CONFIG | WiFi, server URL, device ID/key |
| PIN CONFIG | OLED, buttons, camera pins |
| SYSTEM CONFIG | history limits, timeout, display sizing |
| OLED setup | U8g2 display driver |
| TYPES | photo/chat/button structs |
| GLOBAL STATE | session state and UI state |
| UTIL | memory cleanup, text cleanup, wrapping |
| DISPLAY | 2-line OLED rendering |
| WIFI | WiFi connect logic |
| CAMERA | camera init and capture |
| SERVER API | upload photo and solve calls |
| SOLVE TASK | async upload/AI request task |
| BUTTON ACTIONS | UI navigation logic |
| SETUP | boot/init sequence |
| LOOP | button polling/render loop |

The code currently uses:

```text
PlatformIO
Arduino framework
U8g2
ArduinoJson
esp_camera
WiFi/HTTPClient
```

---

## 15. Development Order

Recommended testing order:

```text
1. ESP32 serial upload test
2. OLED hello world
3. Button input test
4. Camera init test
5. Camera capture test
6. WiFi connect test
7. Upload JPEG to server
8. Server stores photo and returns photo_id
9. Server calls AI and returns display_text
10. Full end-to-end test
11. Battery-powered test
12. Enclosure assembly
```

Do not test everything at once.

---

## 16. Known Technical Risks

### OLED Driver Risk

Screen is SSD1305/SSD1309 class. U8g2 constructor may need adjustment.

Possible symptoms:

```text
compile error
screen blank
garbled pixels
wrong contrast
wrong address
```

First things to check:

```text
I2C address 0x3C vs 0x3D
BS1/BS2 mode selection
OLED reset pin
U8g2 constructor
GPIO conflict
```

### Camera Risk

OV5640 DVP AF may initialize differently from OV2640.

Possible symptoms:

```text
Camera init failed
black image
blurred image
capture timeout
```

Check:

```text
FPC direction
camera pin mapping
camera supply
PSRAM enabled
frame size
jpeg quality
```

### Power Risk

ESP32-S3 + WiFi + camera has high peak current.

Power module should provide:

```text
5V output
at least 1A
preferably 2A
```

Symptoms of weak power:

```text
random reboot
brownout reset
camera capture fails
WiFi disconnect
```

### Memory Risk

Photos are stored in PSRAM before upload.

Avoid storing too many photos at once.

Current max pending photos:

```text
3
```

---

## 17. UX Principles

The device should feel like:

```text
Casio calculator
AI study assistant
low-distraction handheld tool
```

Keep UI text short.

Good UI text:

```text
Ready: CAM photo
Thinking...
Upload 1/2
Ans: x=2
```

Bad UI text:

```text
Your image is currently being processed by the artificial intelligence server...
```

Use simple wording because the screen is tiny.

---

## 18. Future Features

Possible future improvements:

```text
model selection page
token usage page
credits page
WiFi setup page
local saved sessions
server-side history sync
OTA firmware update
better Chinese font support
better math display
streaming AI response
ESP-NOW second display device
```

Optional seventh button can be used for:

```text
page switch
settings
model select
token view
```

---

## 19. Current Design Decision Summary

| Decision | Current Choice |
|---|---|
| AI location | Server-side |
| ESP role | camera + display + buttons |
| Display | 128×32 OLED |
| UI | two-line scrolling |
| Photo flow | capture pending photos, then OK to send |
| Context | RAM-only current session |
| Persistence | not persistent after power off |
| Server storage | server stores photos/results |
| API key | server keeps AI provider key |
| Device key | prototype static device API key |
| Framework | PlatformIO + Arduino |
| Agent framework | not used |

---

## 20. One-Sentence Summary

Casio AI Machine is a calculator-style ESP32-S3 handheld that captures question photos, sends them to a server-side AI pipeline, and displays compact study explanations on a tiny two-line OLED with button-based navigation.
