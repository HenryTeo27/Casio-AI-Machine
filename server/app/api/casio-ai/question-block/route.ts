import {
    authenticateCasioRequest,
    parseIntStrict,
    safeText
} from "@/app/utils/casioAi";
import prisma from "@/utils/prisma";
import { NextRequest, NextResponse } from "next/server";

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

export async function GET(request: NextRequest) {
    try {
        const auth = authenticateCasioRequest(request);
        if (!auth.ok) {
            return NextResponse.json(
                { ok: false, error: auth.error },
                { status: auth.status }
            );
        }

        const params = new URL(request.url).searchParams;
        const questionNo = parseIntStrict(params.get("question_id"));
        const index = parseIntStrict(params.get("index"));

        if (questionNo === null || questionNo < 0) {
            return NextResponse.json(
                { ok: false, error: "invalid_question_id" },
                { status: 400 }
            );
        }
        if (index === null || index < 0) {
            return NextResponse.json(
                { ok: false, error: "invalid_block_index" },
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
                questionNo: true,
                status: true,
                displayBlocks: true,
                errorMessage: true,
                updatedAt: true
            }
        });

        if (!question) {
            return NextResponse.json(
                {
                    ok: false,
                    error: "question_not_found",
                    status: "NOT_FOUND"
                },
                { status: 404 }
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
                    updated_at: question.updatedAt.toISOString()
                },
                { status: question.status === "FAILED" ? 500 : 202 }
            );
        }

        const blocks = normalizeDisplayBlocks(question.displayBlocks);
        const block = blocks[index];
        if (!block) {
            return NextResponse.json(
                {
                    ok: false,
                    error: "block_not_found",
                    status: question.status,
                    index,
                    count: blocks.length
                },
                { status: 404 }
            );
        }

        return NextResponse.json({
            ok: true,
            status: question.status,
            question_id: question.questionNo,
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

        return NextResponse.json(
            {
                ok: false,
                error: message || "question_block_fetch_failed"
            },
            { status: 500 }
        );
    }
}
