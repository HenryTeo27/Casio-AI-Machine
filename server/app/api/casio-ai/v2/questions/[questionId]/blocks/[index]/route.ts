import { authenticateCasioRequest, parseIntStrict, safeText } from "@/app/utils/casioAi";
import prisma from "@/utils/prisma";
import { randomUUID } from "crypto";
import { after, NextRequest, NextResponse } from "next/server";
import { normalizeDisplayBlocks, runRenderJobV2 } from "../../../../_shared";

type RouteContext = { params: Promise<{ questionId: string; index: string }> };

export async function GET(request: NextRequest, { params }: RouteContext) {
    try {
        const auth = authenticateCasioRequest(request);
        if (!auth.ok) {
            return NextResponse.json({ ok: false, error: auth.error }, { status: auth.status });
        }

        const { questionId, index: indexParam } = await params;
        const index = parseIntStrict(indexParam);
        if (index === null || index < 0) {
            return NextResponse.json({ ok: false, error: "invalid_block_index" }, { status: 400 });
        }

        const question = await prisma.casioAiQuestion.findFirst({
            where: { id: questionId, deviceId: auth.deviceId },
            select: {
                id: true,
                displayOrder: true,
                questionNo: true,
                status: true,
                answer: true,
                displayText: true,
                modelHint: true,
                displayBlocks: true,
                errorMessage: true,
                promptTokens: true,
                completionTokens: true,
                totalTokens: true,
                updatedAt: true
            }
        });

        if (!question) {
            return NextResponse.json({ ok: false, error: "question_not_found", status: "NOT_FOUND" }, { status: 404 });
        }
        if (question.status !== "SUCCEEDED") {
            return NextResponse.json({
                ok: false,
                error: question.status === "FAILED" ? safeText(question.errorMessage) || "question_failed" : "question_not_ready",
                status: question.status,
                updated_at: question.updatedAt.toISOString()
            }, { status: question.status === "FAILED" ? 500 : 202 });
        }

        const blocks = normalizeDisplayBlocks(question.displayBlocks);
        const block = blocks[index];
        if (!block) {
            if (blocks.length === 0) {
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
                    status: "RENDERING",
                    index,
                    count: 0,
                    poll_after_ms: 3000
                }, { status: 202 });
            }

            return NextResponse.json({
                ok: false,
                error: "block_not_found",
                status: question.status,
                index,
                count: blocks.length
            }, { status: 404 });
        }

        return NextResponse.json({
            ok: true,
            question_id: question.id,
            display_order: question.displayOrder || question.questionNo,
            status: question.status,
            index,
            count: blocks.length,
            block
        });
    } catch (error: unknown) {
        const message = safeText(
            error && typeof error === "object" && "message" in error
                ? (error as { message?: unknown }).message
                : "question_block_fetch_failed"
        );
        return NextResponse.json({ ok: false, error: message || "question_block_fetch_failed" }, { status: 500 });
    }
}
