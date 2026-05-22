# Casio AI Machine Server Backend Copy
This folder is a copy-paste backend reference, not a standalone runnable server.
Create your own Next.js TypeScript backend, then copy the relevant API routes, utils, Prisma models, and migrations into it.

This folder is a direct reference copy of the Casio AI Machine backend pieces from:

`C:\Users\User\project\EstateBoostCopilot\general`

It is intended for open-source migration/reference. Sensitive values, URLs, project-specific aliases, and deployment settings should be reviewed before use.

## Copied Backend API

- `app/api/casio-ai/upload-photo/route.ts`
  - Uploads JPEG photos from ESP32.
  - Stores photo metadata in Prisma.
  - Stores image bytes in Vercel Blob.
- `app/api/casio-ai/solve/route.ts`
  - Starts/continues solve workflow.
  - Loads uploaded photos.
  - Calls OpenAI solve prompt.
  - Saves answer/status/usage in DB.
- `app/api/casio-ai/question/route.ts`
  - Fetches question status/result.
  - Can continue OLED render workflow when answer exists but display blocks are not ready.
- `app/api/casio-ai/question-block/route.ts`
  - Fetches one display block at a time for ESP32 incremental loading.

## Copied Utilities

- `app/utils/casioAi.ts`
  - Casio device auth.
  - OpenAI solve/layout calls.
  - Retry/fallback logic.
  - Usage/status normalization.
- `app/utils/casioDisplay.ts`
  - OLED bitmap rendering helpers.
  - Text/formula layout rendering helpers.
  - 1-bit XBM/base64 conversion.
- `app/utils/casioFontconfig.ts`
  - Fontconfig setup for serverless rendering.
- `app/utils/fonts/`
  - Font files used by the renderer.
- `utils/prisma.ts`
  - Prisma client setup used by the copied routes.

## Copied DB Parts

- `prisma/casio-models.prisma`
  - Casio-only Prisma model excerpt.
- `prisma/migrations/20260514093000_ebc_add_casio_ai_tables/migration.sql`
  - Creates `CasioAiQuestion` and `CasioAiPhoto`.
- `prisma/migrations/20260519100000_ebc_add_casio_display_blocks/migration.sql`
  - Adds display-block related DB fields.

## Required Runtime Dependencies

The copied backend expects a Next.js/Node server environment with at least:

- `@prisma/client`
- `@prisma/adapter-pg`
- `@vercel/blob`
- `mathjax-full`
- `sharp`
- `prisma`

## Required Environment Variables

Review and replace these before deployment:

- `POSTGRES_URL`
- `BLOB_READ_WRITE_TOKEN`
- `OPENAI_API_KEY`
- `CASIO_AI_DEVICE_ID`
- `CASIO_AI_DEVICE_API_KEY`
- `CASIO_AI_OPENAI_SOLVE_PROMPT_ID`
- `CASIO_AI_OPENAI_SOLVE_PROMPT_VERSION`
- `CASIO_AI_OPENAI_LAYOUT_PROMPT_ID`
- `CASIO_AI_OPENAI_LAYOUT_PROMPT_VERSION`
- Optional rendering/model tuning env vars referenced in `app/utils/casioAi.ts` and `app/utils/casioDisplay.ts`.

