/* eslint-disable no-console */
import prisma from "@/utils/prisma";
import {
    authenticateCasioRequest,
    resolveQuestionNo,
    safeText,
    solveCasioAnswerWithOpenAi
} from "@/app/utils/casioAi";
import { randomUUID } from "crypto";
import { after, NextRequest, NextResponse } from "next/server";

export const maxDuration = 300;

const LOG_PREFIX = "[CASIO_AI_SOLVE]";
const SOLVE_JOB_PREFIX = "casio_solve_job:";

type SolveBody = {
    device_id?: unknown;
    question_id?: unknown;
    photo_count?: unknown;
    mode?: unknown;
    photo_ids?: unknown;
    context_tail?: unknown;
};

function normalizePhotoIds(raw: unknown): string[] {
    if (!Array.isArray(raw)) return [];
    const cleaned = raw
        .map((item) => safeText(item))
        .filter(Boolean);
    return Array.from(new Set(cleaned));
}

async function runAnswerJob(args: {
    questionId: string;
    deviceId: string;
    questionNo: number;
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
                status: "PROCESSING",
                modelHint: args.jobToken
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
            console.warn(`${LOG_PREFIX} answer_job_stale_ignored`, {
                totalMs: Date.now() - startedAt,
                deviceId: args.deviceId,
                questionNo: args.questionNo
            });
            return;
        }

        console.log(`${LOG_PREFIX} answer_job_done`, {
            totalMs: Date.now() - startedAt,
            deviceId: args.deviceId,
            questionNo: args.questionNo,
            answerChars: answerResult.answer.length,
            tokens: answerResult.usage
        });
    } catch (error: unknown) {
        const message = safeText(
            error && typeof error === "object" && "message" in error
                ? (error as { message?: unknown }).message
                : "casio_ai_answer_failed"
        ) || "casio_ai_answer_failed";
        console.error(`${LOG_PREFIX} answer_job_failed`, {
            totalMs: Date.now() - startedAt,
            deviceId: args.deviceId,
            questionNo: args.questionNo,
            message
        });

        const updated = await prisma.casioAiQuestion.updateMany({
            where: {
                id: args.questionId,
                status: "PROCESSING",
                modelHint: args.jobToken
            },
            data: {
                status: "FAILED",
                errorMessage: message,
                completedAt: new Date()
            }
        });
        if (updated.count === 0) {
            console.warn(`${LOG_PREFIX} answer_job_failed_stale_ignored`, {
                totalMs: Date.now() - startedAt,
                deviceId: args.deviceId,
                questionNo: args.questionNo,
                message
            });
        }
    }
}

export async function POST(request: NextRequest) {
    const startedAt = Date.now();
    let questionNoForError: number | null = null;
    let deviceIdForError = "";
    let questionIdForError = "";
    let jobTokenForError = "";

    try {
        const auth = authenticateCasioRequest(request);
        if (!auth.ok) {
            return NextResponse.json({ ok: false, error: auth.error }, { status: auth.status });
        }
        deviceIdForError = auth.deviceId;

        const body = (await request.json()) as SolveBody;

        const bodyDeviceId = safeText(body?.device_id);
        if (bodyDeviceId && bodyDeviceId !== auth.deviceId) {
            return NextResponse.json(
                { ok: false, error: "device_id_mismatch" },
                { status: 400 }
            );
        }

        const questionNo = resolveQuestionNo(request, body?.question_id);
        questionNoForError = questionNo;

        if (questionNo === null || questionNo < 0) {
            return NextResponse.json(
                { ok: false, error: "invalid_question_id" },
                { status: 400 }
            );
        }

        const photoIds = normalizePhotoIds(body?.photo_ids);
        if (!photoIds.length) {
            return NextResponse.json(
                { ok: false, error: "photo_ids_required" },
                { status: 400 }
            );
        }

        const mode = safeText(body?.mode) || "gpt-test";
        const jobToken = `${SOLVE_JOB_PREFIX}${randomUUID()}`;
        console.log(`${LOG_PREFIX} start`, {
            deviceId: auth.deviceId,
            questionNo,
            mode,
            photoIds: photoIds.length,
            contextTailChars: safeText(body?.context_tail).length
        });

        const dbUpsertStartedAt = Date.now();
        const question = await prisma.casioAiQuestion.upsert({
            where: {
                deviceId_questionNo: {
                    deviceId: auth.deviceId,
                    questionNo
                }
            },
            create: {
                deviceId: auth.deviceId,
                questionNo,
                mode,
                modelHint: jobToken,
                photoCount: photoIds.length,
                status: "PROCESSING",
                contextTail: safeText(body?.context_tail)
            },
            update: {
                mode,
                modelHint: jobToken,
                photoCount: photoIds.length,
                status: "PROCESSING",
                contextTail: safeText(body?.context_tail),
                answer: null,
                displayText: null,
                displayBlocks: [],
                promptTokens: null,
                completionTokens: null,
                totalTokens: null,
                errorMessage: null,
                completedAt: null
            }
        });
        questionIdForError = question.id;
        jobTokenForError = jobToken;
        console.log(`${LOG_PREFIX} db_upsert_ms`, Date.now() - dbUpsertStartedAt);

        const photoLoadStartedAt = Date.now();
        const photos = await prisma.casioAiPhoto.findMany({
            where: {
                id: { in: photoIds },
                deviceId: auth.deviceId,
                questionNo
            },
            select: {
                id: true,
                blobUrl: true
            }
        });
        console.log(`${LOG_PREFIX} photo_load`, {
            ms: Date.now() - photoLoadStartedAt,
            found: photos.length,
            expected: photoIds.length
        });

        if (photos.length !== photoIds.length) {
            await prisma.casioAiQuestion.updateMany({
                where: {
                    id: question.id,
                    status: "PROCESSING",
                    modelHint: jobToken
                },
                data: {
                    status: "FAILED",
                    errorMessage: "photo_ids_not_found",
                    completedAt: new Date()
                }
            });
            return NextResponse.json(
                { ok: false, error: "photo_ids_not_found" },
                { status: 400 }
            );
        }

        const byId = new Map(photos.map((photo) => [photo.id, photo]));
        const orderedUrls = photoIds
            .map((photoId) => byId.get(photoId)?.blobUrl || "")
            .filter(Boolean);

        after(() =>
            runAnswerJob({
                questionId: question.id,
                deviceId: auth.deviceId,
                questionNo,
                jobToken,
                mode,
                imageUrls: orderedUrls,
                contextTail: safeText(body?.context_tail)
            })
        );

        const responsePayload = {
            ok: false,
            error: "question_not_ready",
            status: "PROCESSING",
            question_id: questionNo,
            photo_count: orderedUrls.length,
            display_blocks: [],
            display_block_count: 0,
            display_blocks_inline: false
        };
        console.log(`${LOG_PREFIX} response_ready`, {
            totalMs: Date.now() - startedAt,
            async: true,
            responseBytes: Buffer.byteLength(JSON.stringify(responsePayload), "utf8")
        });

        return NextResponse.json(responsePayload, { status: 202 });
    } catch (error: unknown) {
        const status =
            error && typeof error === "object" && "status" in error
                ? Number((error as { status?: unknown }).status) || 500
                : 500;
        const message = safeText(
            error && typeof error === "object" && "message" in error
                ? (error as { message?: unknown }).message
                : "casio_ai_solve_failed"
        ) || "casio_ai_solve_failed";
        console.error(`${LOG_PREFIX} failed`, {
            totalMs: Date.now() - startedAt,
            deviceId: deviceIdForError,
            questionNo: questionNoForError,
            status,
            message
        });

        if (questionIdForError && jobTokenForError) {
            try {
                await prisma.casioAiQuestion.updateMany({
                    where: {
                        id: questionIdForError,
                        status: "PROCESSING",
                        modelHint: jobTokenForError
                    },
                    data: {
                        status: "FAILED",
                        errorMessage: message,
                        completedAt: new Date()
                    }
                });
            } catch {
                // ignore secondary update failure
            }
        }

        return NextResponse.json(
            { ok: false, error: message },
            { status: status >= 400 ? status : 500 }
        );
    }
}
