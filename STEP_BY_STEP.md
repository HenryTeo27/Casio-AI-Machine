# Casio AI Machine - Step by Step

This guide walks you through the full build flow from zero to a working system.

Use this project responsibly for learning and revision workflows.

## 0. Before You Start

1. Decide your goal and expected user experience.
2. Read these files first:
   - [things_to_buy.md](things_to_buy.md)
   - [circuit_diagram.md](circuit_diagram.md)
   - [README.md](README.md)
3. Check reference photos and internal layout ideas in `assets/`.

## 1. Buy All Parts

1. Open [things_to_buy.md](things_to_buy.md).
2. Purchase all required modules and tools from the provided links.
3. Verify you have at least:
   - ESP32-S3 board
   - OV5640 camera
   - 128x32 OLED
   - battery modules and charging module
   - perfboard, switches, wires, solder tools, grinding tools, glue

## 2. Open and Strip the Calculator Shell

1. Disassemble the calculator body with screws.
2. Remove original internal parts, but keep the keycaps/buttons you still need.
3. Grind and cut inner plastic structures until enough space is available.
4. Thin both top and bottom shell where needed.
5. Target usable internal height:
   - baseline around `6mm`
   - after thinning, aim near `8mm`

Notes:
- Grinding and fitment are critical to this project.
- Keep checking alignment against `assets/` reference images.

## 3. Reattach Unused Buttons and Prepare Key Area

1. For non-functional buttons, glue them back into the shell for appearance consistency.
2. Ensure button caps still fit naturally and do not block active buttons.
3. Dry-fit shell halves repeatedly to avoid later clearance issues.

## 4. Build Power System

1. Solder two battery modules and one charging module according to the circuit plan.
2. Route and secure wires safely.
3. Mount power modules into shell using double-sided tape.
4. Keep heat paths and charging-port access in mind.

## 5. Build the Button Matrix Board

1. Use perfboard to create a custom button zone matching your shell layout.
2. Use two button types:
   - latching switches
   - momentary tactile switches
3. Solder all switches, polarity wiring, and signal lines.
4. Label wires early to avoid pin-mapping confusion later.

## 6. Connect Buttons to ESP32 and Test Inputs

1. Connect button wires to target ESP32 pins.
2. Use test programs in `src/` to verify each button.
3. Confirm every direction/action key reads correctly before moving on.
4. Use PlatformIO, not Arduino IDE:
   - project config: `platformio.ini`

## 7. Connect and Test Camera

1. Connect OV5640 to ESP32 DVP-related pins exactly as designed.
2. Check physical contact quality carefully.
3. If contact is unstable, photos may show tearing/noise.
4. Run camera test module from `src/`.
5. Optionally run web tune mode (`camera_web_tune`) to preview and tune camera behavior in browser.

## 8. Bring Up Backend Server and Database

1. Create your own server project (TypeScript/Next.js recommended).
2. Copy backend reference from this repo:
   - `server/` folder (API, utils, prisma models/migrations)
   - [server/README.md](server/README.md)
3. Configure your own env values, keys, DB, and storage.
4. Deploy server.
5. Verify upload and question APIs are reachable from device.

## 9. Connect and Test OLED

1. Wire OLED to ESP32 as shown in circuit docs.
2. Run OLED module tests from `src/`.
3. Confirm text and bitmap drawing both work.

## 10. Full Electrical Validation

1. Test complete power path:
   - battery charge/discharge
   - stable boot while powered by battery
2. Observe thermal behavior:
   - battery
   - ESP32
   - camera module
3. Fix bad solder joints, unstable connectors, or noisy signal lines.

## 11. Validate Bitmap Rendering Pipeline

1. Run OLED bitmap demo test scripts from `src/` and `test/`.
2. Verify one-bit bitmap blocks display correctly.
3. Ensure formula-heavy and mixed-language content remain readable.

## 12. Configure AI Prompt and Harness

1. Use [ai_prompt_&_harness/README.md](ai_prompt_&_harness/README.md).
2. Configure prompt and schema in your AI platform.
3. Set up two-stage workflow:
   - Stage 1: solve answer
   - Stage 2: layout blocks JSON
4. Ensure server consumes stage-2 JSON and renders final OLED bitmap blocks.

## 13. End-to-End System Test

1. Flash and run main firmware (`main_system`).
2. Test complete flow:
   - capture
   - upload
   - solve
   - layout
   - render
   - block fetch
   - OLED display
3. If output layout is not ideal, use `ai_solve_test` and related test scripts to calibrate behavior.

## 14. Final Integration

1. Lock stable pin mapping and camera parameters.
2. Clean internal wire routing and secure moving parts.
3. Reassemble shell.
4. Run long-duration test (multiple capture/solve cycles).
5. Confirm battery life and usability match your target.

## Troubleshooting Checklist

1. Camera output noisy or torn:
   - recheck OV5640 connector contact and pin mapping.
2. OLED shows malformed output:
   - inspect JSON blocks, then inspect bitmap render stage.
3. Solve works but display fails:
   - test `/question` and `/question-block` API separately.
4. Random resets:
   - check power rails, solder joints, and short circuits.

## Related Docs

- [README.md](README.md)
- [things_to_buy.md](things_to_buy.md)
- [circuit_diagram.md](circuit_diagram.md)
- [server/README.md](server/README.md)
- [ai_prompt_&_harness/README.md](ai_prompt_&_harness/README.md)
