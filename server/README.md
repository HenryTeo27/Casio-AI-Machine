# Casio AI Backend Reference

This folder is a copied reference of the Casio AI backend pieces from the EstateBoostCopilot web/server project. It is included so the firmware repository contains enough backend context for open-source readers to understand and rebuild the device pipeline.

The source project was not modified while creating this copy.

## Included Files

- `app/api/casio-ai/` - legacy upload/solve routes and the newer V2 session/question routes.
- `app/utils/casioAi.ts` - AI solve and OLED-layout orchestration.
- `app/utils/casioDisplay.ts` - server-side text/formula rendering into `1-bit_xbm` OLED bitmap blocks.
- `app/utils/casioFontconfig.ts` and `app/utils/fonts/` - font setup used by the bitmap renderer.
- `utils/prisma.ts` - Prisma client helper used by the routes.
- `prisma/schema.prisma` - copied schema for reference.
- `prisma/migrations/20260514093000_ebc_add_casio_ai_tables/` - initial Casio AI tables.
- `prisma/migrations/20260519100000_ebc_add_casio_display_blocks/` - display block storage.
- `prisma/migrations/20260624090000_casio_ai_v2_session/` - V2 session/question pipeline.
- `prisma/migrations/20260625093000_casio_ai_question_create_token/` - idempotent question creation token.

## What The Backend Does

1. Starts/resumes a device session.
2. Creates question records.
3. Accepts one or more uploaded photos per question.
4. Stores photo metadata and blob URLs.
5. Runs AI solving against the uploaded photos.
6. Runs answer-layout formatting for tiny OLED screens.
7. Renders Chinese/math/text blocks into `1-bit` bitmap payloads.
8. Lets the ESP32 poll status and fetch answer blocks incrementally.

## Important Notes

- This is a reference copy, not a standalone Next.js app.
- You still need the parent web app dependencies, environment variables, database, blob storage, and OpenAI configuration to run it.
- Do not commit real `.env` secrets.
- If the production backend changes, re-copy the Casio-specific route/util/prisma files into this folder.
