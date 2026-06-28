import { authenticateCasioRequest, safeText } from "@/app/utils/casioAi";
import prisma from "@/utils/prisma";
import { randomUUID } from "crypto";
import { after, NextRequest, NextResponse } from "next/server";
import { runAnswerJobV2 } from "../../../_shared";

type RouteContext = { params: Promise<{ questionId: string }> };

export const maxDuration = 300;

export async function POST(request: NextRequest, { params }: RouteContext) {
    try {
        const auth = authenticateCasioRequest(request);
        if (!auth.ok) {
            return NextResponse.json({ ok: false, error: auth.error }, { status: auth.status });
        }

        const { questionId } = await params;
        const body = await request.json().catch(() => ({}));
        const mode = safeText(body?.mode) || "oled-v2";
        const contextTail = safeText(body?.context_tail);
        const idempotencyKey = safeText(request.headers.get("x-solve-token"));

        const question = await prisma.casioAiQuestion.findFirst({
            where: { id: questionId, deviceId: auth.deviceId },
            include: {
                photos: {
                    orderBy: { photoIndex: "asc" },
                    select: { id: true, blobUrl: true }
                }
            }
        });
        if (!question) {
            return NextResponse.json({ ok: false, error: "question_not_found" }, { status: 404 });
        }
        if (!question.photos.length) {
            return NextResponse.json({ ok: false, error: "photos_required" }, { status: 400 });
        }
        if (["PROCESSING", "ANSWER_READY", "RENDERING"].includes(question.status)) {
            return NextResponse.json({
                ok: true,
                question_id: question.id,
                status: question.status,
                photo_count: question.photos.length,
                poll_after_ms: 3000
            }, { status: 202 });
        }
        if (question.status === "SUCCEEDED") {
            return NextResponse.json({
                ok: true,
                question_id: question.id,
                status: question.status,
                photo_count: question.photos.length,
                poll_after_ms: 0
            });
        }

        const jobToken = idempotencyKey || `casio_v2_solve:${randomUUID()}`;
        await prisma.casioAiQuestion.update({
            where: { id: question.id },
            data: {
                status: "PROCESSING",
                mode,
                solveToken: jobToken,
                contextTail,
                photoCount: question.photos.length,
                answer: null,
                displayText: null,
                displayBlocks: [],
                errorMessage: null,
                completedAt: null
            }
        });

        after(() => runAnswerJobV2({
            questionId: question.id,
            deviceId: auth.deviceId,
            jobToken,
            mode,
            imageUrls: question.photos.map((photo) => photo.blobUrl),
            contextTail
        }));

        return NextResponse.json({
            ok: true,
            question_id: question.id,
            status: "PROCESSING",
            photo_count: question.photos.length,
            poll_after_ms: 3000
        }, { status: 202 });
    } catch (error: unknown) {
        const message = safeText(
            error && typeof error === "object" && "message" in error
                ? (error as { message?: unknown }).message
                : "solve_start_failed"
        );
        return NextResponse.json({ ok: false, error: message || "solve_start_failed" }, { status: 500 });
    }
}
