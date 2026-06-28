/* eslint-disable no-console */
import {
    authenticateCasioRequest,
    parseIntStrict,
    renderCasioAnswerForOledWithOpenAi,
    safeText
} from "@/app/utils/casioAi";
import prisma from "@/utils/prisma";
import { randomUUID } from "crypto";
import { Prisma } from "@prisma/client";
import { after, NextRequest, NextResponse } from "next/server";

export const maxDuration = 300;

const INLINE_DISPLAY_BLOCK_BYTE_LIMIT = 32_000;
const LOG_PREFIX = "[CASIO_AI_QUESTION]";
const RENDER_JOB_PREFIX = "casio_render_job:";

type CasioDisplayBlockPayload = {
    type: "bitmap";
    kind: "text" | "formula";
    width: number;
    height: number;
    format: "1bit_xbm";
    data: string;
};

function normalizeDisplayBlocks(value: unknown): CasioDisplayBlockPayload[] {
    if (!Array.isArray(value)) return [];
    return value.filter((item): item is CasioDisplayBlockPayload => {
        if (!item || typeof item !== "object") return false;
        const block = item as Partial<CasioDisplayBlockPayload>;
        return (
            block.type === "bitmap" &&
            (block.kind === "text" || block.kind === "formula") &&
            typeof block.width === "number" &&
            typeof block.height === "number" &&
            (block.height === 16 || block.height === 32) &&
            block.format === "1bit_xbm" &&
            typeof block.data === "string"
        );
    });
}

function resolveQuestionNo(request: NextRequest): number | null {
    const params = new URL(request.url).searchParams;
    return parseIntStrict(params.get("question_id"));
}

async function runRenderJob(args: {
    questionDbId: string;
    questionNo: number;
    jobToken: string;
    answer: string;
    displayText: string;
    model: string;
    promptTokens: number;
    completionTokens: number;
    totalTokens: number;
}) {
    const startedAt = Date.now();
    try {
        const {
            displayText: finalDisplayText,
            displayBlocks: finalDisplayBlocks,
            usage: finalUsage
        } = await renderCasioAnswerForOledWithOpenAi({
            answer: args.answer,
            displayText: args.displayText,
            model: args.model
        });

        const updated = await prisma.casioAiQuestion.updateMany({
            where: {
                id: args.questionDbId,
                status: "RENDERING",
                modelHint: args.jobToken
            },
            data: {
                status: "SUCCEEDED",
                modelHint: args.model,
                displayText: finalDisplayText,
                displayBlocks:
                    finalDisplayBlocks as unknown as Prisma.InputJsonValue,
                promptTokens: args.promptTokens + finalUsage.promptTokens,
                completionTokens:
                    args.completionTokens + finalUsage.completionTokens,
                totalTokens: args.totalTokens + finalUsage.totalTokens,
                errorMessage: null,
                completedAt: new Date()
            }
        });

        if (updated.count === 0) {
            console.warn(`${LOG_PREFIX} render_job_stale_ignored`, {
                totalMs: Date.now() - startedAt,
                questionNo: args.questionNo
            });
            return;
        }

        console.log(`${LOG_PREFIX} render_job_done`, {
            totalMs: Date.now() - startedAt,
            questionNo: args.questionNo,
            displayBlocks: finalDisplayBlocks.length,
            renderTokens: finalUsage
        });
    } catch (error: unknown) {
        const message =
            safeText(
                error && typeof error === "object" && "message" in error
                    ? (error as { message?: unknown }).message
                    : "casio_ai_render_failed"
            ) || "casio_ai_render_failed";

        console.error(`${LOG_PREFIX} render_job_failed`, {
            totalMs: Date.now() - startedAt,
            questionNo: args.questionNo,
            message
        });

        const updated = await prisma.casioAiQuestion.updateMany({
            where: {
                id: args.questionDbId,
                status: "RENDERING",
                modelHint: args.jobToken
            },
            data: {
                status: "FAILED",
                modelHint: args.model,
                errorMessage: message,
                completedAt: new Date()
            }
        });
        if (updated.count === 0) {
            console.warn(`${LOG_PREFIX} render_job_failed_stale_ignored`, {
                totalMs: Date.now() - startedAt,
                questionNo: args.questionNo,
                message
            });
        }
    }
}

export async function GET(request: NextRequest) {
    try {
        const auth = authenticateCasioRequest(request);
        if (!auth.ok) {
            return NextResponse.json(
                { ok: false, error: auth.error },
                { status: auth.status }
            );
        }

        const questionNo = resolveQuestionNo(request);
        if (questionNo === null || questionNo < 0) {
            return NextResponse.json(
                { ok: false, error: "invalid_question_id" },
                { status: 400 }
            );
        }

        const question = await prisma.casioAiQuestion.findUnique({
            where: {
                deviceId_questionNo: {
                    deviceId: auth.deviceId,
                    questionNo
                }
            },
            select: {
                id: true,
                questionNo: true,
                status: true,
                answer: true,
                displayText: true,
                displayBlocks: true,
                modelHint: true,
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
            return NextResponse.json(
                {
                    ok: false,
                    error: "question_not_found",
                    status: "NOT_FOUND",
                    question_id: questionNo
                },
                { status: 404 }
            );
        }

        if (question.status === "ANSWER_READY") {
            const answer = safeText(question.answer);
            if (!answer) {
                await prisma.casioAiQuestion.updateMany({
                    where: {
                        id: question.id,
                        status: "ANSWER_READY"
                    },
                    data: {
                        status: "FAILED",
                        errorMessage: "answer_missing",
                        completedAt: new Date()
                    }
                });

                return NextResponse.json(
                    {
                        ok: false,
                        error: "answer_missing",
                        status: "FAILED",
                        question_id: question.questionNo,
                        photo_count: question.photoCount,
                        updated_at: question.updatedAt.toISOString()
                    },
                    { status: 500 }
                );
            }

            const renderJobToken = `${RENDER_JOB_PREFIX}${randomUUID()}`;
            const transition = await prisma.casioAiQuestion.updateMany({
                where: {
                    id: question.id,
                    status: "ANSWER_READY"
                },
                data: {
                    status: "RENDERING",
                    modelHint: renderJobToken
                }
            });

            if (transition.count === 0) {
                return NextResponse.json(
                    {
                        ok: false,
                        error: "question_not_ready",
                        status: "PROCESSING",
                        question_id: question.questionNo,
                        photo_count: question.photoCount,
                        updated_at: new Date().toISOString()
                    },
                    { status: 202 }
                );
            }

            after(() =>
                runRenderJob({
                    questionDbId: question.id,
                    questionNo: question.questionNo,
                    jobToken: renderJobToken,
                    answer,
                    displayText: safeText(question.displayText),
                    model: safeText(question.modelHint),
                    promptTokens: question.promptTokens || 0,
                    completionTokens: question.completionTokens || 0,
                    totalTokens: question.totalTokens || 0
                })
            );

            return NextResponse.json(
                {
                    ok: false,
                    error: "question_not_ready",
                    status: "RENDERING",
                    question_id: question.questionNo,
                    photo_count: question.photoCount,
                    updated_at: new Date().toISOString()
                },
                { status: 202 }
            );
        }

        if (question.status !== "SUCCEEDED") {
            return NextResponse.json(
                {
                    ok: false,
                    error:
                        question.status === "FAILED"
                            ? safeText(question.errorMessage) ||
                              "question_failed"
                            : "question_not_ready",
                    status: question.status,
                    question_id: question.questionNo,
                    photo_count: question.photoCount,
                    updated_at: question.updatedAt.toISOString()
                },
                { status: question.status === "FAILED" ? 500 : 202 }
            );
        }

        const displayBlocks = normalizeDisplayBlocks(question.displayBlocks);
        const displayBlockBytes = displayBlocks.reduce(
            (total, block) => total + Buffer.byteLength(block.data, "base64"),
            0
        );
        const shouldInlineDisplayBlocks =
            displayBlockBytes <= INLINE_DISPLAY_BLOCK_BYTE_LIMIT;

        return NextResponse.json({
            ok: true,
            status: question.status,
            question_id: question.questionNo,
            photo_count: question.photoCount,
            answer: safeText(question.answer) || "",
            display_text: safeText(question.displayText) || "",
            display_blocks: shouldInlineDisplayBlocks ? displayBlocks : [],
            display_block_count: displayBlocks.length,
            display_blocks_inline: shouldInlineDisplayBlocks,
            model: safeText(question.modelHint) || "",
            usage: {
                input_tokens: question.promptTokens || 0,
                output_tokens: question.completionTokens || 0,
                total_tokens: question.totalTokens || 0
            },
            completed_at: question.completedAt?.toISOString() || null,
            updated_at: question.updatedAt.toISOString()
        });
    } catch (error: unknown) {
        const message = safeText(
            error && typeof error === "object" && "message" in error
                ? (error as { message?: unknown }).message
                : "question_fetch_failed"
        );

        return NextResponse.json(
            {
                ok: false,
                error: message || "question_fetch_failed"
            },
            { status: 500 }
        );
    }
}
