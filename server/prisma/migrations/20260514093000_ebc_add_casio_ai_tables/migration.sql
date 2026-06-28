-- CreateTable
CREATE TABLE "CasioAiQuestion" (
    "id" TEXT NOT NULL,
    "deviceId" TEXT NOT NULL,
    "questionNo" INTEGER NOT NULL,
    "mode" TEXT,
    "modelHint" TEXT,
    "photoCount" INTEGER NOT NULL DEFAULT 0,
    "status" TEXT NOT NULL DEFAULT 'PENDING',
    "contextTail" TEXT,
    "answer" TEXT,
    "displayText" TEXT,
    "errorMessage" TEXT,
    "promptTokens" INTEGER,
    "completionTokens" INTEGER,
    "totalTokens" INTEGER,
    "completedAt" TIMESTAMP(3),
    "createdAt" TIMESTAMP(3) NOT NULL DEFAULT CURRENT_TIMESTAMP,
    "updatedAt" TIMESTAMP(3) NOT NULL,

    CONSTRAINT "CasioAiQuestion_pkey" PRIMARY KEY ("id")
);

-- CreateTable
CREATE TABLE "CasioAiPhoto" (
    "id" TEXT NOT NULL,
    "questionId" TEXT NOT NULL,
    "deviceId" TEXT NOT NULL,
    "questionNo" INTEGER NOT NULL,
    "photoIndex" INTEGER NOT NULL,
    "blobUrl" TEXT NOT NULL,
    "blobPath" TEXT NOT NULL,
    "contentType" TEXT NOT NULL,
    "sizeBytes" INTEGER NOT NULL,
    "sha256" TEXT NOT NULL,
    "createdAt" TIMESTAMP(3) NOT NULL DEFAULT CURRENT_TIMESTAMP,
    "updatedAt" TIMESTAMP(3) NOT NULL,

    CONSTRAINT "CasioAiPhoto_pkey" PRIMARY KEY ("id")
);

-- CreateIndex
CREATE UNIQUE INDEX "CasioAiQuestion_deviceId_questionNo_key" ON "CasioAiQuestion"("deviceId", "questionNo");

-- CreateIndex
CREATE INDEX "CasioAiQuestion_deviceId_createdAt_idx" ON "CasioAiQuestion"("deviceId", "createdAt");

-- CreateIndex
CREATE INDEX "CasioAiQuestion_status_createdAt_idx" ON "CasioAiQuestion"("status", "createdAt");

-- CreateIndex
CREATE UNIQUE INDEX "CasioAiPhoto_deviceId_questionNo_photoIndex_key" ON "CasioAiPhoto"("deviceId", "questionNo", "photoIndex");

-- CreateIndex
CREATE INDEX "CasioAiPhoto_questionId_photoIndex_idx" ON "CasioAiPhoto"("questionId", "photoIndex");

-- CreateIndex
CREATE INDEX "CasioAiPhoto_deviceId_createdAt_idx" ON "CasioAiPhoto"("deviceId", "createdAt");

-- AddForeignKey
ALTER TABLE "CasioAiPhoto"
ADD CONSTRAINT "CasioAiPhoto_questionId_fkey"
FOREIGN KEY ("questionId") REFERENCES "CasioAiQuestion"("id")
ON DELETE CASCADE ON UPDATE CASCADE;
