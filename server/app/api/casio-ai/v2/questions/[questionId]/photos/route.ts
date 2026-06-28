import { authenticateCasioRequest, parseIntStrict, safeText } from "@/app/utils/casioAi";
import prisma from "@/utils/prisma";
import { put } from "@vercel/blob";
import { createHash, randomUUID } from "crypto";
import { NextRequest, NextResponse } from "next/server";
import sharp from "sharp";

type RouteContext = { params: Promise<{ questionId: string }> };

export async function POST(request: NextRequest, { params }: RouteContext) {
    try {
        const auth = authenticateCasioRequest(request);
        if (!auth.ok) {
            return NextResponse.json({ ok: false, error: auth.error }, { status: auth.status });
        }

        const { questionId } = await params;
        const photoIndex = parseIntStrict(new URL(request.url).searchParams.get("index"));
        if (photoIndex === null || photoIndex < 0) {
            return NextResponse.json({ ok: false, error: "invalid_photo_index" }, { status: 400 });
        }

        const question = await prisma.casioAiQuestion.findFirst({
            where: { id: questionId, deviceId: auth.deviceId },
            select: {
                id: true,
                questionNo: true,
                displayOrder: true,
                sessionId: true,
                status: true
            }
        });
        if (!question) {
            return NextResponse.json({ ok: false, error: "question_not_found" }, { status: 404 });
        }

        const uploadToken = safeText(request.headers.get("x-upload-token"));
        if (uploadToken) {
            const existing = await prisma.casioAiPhoto.findUnique({
                where: {
                    deviceId_uploadToken: {
                        deviceId: auth.deviceId,
                        uploadToken
                    }
                }
            });
            if (existing && existing.questionId === question.id && existing.photoIndex === photoIndex) {
                const count = await prisma.casioAiPhoto.count({ where: { questionId: question.id } });
                return NextResponse.json({
                    ok: true,
                    question_id: question.id,
                    photo_id: existing.id,
                    index: existing.photoIndex,
                    photo_count: count,
                    deduped: true
                });
            }
            if (existing) {
                return NextResponse.json({ ok: false, error: "upload_token_conflict" }, { status: 409 });
            }
        }

        const existingByIndex = await prisma.casioAiPhoto.findUnique({
            where: {
                questionId_photoIndex: {
                    questionId: question.id,
                    photoIndex
                }
            }
        });
        if (existingByIndex) {
            const count = await prisma.casioAiPhoto.count({ where: { questionId: question.id } });
            return NextResponse.json({
                ok: true,
                question_id: question.id,
                photo_id: existingByIndex.id,
                index: existingByIndex.photoIndex,
                photo_count: count,
                deduped: true
            });
        }

        if (!["DRAFT", "UPLOADING", "UPLOADED"].includes(question.status)) {
            return NextResponse.json({
                ok: false,
                error: "question_not_accepting_photos",
                question_id: question.id,
                status: question.status
            }, { status: 409 });
        }

        const bodyBuffer = Buffer.from(await request.arrayBuffer());
        if (!bodyBuffer.length) {
            return NextResponse.json({ ok: false, error: "empty_photo_body" }, { status: 400 });
        }

        const contentType = safeText(request.headers.get("content-type")) || "image/jpeg";
        if (!contentType.toLowerCase().includes("image/")) {
            return NextResponse.json({ ok: false, error: "invalid_content_type" }, { status: 400 });
        }

        // Device v2 camera is mounted 90 degrees counter-clockwise; normalize uploads server-side.
        const photoBuffer = await sharp(bodyBuffer)
            .rotate(90)
            .jpeg({ quality: 92, mozjpeg: true })
            .toBuffer();

        const now = new Date();
        const dateSegment = now.toISOString().slice(0, 10);
        const hash = createHash("sha256").update(photoBuffer).digest("hex");
        const fileName = `${question.displayOrder || question.questionNo}-${photoIndex}-${randomUUID()}.jpg`;
        const blobPath = [
            "casio-ai",
            "device",
            auth.deviceId,
            dateSegment,
            question.sessionId || "sessionless",
            question.id,
            fileName
        ].join("/");

        const uploadResult = await put(blobPath, photoBuffer, {
            access: "public",
            addRandomSuffix: false,
            contentType: "image/jpeg"
        });

        const photo = await prisma.casioAiPhoto.upsert({
            where: {
                questionId_photoIndex: {
                    questionId: question.id,
                    photoIndex
                }
            },
            create: {
                questionId: question.id,
                deviceId: auth.deviceId,
                questionNo: question.questionNo,
                photoIndex,
                uploadToken: uploadToken || null,
                blobUrl: uploadResult.url,
                blobPath,
                contentType: "image/jpeg",
                sizeBytes: photoBuffer.length,
                sha256: hash
            },
            update: {
                uploadToken: uploadToken || null,
                blobUrl: uploadResult.url,
                blobPath,
                contentType: "image/jpeg",
                sizeBytes: photoBuffer.length,
                sha256: hash
            }
        });

        const countedPhotos = await prisma.casioAiPhoto.count({
            where: { questionId: question.id }
        });
        await prisma.casioAiQuestion.updateMany({
            where: {
                id: question.id,
                status: { in: ["DRAFT", "UPLOADING", "UPLOADED"] }
            },
            data: {
                photoCount: countedPhotos,
                status: "UPLOADED",
                errorMessage: null
            }
        });

        return NextResponse.json({
            ok: true,
            question_id: question.id,
            photo_id: photo.id,
            index: photoIndex,
            photo_count: countedPhotos
        });
    } catch (error: unknown) {
        const message = safeText(
            error && typeof error === "object" && "message" in error
                ? (error as { message?: unknown }).message
                : "photo_upload_failed"
        );
        return NextResponse.json({ ok: false, error: message || "photo_upload_failed" }, { status: 500 });
    }
}
