import prisma from "@/utils/prisma";
import {
    authenticateCasioRequest,
    resolvePhotoIndex,
    resolveQuestionNo,
    safeText
} from "@/app/utils/casioAi";
import { put } from "@vercel/blob";
import { createHash, randomUUID } from "crypto";
import { NextRequest, NextResponse } from "next/server";

export async function POST(request: NextRequest) {
    try {
        const auth = authenticateCasioRequest(request);
        if (!auth.ok) {
            return NextResponse.json({ ok: false, error: auth.error }, { status: auth.status });
        }

        const questionNo = resolveQuestionNo(request);
        const photoIndex = resolvePhotoIndex(request);

        if (questionNo === null || questionNo < 0) {
            return NextResponse.json(
                { ok: false, error: "invalid_question_id" },
                { status: 400 }
            );
        }

        if (photoIndex === null || photoIndex < 0) {
            return NextResponse.json(
                { ok: false, error: "invalid_photo_index" },
                { status: 400 }
            );
        }

        const bodyBuffer = Buffer.from(await request.arrayBuffer());
        if (!bodyBuffer.length) {
            return NextResponse.json(
                { ok: false, error: "empty_photo_body" },
                { status: 400 }
            );
        }

        const contentType = safeText(request.headers.get("content-type")) || "image/jpeg";
        if (!contentType.toLowerCase().includes("image/")) {
            return NextResponse.json(
                { ok: false, error: "invalid_content_type" },
                { status: 400 }
            );
        }

        const now = new Date();
        const dateSegment = now.toISOString().slice(0, 10);
        const hash = createHash("sha256").update(bodyBuffer).digest("hex");
        const fileName = `${questionNo}-${photoIndex}-${randomUUID()}.jpg`;
        const blobPath = [
            "casio-ai",
            "device",
            auth.deviceId,
            dateSegment,
            `q${questionNo}`,
            fileName
        ].join("/");

        const uploadResult = await put(blobPath, bodyBuffer, {
            access: "public",
            addRandomSuffix: false,
            contentType: "image/jpeg"
        });

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
                status: "PENDING"
            },
            update: {
                status: "PENDING",
                completedAt: null,
                errorMessage: null
            }
        });

        const photo = await prisma.casioAiPhoto.upsert({
            where: {
                deviceId_questionNo_photoIndex: {
                    deviceId: auth.deviceId,
                    questionNo,
                    photoIndex
                }
            },
            create: {
                questionId: question.id,
                deviceId: auth.deviceId,
                questionNo,
                photoIndex,
                blobUrl: uploadResult.url,
                blobPath,
                contentType: "image/jpeg",
                sizeBytes: bodyBuffer.length,
                sha256: hash
            },
            update: {
                questionId: question.id,
                blobUrl: uploadResult.url,
                blobPath,
                contentType: "image/jpeg",
                sizeBytes: bodyBuffer.length,
                sha256: hash
            }
        });

        const countedPhotos = await prisma.casioAiPhoto.count({
            where: {
                questionId: question.id
            }
        });

        await prisma.casioAiQuestion.update({
            where: { id: question.id },
            data: {
                photoCount: countedPhotos
            }
        });

        return NextResponse.json({
            ok: true,
            photo_id: photo.id,
            question_id: questionNo,
            index: photoIndex
        });
    } catch (error: unknown) {
        const message = safeText(
            error && typeof error === "object" && "message" in error
                ? (error as { message?: unknown }).message
                : "upload_photo_failed"
        );

        return NextResponse.json(
            {
                ok: false,
                error: message || "upload_photo_failed"
            },
            { status: 500 }
        );
    }
}
