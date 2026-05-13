# Casio-AI-Machine Agent Guide

This repository is firmware-oriented (PlatformIO). Keep search/edit scope narrow to save tokens.

## 1) Search Policy

- Start from `agents/PROJECT_MAP.md` before searching.
- Do not run broad recursive PowerShell dumps.
- Prefer targeted `rg` with explicit paths.
- Use `Select-String` fallback only on narrow scopes.
- Limit each read to short windows (60-90 lines) when file size is unknown.
- Default `rg` scope:
  - `rg -n "<pattern>" agents skills src include lib test platformio.ini`
- Expand scope only after no-hit in default scope.

## 2) Token Hygiene

- Respect `.rgignore` defaults.
- Do not open `.pio/` or `build/` by default.
- Avoid full reads of generated maps/binaries.
- Prefer exact symbol/path queries over broad keywords.

## 3) Output Compression and Raw Fallback

- Use `rtk` for noisy commands:
  - `git status`, `git diff`, `git log`, `rg`, `ls/tree`, test output.
- If compressed output is unclear, rerun one raw command in the smallest scope.

## 4) UTF-8 Rule

- Read docs/tasks with UTF-8 in PowerShell:
  - `Get-Content -Encoding UTF8 <file>`

## 5) Large File Rule

- Default: no full-read for large files.
- Use targeted search + short window reads first.
- Expand only when ambiguity remains.

## 6) Repomix Rule

- Use `repomix.config.json` for context packing.
- Exclude generated/cache/static/binary-heavy outputs.
- Keep compressed bundle mode enabled.

## 7) Edit Scope Rule

- Default editable scope:
  - `agents/**`
  - `skills/**`
  - firmware sources (`src/**`, `include/**`, `lib/**`, `test/**`)
  - root policy/config docs (`AGENTS.md`, `PROJECT_MAP.md`, `.rgignore`, `repomix.config.json`)
- Do not modify by default:
  - `.pio/**`, `build/**`, generated binaries

## 8) Info Entry Rule

- Project basics: `agents/PROJECT_MAP.md`
- Policy details: `agents/docs/agents/context-audit.md`
- Firmware config: `platformio.ini`
