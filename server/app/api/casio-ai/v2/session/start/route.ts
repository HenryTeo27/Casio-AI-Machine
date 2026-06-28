import { authenticateCasioRequest, safeText } from "@/app/utils/casioAi";
import prisma from "@/utils/prisma";
import { NextRequest, NextResponse } from "next/server";
import { casioSessionCutoffDate, getOrCreateActiveSession } from "../../_shared";

export async function POST(request: NextRequest) {
    try {
        const auth = authenticateCasioRequest(request);
        if (!auth.ok) {
            return NextResponse.json({ ok: false, error: auth.error }, { status: auth.status });
        }

        const body = await request.json().catch(() => ({}));
        const requestedSessionId = safeText(body?.session_id);
        const session = await getOrCreateActiveSession(auth.deviceId, requestedSessionId);
        const cutoff = casioSessionCutoffDate();
        const activeQuestionCount = await prisma.casioAiQuestion.count({
            where: {
                deviceId: auth.deviceId,
                sessionId: session.id,
                createdAt: { gte: cutoff }
            }
        });

        return NextResponse.json({
            ok: true,
            session_id: session.id,
            status: session.status,
            title: safeText(session.title),
            active_question_count: activeQuestionCount,
            server_time: new Date().toISOString()
        });
    } catch (error: unknown) {
        const message = safeText(
            error && typeof error === "object" && "message" in error
                ? (error as { message?: unknown }).message
                : "session_start_failed"
        );
        return NextResponse.json({ ok: false, error: message || "session_start_failed" }, { status: 500 });
    }
}
