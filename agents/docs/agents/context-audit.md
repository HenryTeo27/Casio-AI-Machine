# Context Audit (Casio-AI-Machine)

This repository is firmware-oriented with generated PlatformIO artifacts.

## Token-Heavy Areas

- `.pio/` build artifact tree
- `build/` generated outputs
- binary and map artifacts (`*.bin`, `*.elf`, `*.hex`, `*.map`, `*.o`, `*.a`)

## Default No-Full-Read Policy

Avoid full reads of:

- `.pio/**`
- `build/**`
- binary artifact patterns

Use targeted search and short windows first.

## Read Order

1. `agents/PROJECT_MAP.md`
2. `AGENTS.md`
3. `platformio.ini`
4. `src/` and related headers/libs as needed
