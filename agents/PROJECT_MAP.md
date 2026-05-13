# Casio-AI-Machine Project Map

Use this map before broad search. This repo is firmware-first (PlatformIO).

## Default Entry Points

- Agent policy: `AGENTS.md`
- Canonical map: `agents/PROJECT_MAP.md`
- Agent docs: `agents/docs/agents/`
- Skills: `skills/`
- Firmware config: `platformio.ini`
- Main firmware code: `src/`
- Headers and shared interfaces: `include/`, `lib/`
- Tests: `test/`
- Context packing config: `repomix.config.json`

## Context-Saving Workflow

1. Classify task:
   - policy/process task;
   - firmware implementation task;
   - build/config task.
2. Search only likely paths first.
3. Read short windows around exact matches.
4. Expand scope only if uncertainty remains.

## Task Routing

- Policy/process: `AGENTS.md`, `agents/docs/agents/*.md`
- Firmware feature/bugfix: `src/`, then `include/` and `lib/`
- Build config: `platformio.ini`
- Tests: `test/`
- Skill updates: `skills/<skill-name>/`

## High-Risk Token Areas

- `.pio/` and `build/`
- binary outputs (`*.bin`, `*.elf`, `*.hex`, `*.map`, `*.o`, `*.a`)
- large generated logs/artifacts

Default behavior:

- no full-read of high-risk areas;
- targeted query + short windows only;
- explicit path + purpose before opening.
