/* eslint-disable no-console */
import prisma from "@/utils/prisma";
import {
    safeText,
    solveCasioAnswerWithOpenAi,
    renderCasioAnswerForOledWithOpenAi
} from "@/app/utils/casioAi";
import { Prisma } from "@prisma/client";
import { randomUUID } from "crypto";

export const CASIO_V2_LOG_PREFIX = "[CASIO_AI_V2]";
export const CASIO_SESSION_IDLE_MS = 2 * 60 * 60 * 1000;
export const CASIO_UPLOAD_RECOVERY_MS = 5 * 60 * 1000;

export type CasioDisplayBlockPayload = {
    type: "bitmap";
    kind: "text" | "formula";
    width: number;
    height: number;
    format: "1bit_xbm";
    data: string;
};

export function normalizeDisplayBlocks(value: unknown): CasioDisplayBlockPayload[] {
    if (!Array.isArray(value)) return [];
    return value.filter((item): item is CasioDisplayBlockPayload => {
        if (!item || typeof item !== "object") return false;
        const block = item as Partial<CasioDisplayBlockPayload>;
        return (
            block.type === "bitmap" &&
            (block.kind === "text" || block.kind === "formula") &&
            typeof block.width === "number" &&
            typeof block.height === "number" &&
            (block.height === 16 || block.height === 32 || block.height === 64) &&
            block.format === "1bit_xbm" &&
            typeof block.data === "string"
        );
    });
}

export function summaryFromQuestion(question: {
    displayText?: string | null;
    answer?: string | null;
    status: string;
    errorMessage?: string | null;
}) {
    const source =
        safeText(question.displayText) ||
        safeText(question.answer) ||
        safeText(question.errorMessage) ||
        question.status;
    return source.replace(/\s+/g, " ").slice(0, 32);
}

export function casioSessionCutoffDate(now = new Date()) {
    return new Date(now.getTime() - CASIO_SESSION_IDLE_MS);
}

export function casioUploadRecoveryCutoffDate(now = new Date()) {
    return new Date(now.getTime() - CASIO_UPLOAD_RECOVERY_MS);
}

export type CasioAnswerJobArgs = Parameters<typeof runAnswerJobV2>[0];

export async function recoverStaleUploadQuestions(args: {
    deviceId: string;
    sessionId?: string;
    questionId?: string;
}) {
    const cutoff = casioUploadRecoveryCutoffDate();
    const staleQuestions = await prisma.casioAiQuestion.findMany({
        where: {
            deviceId: args.deviceId,
            ...(args.sessionId ? { sessionId: args.sessionId } : {}),
            ...(args.questionId ? { id: args.questionId } : {}),
            status: { in: ["DRAFT", "UPLOADING", "UPLOADED"] },
            updatedAt: { lt: cutoff }
        },
        include: {
            photos: {
                orderBy: { photoIndex: "asc" },
                select: { blobUrl: true }
            }
        },
        take: 20
    });

    const jobs = await Promise.all(staleQuestions.map(async (question) => {
        if (!question.photos.length) {
            const failed = await prisma.casioAiQuestion.updateMany({
                where: {
                    id: question.id,
                    deviceId: args.deviceId,
                    status: { in: ["DRAFT", "UPLOADING", "UPLOADED"] }
                },
                data: {
                    status: "FAILED",
                    errorMessage: "upload_timeout_no_photos",
                    completedAt: new Date()
                }
            });
            if (failed.count > 0) {
                console.warn(`${CASIO_V2_LOG_PREFIX} stale_upload_failed_no_photos`, {
                    questionId: question.id,
                    displayOrder: question.displayOrder,
                    updatedAt: question.updatedAt.toISOString()
                });
            }
            return null;
        }

        const jobToken = `casio_v2_upload_recover:${randomUUID()}`;
        const recovered = await prisma.casioAiQuestion.updateMany({
            where: {
                id: question.id,
                deviceId: args.deviceId,
                status: { in: ["DRAFT", "UPLOADING", "UPLOADED"] }
            },
            data: {
                status: "PROCESSING",
                mode: safeText(question.mode) || "oled-v2",
                solveToken: jobToken,
                photoCount: question.photos.length,
                errorMessage: null,
                completedAt: null
            }
        });

        if (recovered.count > 0) {
            console.warn(`${CASIO_V2_LOG_PREFIX} stale_upload_recovered`, {
                questionId: question.id,
                displayOrder: question.displayOrder,
                photoCount: question.photos.length,
                updatedAt: question.updatedAt.toISOString()
            });
            return {
                questionId: question.id,
                deviceId: args.deviceId,
                jobToken,
                mode: safeText(question.mode) || "oled-v2",
                imageUrls: question.photos.map((photo) => photo.blobUrl),
                contextTail: safeText(question.contextTail)
            };
        }
        return null;
    }));

    return jobs.filter((job): job is CasioAnswerJobArgs => Boolean(job));
}

async function sessionHasRecentQuestion(deviceId: string, sessionId: string, cutoff: Date) {
    const count = await prisma.casioAiQuestion.count({
        where: {
            deviceId,
            sessionId,
            createdAt: { gte: cutoff }
        }
    });
    return count > 0;
}

async function isSessionFresh(session: { id: string; deviceId: string; status: string; createdAt: Date }, cutoff: Date) {
    if (session.status !== "ACTIVE") return false;
    if (session.createdAt >= cutoff) return true;
    return sessionHasRecentQuestion(session.deviceId, session.id, cutoff);
}

async function expireSession(sessionId: string, deviceId: string) {
    await prisma.casioAiSession.updateMany({
        where: { id: sessionId, deviceId, status: "ACTIVE" },
        data: { status: "EXPIRED" }
    });
}

export async function getOrCreateActiveSession(deviceId: string, requestedSessionId = "") {
    const cutoff = casioSessionCutoffDate();

    if (requestedSessionId) {
        const existing = await prisma.casioAiSession.findFirst({
            where: { id: requestedSessionId, deviceId }
        });
        if (existing && await isSessionFresh(existing, cutoff)) return existing;
        if (existing) await expireSession(existing.id, deviceId);
    }

    const activeSessions = await prisma.casioAiSession.findMany({
        where: { deviceId, status: "ACTIVE" },
        orderBy: { updatedAt: "desc" },
        take: 20
    });
    const checkedSessions = await Promise.all(
        activeSessions.map(async (session) => ({
            session,
            fresh: await isSessionFresh(session, cutoff)
        }))
    );
    const freshSession = checkedSessions.find((item) => item.fresh)?.session;
    if (freshSession) return freshSession;
    await Promise.all(
        checkedSessions.map((item) => expireSession(item.session.id, deviceId))
    );

    const recentQuestion = await prisma.casioAiQuestion.findFirst({
        where: {
            deviceId,
            createdAt: { gte: cutoff },
            session: { status: "ACTIVE" }
        },
        orderBy: { createdAt: "desc" },
        include: { session: true }
    });
    if (recentQuestion?.session) return recentQuestion.session;

    const dateTitle = new Date().toISOString().slice(0, 10);
    return prisma.casioAiSession.create({
        data: {
            deviceId,
            title: dateTitle,
            status: "ACTIVE"
        }
    });
}

export async function allocateQuestionNumbers(deviceId: string, sessionId: string) {
    const [deviceMax, sessionMax] = await Promise.all([
        prisma.casioAiQuestion.aggregate({
            where: { deviceId },
            _max: { questionNo: true }
        }),
        prisma.casioAiQuestion.aggregate({
            where: { deviceId, sessionId },
            _max: { displayOrder: true }
        })
    ]);

    return {
        questionNo: (deviceMax._max.questionNo || 0) + 1,
        displayOrder: (sessionMax._max.displayOrder || 0) + 1
    };
}

export function buildQuestionResponse(question: {
    id: string;
    displayOrder: number;
    questionNo: number;
    status: string;
    photoCount: number;
    answer?: string | null;
    displayText?: string | null;
    displayBlocks?: unknown;
    errorMessage?: string | null;
    promptTokens?: number | null;
    completionTokens?: number | null;
    totalTokens?: number | null;
    completedAt?: Date | null;
    updatedAt: Date;
}) {
    const displayBlocks = normalizeDisplayBlocks(question.displayBlocks);
    const displayBlockBytes = displayBlocks.reduce(
        (total, block) => total + Buffer.byteLength(block.data, "base64"),
        0
    );
    const shouldInlineDisplayBlocks =
        displayBlocks.length > 0 &&
        displayBlocks.length <= 2 &&
        displayBlockBytes <= 2_500;

    return {
        ok: question.status === "SUCCEEDED",
        question_id: question.id,
        display_order: question.displayOrder || question.questionNo,
        legacy_question_no: question.questionNo,
        status: question.status,
        photo_count: question.photoCount,
        summary: summaryFromQuestion(question),
        answer: safeText(question.answer) || "",
        display_text: safeText(question.displayText) || "",
        display_blocks: shouldInlineDisplayBlocks ? displayBlocks : [],
        display_block_count: displayBlocks.length,
        display_blocks_inline: shouldInlineDisplayBlocks,
        error: question.status === "FAILED" ? safeText(question.errorMessage) || "question_failed" : "",
        usage: {
            input_tokens: question.promptTokens || 0,
            output_tokens: question.completionTokens || 0,
            total_tokens: question.totalTokens || 0
        },
        completed_at: question.completedAt?.toISOString() || null,
        updated_at: question.updatedAt.toISOString()
    };
}

export async function runAnswerJobV2(args: {
    questionId: string;
    deviceId: string;
    jobToken: string;
    mode: string;
    imageUrls: string[];
    contextTail: string;
}) {
    const startedAt = Date.now();
    try {
        const answerResult = await solveCasioAnswerWithOpenAi({
            imageUrls: args.imageUrls,
            contextTail: args.contextTail
        });

        const updated = await prisma.casioAiQuestion.updateMany({
            where: {
                id: args.questionId,
                deviceId: args.deviceId,
                status: "PROCESSING",
                solveToken: args.jobToken
            },
            data: {
                mode: args.mode,
                modelHint: answerResult.model,
                photoCount: args.imageUrls.length,
                status: "ANSWER_READY",
                answer: answerResult.answer,
                displayText: answerResult.displayText,
                displayBlocks: [],
                promptTokens: answerResult.usage.promptTokens,
                completionTokens: answerResult.usage.completionTokens,
                totalTokens: answerResult.usage.totalTokens,
                errorMessage: null,
                completedAt: null
            }
        });

        if (updated.count === 0) {
            console.warn(`${CASIO_V2_LOG_PREFIX} answer_job_stale_ignored`, {
                questionId: args.questionId,
                totalMs: Date.now() - startedAt
            });
            return;
        }

        console.log(`${CASIO_V2_LOG_PREFIX} answer_job_done`, {
            questionId: args.questionId,
            totalMs: Date.now() - startedAt,
            answerChars: answerResult.answer.length,
            tokens: answerResult.usage
        });

        const renderToken = `casio_v2_render:${randomUUID()}`;
        const renderTransition = await prisma.casioAiQuestion.updateMany({
            where: {
                id: args.questionId,
                deviceId: args.deviceId,
                status: "ANSWER_READY",
                solveToken: args.jobToken
            },
            data: {
                status: "RENDERING",
                solveToken: renderToken
            }
        });

        if (renderTransition.count > 0) {
            console.log(`${CASIO_V2_LOG_PREFIX} render_job_chained`, {
                questionId: args.questionId,
                elapsedMs: Date.now() - startedAt
            });
            await runRenderJobV2({
                questionId: args.questionId,
                deviceId: args.deviceId,
                jobToken: renderToken,
                answer: answerResult.answer,
                displayText: answerResult.displayText,
                model: answerResult.model,
                promptTokens: answerResult.usage.promptTokens,
                completionTokens: answerResult.usage.completionTokens,
                totalTokens: answerResult.usage.totalTokens
            });
        }
    } catch (error: unknown) {
        const message = safeText(
            error && typeof error === "object" && "message" in error
                ? (error as { message?: unknown }).message
                : "casio_ai_answer_failed"
        ) || "casio_ai_answer_failed";

        console.error(`${CASIO_V2_LOG_PREFIX} answer_job_failed`, {
            questionId: args.questionId,
            totalMs: Date.now() - startedAt,
            message
        });

        await prisma.casioAiQuestion.updateMany({
            where: {
                id: args.questionId,
                deviceId: args.deviceId,
                status: "PROCESSING",
                solveToken: args.jobToken
            },
            data: {
                status: "FAILED",
                errorMessage: message,
                completedAt: new Date()
            }
        });
    }
}

export async function runRenderJobV2(args: {
    questionId: string;
    deviceId: string;
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
            displayText,
            displayBlocks,
            usage
        } = await renderCasioAnswerForOledWithOpenAi({
            answer: args.answer,
            displayText: args.displayText,
            model: args.model
        });

        const updated = await prisma.casioAiQuestion.updateMany({
            where: {
                id: args.questionId,
                deviceId: args.deviceId,
                status: "RENDERING",
                solveToken: args.jobToken
            },
            data: {
                status: "SUCCEEDED",
                modelHint: args.model,
                displayText,
                displayBlocks: displayBlocks as unknown as Prisma.InputJsonValue,
                promptTokens: args.promptTokens + usage.promptTokens,
                completionTokens: args.completionTokens + usage.completionTokens,
                totalTokens: args.totalTokens + usage.totalTokens,
                errorMessage: null,
                completedAt: new Date()
            }
        });

        if (updated.count === 0) {
            console.warn(`${CASIO_V2_LOG_PREFIX} render_job_stale_ignored`, {
                questionId: args.questionId,
                totalMs: Date.now() - startedAt
            });
            return;
        }

        console.log(`${CASIO_V2_LOG_PREFIX} render_job_done`, {
            questionId: args.questionId,
            totalMs: Date.now() - startedAt,
            displayBlocks: displayBlocks.length,
            tokens: usage
        });
    } catch (error: unknown) {
        const message = safeText(
            error && typeof error === "object" && "message" in error
                ? (error as { message?: unknown }).message
                : "casio_ai_render_failed"
        ) || "casio_ai_render_failed";

        console.error(`${CASIO_V2_LOG_PREFIX} render_job_failed`, {
            questionId: args.questionId,
            totalMs: Date.now() - startedAt,
            message
        });

        await prisma.casioAiQuestion.updateMany({
            where: {
                id: args.questionId,
                deviceId: args.deviceId,
                status: "RENDERING",
                solveToken: args.jobToken
            },
            data: {
                status: "FAILED",
                errorMessage: message,
                completedAt: new Date()
            }
        });
    }
}
