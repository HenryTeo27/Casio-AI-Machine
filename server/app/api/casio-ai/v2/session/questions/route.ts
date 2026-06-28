import { authenticateCasioRequest, parseIntStrict, safeText } from "@/app/utils/casioAi";
import prisma from "@/utils/prisma";
import { after, NextRequest, NextResponse } from "next/server";
import {
    casioSessionCutoffDate,
    getOrCreateActiveSession,
    recoverStaleUploadQuestions,
    runAnswerJobV2,
    summaryFromQuestion
} from "../../_shared";

export async function GET(request: NextRequest) {
    try {
        const auth = authenticateCasioRequest(request);
        if (!auth.ok) {
            return NextResponse.json({ ok: false, error: auth.error }, { status: auth.status });
        }

        const params = new URL(request.url).searchParams;
        const sessionId = safeText(params.get("session_id"));
        const cursor = parseIntStrict(params.get("cursor"));
        const limitRaw = parseIntStrict(params.get("limit"));
        const limit = Math.max(1, Math.min(limitRaw || 20, 40));
        const session = await getOrCreateActiveSession(auth.deviceId, sessionId);
        const cutoff = casioSessionCutoffDate();
        const recoveryJobs = await recoverStaleUploadQuestions({
            deviceId: auth.deviceId,
            sessionId: session.id
        });
        for (const job of recoveryJobs) {
            after(() => runAnswerJobV2(job));
        }

        const questions = await prisma.casioAiQuestion.findMany({
            where: {
                deviceId: auth.deviceId,
                sessionId: session.id,
                createdAt: { gte: cutoff },
                ...(cursor !== null ? { displayOrder: { gt: cursor } } : {})
            },
            orderBy: { displayOrder: "asc" },
            take: limit,
            select: {
                id: true,
                displayOrder: true,
                questionNo: true,
                status: true,
                photoCount: true,
                answer: true,
                displayText: true,
                errorMessage: true,
                updatedAt: true
            }
        });

        const items = questions.map((question) => ({
            question_id: question.id,
            display_order: question.displayOrder || question.questionNo,
            legacy_question_no: question.questionNo,
            status: question.status,
            photo_count: question.photoCount,
            summary: summaryFromQuestion(question),
            updated_at: question.updatedAt.toISOString()
        }));

        const nextCursor = questions.length === limit
            ? questions[questions.length - 1]?.displayOrder || null
            : null;

        return NextResponse.json({
            ok: true,
            session_id: session.id,
            items,
            next_cursor: nextCursor
        });
    } catch (error: unknown) {
        const message = safeText(
            error && typeof error === "object" && "message" in error
                ? (error as { message?: unknown }).message
                : "session_questions_failed"
        );
        return NextResponse.json({ ok: false, error: message || "session_questions_failed" }, { status: 500 });
    }
}
