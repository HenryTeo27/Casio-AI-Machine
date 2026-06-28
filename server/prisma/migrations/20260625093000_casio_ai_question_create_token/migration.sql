-- Idempotent Casio v2 question creation for unstable device/network retries.

ALTER TABLE "CasioAiQuestion"
    ADD COLUMN "createToken" TEXT;

CREATE UNIQUE INDEX "CasioAiQuestion_deviceId_createToken_key"
    ON "CasioAiQuestion"("deviceId", "createToken");
