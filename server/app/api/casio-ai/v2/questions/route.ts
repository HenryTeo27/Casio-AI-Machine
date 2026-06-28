import { authenticateCasioRequest, safeText } from "@/app/utils/casioAi";
import prisma from "@/utils/prisma";
import { NextRequest, NextResponse } from "next/server";
import { allocateQuestionNumbers, getOrCreateActiveSession } from "../_shared";

export async function POST(request: NextRequest) {
    try {
        const auth = authenticateCasioRequest(request);
        if (!auth.ok) {
            return NextResponse.json({ ok: false, error: auth.error }, { status: auth.status });
        }

        const body = await request.json().catch(() => ({}));
        const session = await getOrCreateActiveSession(auth.deviceId, safeText(body?.session_id));
        const mode = safeText(body?.mode) || "oled-v2";
        const contextTail = safeText(body?.context_tail);
        const createToken = safeText(body?.client_request_id) || safeText(request.headers.get("x-create-token"));

        if (createToken) {
            const existing = await prisma.casioAiQuestion.findUnique({
                where: {
                    deviceId_createToken: {
                        deviceId: auth.deviceId,
                        createToken
                    }
                }
            });

            if (existing) {
                return NextResponse.json({
                    ok: true,
                    session_id: existing.sessionId || session.id,
                    question_id: existing.id,
                    display_order: existing.displayOrder || existing.questionNo,
                    legacy_question_no: existing.questionNo,
                    status: existing.status,
                    deduped: true
                });
            }
        }

        const { questionNo, displayOrder } = await allocateQuestionNumbers(auth.deviceId, session.id);
        const question = await prisma.casioAiQuestion.create({
            data: {
                deviceId: auth.deviceId,
                questionNo,
                legacyQuestionNo: questionNo,
                sessionId: session.id,
                displayOrder,
                mode,
                contextTail,
                createToken: createToken || null,
                status: "DRAFT"
            }
        });
        await prisma.casioAiSession.update({
            where: { id: session.id },
            data: { status: "ACTIVE" }
        });

        return NextResponse.json({
            ok: true,
            session_id: session.id,
            question_id: question.id,
            display_order: question.displayOrder,
            legacy_question_no: question.questionNo,
            status: question.status
        });
    } catch (error: unknown) {
        const message = safeText(
            error && typeof error === "object" && "message" in error
                ? (error as { message?: unknown }).message
                : "question_create_failed"
        );
        return NextResponse.json({ ok: false, error: message || "question_create_failed" }, { status: 500 });
    }
}
