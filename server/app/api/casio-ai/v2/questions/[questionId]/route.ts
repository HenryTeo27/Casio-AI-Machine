import { authenticateCasioRequest, safeText } from "@/app/utils/casioAi";
import prisma from "@/utils/prisma";
import { randomUUID } from "crypto";
import { after, NextRequest, NextResponse } from "next/server";
import {
    buildQuestionResponse,
    normalizeDisplayBlocks,
    recoverStaleUploadQuestions,
    runAnswerJobV2,
    runRenderJobV2
} from "../../_shared";

type RouteContext = { params: Promise<{ questionId: string }> };

export const maxDuration = 300;

export async function GET(request: NextRequest, { params }: RouteContext) {
    try {
        const auth = authenticateCasioRequest(request);
        if (!auth.ok) {
            return NextResponse.json({ ok: false, error: auth.error }, { status: auth.status });
        }

        const { questionId } = await params;
        const recoveryJobs = await recoverStaleUploadQuestions({
            deviceId: auth.deviceId,
            questionId
        });
        for (const job of recoveryJobs) {
            after(() => runAnswerJobV2(job));
        }

        const question = await prisma.casioAiQuestion.findFirst({
            where: { id: questionId, deviceId: auth.deviceId },
            select: {
                id: true,
                questionNo: true,
                displayOrder: true,
                status: true,
                answer: true,
                displayText: true,
                displayBlocks: true,
                modelHint: true,
                solveToken: true,
                photoCount: true,
                errorMessage: true,
                promptTokens: true,
                completionTokens: true,
                totalTokens: true,
                completedAt: true,
                updatedAt: true
            }
        });

        if (!question) {
            return NextResponse.json({ ok: false, error: "question_not_found", status: "NOT_FOUND" }, { status: 404 });
        }

        if (question.status === "ANSWER_READY") {
            const answer = safeText(question.answer);
            if (!answer) {
                await prisma.casioAiQuestion.updateMany({
                    where: { id: question.id, status: "ANSWER_READY" },
                    data: {
                        status: "FAILED",
                        errorMessage: "answer_missing",
                        completedAt: new Date()
                    }
                });
                return NextResponse.json({
                    ok: false,
                    error: "answer_missing",
                    question_id: question.id,
                    status: "FAILED"
                }, { status: 500 });
            }

            const renderToken = `casio_v2_render:${randomUUID()}`;
            const transition = await prisma.casioAiQuestion.updateMany({
                where: { id: question.id, status: "ANSWER_READY" },
                data: {
                    status: "RENDERING",
                    solveToken: renderToken
                }
            });

            if (transition.count > 0) {
                after(() => runRenderJobV2({
                    questionId: question.id,
                    deviceId: auth.deviceId,
                    jobToken: renderToken,
                    answer,
                    displayText: safeText(question.displayText),
                    model: safeText(question.modelHint),
                    promptTokens: question.promptTokens || 0,
                    completionTokens: question.completionTokens || 0,
                    totalTokens: question.totalTokens || 0
                }));
            }

            return NextResponse.json({
                ok: false,
                error: "question_not_ready",
                question_id: question.id,
                display_order: question.displayOrder || question.questionNo,
                status: "RENDERING",
                photo_count: question.photoCount,
                poll_after_ms: 3000,
                updated_at: new Date().toISOString()
            }, { status: 202 });
        }

        if (question.status !== "SUCCEEDED") {
            return NextResponse.json({
                ok: false,
                error: question.status === "FAILED" ? safeText(question.errorMessage) || "question_failed" : "question_not_ready",
                question_id: question.id,
                display_order: question.displayOrder || question.questionNo,
                status: question.status,
                photo_count: question.photoCount,
                poll_after_ms: question.status === "FAILED" ? 0 : 3000,
                updated_at: question.updatedAt.toISOString()
            }, { status: question.status === "FAILED" ? 500 : 202 });
        }

        if (normalizeDisplayBlocks(question.displayBlocks).length === 0) {
            const answer = safeText(question.answer);
            if (!answer) {
                await prisma.casioAiQuestion.updateMany({
                    where: { id: question.id, deviceId: auth.deviceId, status: "SUCCEEDED" },
                    data: {
                        status: "FAILED",
                        errorMessage: "answer_missing_for_render_repair",
                        completedAt: new Date()
                    }
                });
                return NextResponse.json({
                    ok: false,
                    error: "answer_missing_for_render_repair",
                    question_id: question.id,
                    status: "FAILED"
                }, { status: 500 });
            }

            const renderToken = `casio_v2_render_repair:${randomUUID()}`;
            const transition = await prisma.casioAiQuestion.updateMany({
                where: { id: question.id, deviceId: auth.deviceId, status: "SUCCEEDED" },
                data: {
                    status: "RENDERING",
                    solveToken: renderToken
                }
            });

            if (transition.count > 0) {
                after(() => runRenderJobV2({
                    questionId: question.id,
                    deviceId: auth.deviceId,
                    jobToken: renderToken,
                    answer,
                    displayText: safeText(question.displayText),
                    model: safeText(question.modelHint),
                    promptTokens: question.promptTokens || 0,
                    completionTokens: question.completionTokens || 0,
                    totalTokens: question.totalTokens || 0
                }));
            }

            return NextResponse.json({
                ok: false,
                error: "display_blocks_missing",
                question_id: question.id,
                display_order: question.displayOrder || question.questionNo,
                status: "RENDERING",
                photo_count: question.photoCount,
                display_block_count: 0,
                poll_after_ms: 3000,
                updated_at: new Date().toISOString()
            }, { status: 202 });
        }

        return NextResponse.json(buildQuestionResponse(question));
    } catch (error: unknown) {
        const message = safeText(
            error && typeof error === "object" && "message" in error
                ? (error as { message?: unknown }).message
                : "question_fetch_failed"
        );
        return NextResponse.json({ ok: false, error: message || "question_fetch_failed" }, { status: 500 });
    }
}
