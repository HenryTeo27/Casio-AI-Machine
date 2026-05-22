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
- Server backend reference copy: `server/`
- Tests: `test/`
- Context packing config: `repomix.config.json`

## Context-Saving Workflow

1. Classify task:
   - policy/process task;
   - firmware implementation task;
   - build/config task;
   - server/backend reference task.
2. Search only likely paths first.
3. Read short windows around exact matches.
4. Expand scope only if uncertainty remains.

## Task Routing

- Policy/process: `AGENTS.md`, `agents/docs/agents/*.md`
- Firmware feature/bugfix: `src/`, then `include/` and `lib/`
- Server/API/DB reference: `server/README.md`, then `server/app/api/`, `server/app/utils/`, `server/prisma/`
- Build config: `platformio.ini`
- Tests: `test/`
- Skill updates: `skills/<skill-name>/`

## High-Risk Token Areas

- `.pio/` and `build/`
- binary outputs (`*.bin`, `*.elf`, `*.hex`, `*.map`, `*.o`, `*.a`)
- large generated logs/artifacts
- server font assets: `server/app/utils/fonts/`

Default behavior:

- no full-read of high-risk areas;
- targeted query + short windows only;
- explicit path + purpose before opening.

## Server Backend Reference

The `server/` folder is a copied reference of the EstateBoostCopilot Casio backend.

- API routes: `server/app/api/casio-ai/`
- Casio utilities: `server/app/utils/casioAi.ts`, `server/app/utils/casioDisplay.ts`, `server/app/utils/casioFontconfig.ts`
- Prisma helper: `server/utils/prisma.ts`
- DB models/migrations: `server/prisma/`
- Start with `server/README.md`; do not full-read large utils unless the task specifically needs them.
