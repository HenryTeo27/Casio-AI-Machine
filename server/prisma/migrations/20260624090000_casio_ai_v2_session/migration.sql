-- Casio AI Machine v2 server-source-of-truth/session support

CREATE TABLE "CasioAiSession" (
    "id" TEXT NOT NULL,
    "deviceId" TEXT NOT NULL,
    "title" TEXT,
    "status" TEXT NOT NULL DEFAULT 'ACTIVE',
    "createdAt" TIMESTAMP(3) NOT NULL DEFAULT CURRENT_TIMESTAMP,
    "updatedAt" TIMESTAMP(3) NOT NULL,

    CONSTRAINT "CasioAiSession_pkey" PRIMARY KEY ("id")
);

ALTER TABLE "CasioAiQuestion"
    ADD COLUMN "sessionId" TEXT,
    ADD COLUMN "displayOrder" INTEGER NOT NULL DEFAULT 0,
    ADD COLUMN "legacyQuestionNo" INTEGER,
    ADD COLUMN "solveToken" TEXT;

UPDATE "CasioAiQuestion"
SET "displayOrder" = "questionNo",
    "legacyQuestionNo" = "questionNo"
WHERE "displayOrder" = 0;

ALTER TABLE "CasioAiPhoto"
    ADD COLUMN "uploadToken" TEXT;

CREATE INDEX "CasioAiSession_deviceId_updatedAt_idx" ON "CasioAiSession"("deviceId", "updatedAt");
CREATE INDEX "CasioAiSession_deviceId_status_updatedAt_idx" ON "CasioAiSession"("deviceId", "status", "updatedAt");
CREATE INDEX "CasioAiQuestion_deviceId_sessionId_displayOrder_idx" ON "CasioAiQuestion"("deviceId", "sessionId", "displayOrder");
CREATE INDEX "CasioAiQuestion_deviceId_status_updatedAt_idx" ON "CasioAiQuestion"("deviceId", "status", "updatedAt");
CREATE UNIQUE INDEX "CasioAiPhoto_questionId_photoIndex_key" ON "CasioAiPhoto"("questionId", "photoIndex");
CREATE UNIQUE INDEX "CasioAiPhoto_deviceId_uploadToken_key" ON "CasioAiPhoto"("deviceId", "uploadToken");

ALTER TABLE "CasioAiQuestion"
    ADD CONSTRAINT "CasioAiQuestion_sessionId_fkey"
    FOREIGN KEY ("sessionId") REFERENCES "CasioAiSession"("id") ON DELETE SET NULL ON UPDATE CASCADE;
