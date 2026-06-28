-- AlterTable
ALTER TABLE "CasioAiQuestion"
ADD COLUMN "displayBlocks" JSONB NOT NULL DEFAULT '[]';
