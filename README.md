# Casio AI Machine

> An unauthorized exam-assistance calculator prototype built from an `ESP32-S3`, an `OV5640` camera, a tiny OLED, and a modified `CASIO fx-570MS / fx-560MS` style shell.
>
> This repository is shared as a hardware/software research record. Do not use this device in real exams, schools, certifications, interviews, or any environment where hidden assistance is prohibited.

## V1 Prototype

<table>
  <tr>
    <td rowspan="2" width="50%"><img src="assets/non-cover-v2-casio.jpg" width="100%" alt="V1 internal build" /></td>
    <td width="50%"><img src="assets/demo1.jpg" width="100%" alt="V1 OLED answer demo" /></td>
  </tr>
  <tr>
    <td width="50%"><img src="assets/demo2.jpg" width="100%" alt="V1 math display demo" /></td>
  </tr>
</table>

## V2 Prototype

<table>
  <tr>
    <td rowspan="2" width="50%"><img src="assets/v2%20back.jpg" width="100%" alt="V2 back" /></td>
    <td width="50%"><img src="assets/v2%20front.jpg" width="100%" alt="V2 front" /></td>
  </tr>
  <tr>
    <td width="50%"><img src="assets/v2%20inside.jpg" width="100%" alt="V2 inside" /></td>
  </tr>
</table>

## Why This Exists

I built this for a close friend who kept failing exams. After talking through his situation, I made this calculator-shaped device as an experiment in hidden AI assistance: a camera captures questions, a server sends them to AI, and the answer is rendered back onto a tiny OLED inside the calculator.

That makes the project technically interesting, but ethically sensitive. The honest description is: this is an exam-cheating machine prototype. It is not a normal learning tool, and it should not be used where it violates rules, trust, or academic integrity.

## What V1 Could Do

- Hide inside a calculator body and look mostly like a normal calculator from the outside.
- Capture question photos with an embedded `OV5640` camera.
- Upload one or more photos for a single question.
- Send photos to a backend server for AI solving.
- Show answers on a `128x32` OLED screen.
- Display Chinese, English, symbols, and math content by using server-rendered `1-bit bitmap` blocks.
- Navigate answers with physical buttons.
- Keep a small sidebar/history of solved questions during a session.
- Use a server pipeline with AI solving, answer formatting, and OLED bitmap rendering.

## What V2 Improves

### Hardware Improvements

- More internal space: the second battery was removed after testing showed a two-hour exam used less than `10%` of the main battery.
- Simpler power layout: V2 keeps the useful `1200mAh` battery and removes the unnecessary `2100mAh` pack.
- Closed back cover: V1 could not fully close because the internals were too crowded; V2 can close properly.
- Safer wiring: V1 wiring was too compressed, and after vibration some connections shorted and damaged the chip. V2 has more space and cleaner routing.
- Better ESP32 placement: the ESP32 and camera connection are closer together, reducing cable strain and camera signal problems.
- Cleaner internal assembly: solder joints and module placement are less cramped.
- Quieter buttons: soft material/glue was added under the button area so button presses are much more silent.
- Stronger privacy screen: the OLED has multiple privacy-film layers, making it difficult to read from the front, side, or angled views.
- More realistic calculator shell: the device looks cleaner and more complete when assembled.

### V2 Trade-Offs

- The camera is now vertical like a phone camera instead of horizontal.
- A full A4-width question may need left/right photos instead of one landscape shot.
- The camera location is mechanically safer, but framing questions is slightly less convenient than V1.

### Software / Firmware Improvements

- Reworked main firmware around a session-based question list instead of fragile local-only answer storage.
- Server now stores question records, photo records, solve status, answer text, and display blocks.
- Device can reboot and pull the current session back from the server.
- Session window is limited to recent study activity instead of loading all historical questions.
- Supports multiple questions in flight: uploading, thinking, rendering, ready, and answer-pulling can happen independently.
- Supports multiple photos per question, up to the firmware/server configured limit.
- Camera pipeline is more stable: warmup frames, controlled JPEG quality, PSRAM copy, immediate camera deinit, XCLK stop, and quiet camera pins.
- Camera overheating is reduced by keeping the camera off except during capture.
- OLED rendering uses server-generated `1-bit XBM` bitmap blocks, avoiding broken Chinese/LaTeX font rendering on the ESP32.
- Answer blocks support text and math layouts with `16px`, `32px`, and `64px` heights.
- Device fetches answer blocks incrementally instead of requiring the entire answer to be loaded before reading.
- Current/open question gets fetch priority; background questions are prefetched when resources allow.
- Bitmap cache is managed locally so old viewed blocks can be cleared and re-fetched from the server later.
- Upload retries and idempotent request tokens reduce duplicate/failure problems.
- Sidebar status handling now includes queued/creating/uploading/thinking/rendering/ready states.
- Serial monitor logs were improved for camera, upload, solve, render, memory, and OLED debugging.
- Thermal guard can warn/shutdown behavior if chip temperature becomes abnormal.

## Technology Stack

### Device

- `ESP32-S3` with PlatformIO + Arduino framework
- `OV5640` camera module
- `128x32` OLED display using `U8g2`
- Physical buttons wired to ESP32 GPIOs
- PSRAM-backed JPEG capture and bitmap cache
- HTTPS upload/polling to backend APIs

### Backend

- Next.js API routes
- Prisma database models and migrations
- Vercel Blob / storage-backed photo upload flow
- OpenAI API for visual question solving and answer formatting
- Server-side rendering of Chinese/math answer blocks into `1-bit` OLED bitmaps
- Session/question/photo/display-block persistence

## Repository Contents

- `src/` - ESP32 firmware and test firmware files.
- `platformio.ini` - PlatformIO environments for main system, camera tests, OLED tests, button tests, and AI solve tests.
- `assets/` - V1/V2 photos, camera output examples, and build photos.
- `server/` - copied Casio AI backend reference from the web/server project.
- `server/app/api/casio-ai/` - legacy and V2 Casio API routes.
- `server/app/utils/casioAi.ts` - AI solve/layout pipeline.
- `server/app/utils/casioDisplay.ts` - OLED bitmap block rendering.
- `server/prisma/` - Prisma schema plus Casio-related migrations.
- `circuit_diagram.md` - wiring and circuit notes.
- `things_to_buy.md` - parts/materials list.
- `STEP_BY_STEP.md` - build and setup walkthrough.
- `ai_prompt_&_harness/` - prompt/harness notes for answer formatting.

## Basic Flow

1. Power on the calculator device.
2. Device starts or resumes a recent server session.
3. Press camera to enter capture mode.
4. Capture one or more photos for a question.
5. Press enter to upload and submit.
6. Server stores the photos, solves the question, formats the answer, and renders OLED blocks.
7. Device polls status, pulls blocks, and displays the answer.
8. User can keep reading while other questions upload/render in the background.

## Open Source Notes

This repo includes firmware, wiring notes, materials, build photos, and a copied backend reference so others can understand how the whole system was put together. Secrets are not committed; use `.env.example` / `.env.local` style configuration for WiFi and API/server credentials.

## Responsible Use

This is a technical artifact, not a recommendation. It demonstrates embedded hardware, tiny-screen rendering, camera capture, server-side AI, and constrained-device UX. Do not deploy or use it for real cheating.

## License

MIT License. See [LICENSE](LICENSE).
