# Skill: Token Hygiene

Purpose: enforce low-context operating mode in this repository.

## Runbook

1. Read `AGENTS.md` and `agents/PROJECT_MAP.md`.
2. Start search in narrow scope (`agents`, `skills`, `src`, `include`, `lib`, `test`); use `server` only for backend tasks.
3. Avoid default reads in `.pio/`, `build/`, and `server/app/utils/fonts/`.
4. Use short windows around exact hits.
5. Use `rtk` for noisy command outputs.
6. If compressed output is unclear, rerun one raw command narrowly.
