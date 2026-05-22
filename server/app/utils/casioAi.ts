/* eslint-disable no-console, no-await-in-loop, no-continue, @typescript-eslint/no-unused-vars */
import {
    CasioDisplayBlock,
    buildDisplayBlocksFromOledPages,
    buildDisplayBlocksFromModel,
    buildDisplayBlocksFromOledLayout,
    getDisplayTextFromOledLayout
} from "@/app/utils/casioDisplay";
import { NextRequest } from "next/server";

const FALLBACK_DEVICE_ID = // CASIO_AI_MACHINE_001 from .env;
const FALLBACK_DEVICE_API_KEY = // DEVICE_API_KEY from .env;
const LOG_PREFIX = "[CASIO_AI]";
const DEFAULT_SOLVE_PROMPT_ID = // open ai api platform chat 开第一轮ai解题api ;
const DEFAULT_SOLVE_PROMPT_VERSION = // open ai api platform chat 开第一轮ai version;
const DEFAULT_LAYOUT_PROMPT_ID = // open ai api platform chat 开第2轮ai把第一轮解的题拿去切block api;
const DEFAULT_LAYOUT_PROMPT_VERSION = // open ai api platform chat 开第2轮ai把第一轮解的题拿去切block version;

const DISPLAY_LINE_UNITS = 40; // About 20 CJK chars or 40 narrow chars.
const MAX_CONTEXT_TAIL_CHARS = 3000;
const MAX_DISPLAY_CHARS = 2200;
const OLED_BITMAP_WIDTH = 128;
const OLED_BITMAP_HEIGHT = 32;
const OLED_BITMAP_BYTES = (OLED_BITMAP_WIDTH / 8) * OLED_BITMAP_HEIGHT;
const OLED_MAX_DIRECT_BLOCKS = 80;
type JsonObject = Record<string, unknown>;

export type CasioAuthResult =
    | {
          ok: true;
          deviceId: string;
      }
    | {
          ok: false;
          status: number;
          error: string;
      };

export type CasioUsage = {
    promptTokens: number;
    completionTokens: number;
    totalTokens: number;
};

export type CasioSolveResult = {
    answer: string;
    displayText: string;
    displayBlocks: CasioDisplayBlock[];
    model: string;
    usage: CasioUsage;
};

export type CasioAnswerResult = {
    answer: string;
    displayText: string;
    model: string;
    usage: CasioUsage;
    rawBlocks?: unknown;
};

export type CasioRenderResult = {
    displayText: string;
    displayBlocks: CasioDisplayBlock[];
    usage: CasioUsage;
};

export type CasioAiOptions = {
    reasoningEffort?: "minimal" | "low" | "medium" | "high";
    enableWebSearch?: boolean;
    maxOutputTokens?: number;
};

function currentDeviceId(): string {
    return safeText(process.env.CASIO_AI_DEVICE_ID || FALLBACK_DEVICE_ID);
}

function currentDeviceApiKey(): string {
    return safeText(
        process.env.CASIO_AI_DEVICE_API_KEY || FALLBACK_DEVICE_API_KEY
    );
}

export function safeText(value: unknown): string {
    if (typeof value !== "string") return "";
    return value.trim();
}

export function parseIntStrict(value: unknown): number | null {
    if (typeof value === "number" && Number.isInteger(value)) return value;
    if (typeof value !== "string") return null;

    const trimmed = value.trim();
    if (!/^-?\d+$/.test(trimmed)) return null;

    const parsed = Number.parseInt(trimmed, 10);
    return Number.isInteger(parsed) ? parsed : null;
}

export function authenticateCasioRequest(
    request: NextRequest
): CasioAuthResult {
    const requestDeviceId = safeText(request.headers.get("x-device-id"));
    const requestApiKey = safeText(request.headers.get("x-device-api-key"));

    if (!requestDeviceId || !requestApiKey) {
        return {
            ok: false,
            status: 401,
            error: "device_auth_headers_required"
        };
    }

    if (
        requestDeviceId !== currentDeviceId() ||
        requestApiKey !== currentDeviceApiKey()
    ) {
        return {
            ok: false,
            status: 401,
            error: "invalid_device_auth"
        };
    }

    return { ok: true, deviceId: requestDeviceId };
}

export function resolveQuestionNo(
    request: NextRequest,
    bodyQuestionId?: unknown
): number | null {
    const queryValue = parseIntStrict(
        new URL(request.url).searchParams.get("question_id")
    );
    if (queryValue !== null) return queryValue;

    const headerValue = parseIntStrict(request.headers.get("x-question-id"));
    if (headerValue !== null) return headerValue;

    return parseIntStrict(bodyQuestionId);
}

export function resolvePhotoIndex(request: NextRequest): number | null {
    const queryValue = parseIntStrict(
        new URL(request.url).searchParams.get("index")
    );
    if (queryValue !== null) return queryValue;

    return parseIntStrict(request.headers.get("x-photo-index"));
}

function markdownCleanup(text: string): string {
    const lines = text
        .replace(/\r/g, "")
        .replace(/```[\s\S]*?```/g, " ")
        .replace(/`/g, "")
        .replace(/\*\*/g, "")
        .replace(/__/g, "")
        .replace(/~~/g, "")
        .replace(/#+\s?/g, "")
        .replace(/\[([^\]]+)\]\(([^)]+)\)/g, "$1")
        .replace(/\\times/g, "*")
        .replace(/\\cdot/g, "*")
        .replace(/\\div/g, "/")
        .replace(/\\sqrt/g, "sqrt")
        .replace(/\\leq/g, "<=")
        .replace(/\\geq/g, ">=")
        .replace(/\\neq/g, "!=")
        .replace(/\\pi/g, "pi")
        .replace(/\$/g, "")
        .replace(/[“”]/g, '"')
        .replace(/[‘’]/g, "'")
        .split("\n")
        .map((line) => line.trim())
        .filter(Boolean);

    return lines.join("\n");
}

function isWideChar(char: string): boolean {
    return /[\u1100-\u115f\u2e80-\ua4cf\uac00-\ud7a3\uf900-\ufaff\ufe10-\ufe6f\uff00-\uff60\uffe0-\uffe6]/u.test(
        char
    );
}

function wrapLineForOled(line: string, unitLimit: number): string[] {
    if (!line) return [""];
    const output: string[] = [];
    let current = "";
    let units = 0;

    for (const char of line) {
        const weight = isWideChar(char) ? 2 : 1;
        if (units + weight > unitLimit) {
            if (current) output.push(current);
            current = char;
            units = weight;
        } else {
            current += char;
            units += weight;
        }
    }

    if (current) output.push(current);
    return output.length ? output : [""];
}

export function normalizeDisplayText(raw: string): string {
    const cleaned = markdownCleanup(raw).slice(0, MAX_DISPLAY_CHARS);
    if (!cleaned) return "No answer generated.";

    const sourceLines = cleaned.split("\n");
    const resultLines: string[] = [];

    for (const line of sourceLines) {
        resultLines.push(...wrapLineForOled(line, DISPLAY_LINE_UNITS));
    }

    return resultLines.join("\n");
}

function parseJsonFromModelText(text: string): JsonObject | null {
    const direct = safeText(text);
    if (!direct) return null;

    try {
        const parsed = JSON.parse(direct) as unknown;
        if (parsed && typeof parsed === "object" && !Array.isArray(parsed)) {
            return parsed as JsonObject;
        }
    } catch {
        // continue with fallback parse
    }

    const fenceMatch = direct.match(/```(?:json)?\s*([\s\S]*?)\s*```/i);
    if (fenceMatch?.[1]) {
        try {
            const parsed = JSON.parse(fenceMatch[1]) as unknown;
            if (
                parsed &&
                typeof parsed === "object" &&
                !Array.isArray(parsed)
            ) {
                return parsed as JsonObject;
            }
        } catch {
            // continue
        }
    }

    const firstBrace = direct.indexOf("{");
    const lastBrace = direct.lastIndexOf("}");
    if (firstBrace >= 0 && lastBrace > firstBrace) {
        const candidate = direct.slice(firstBrace, lastBrace + 1);
        try {
            const parsed = JSON.parse(candidate) as unknown;
            if (
                parsed &&
                typeof parsed === "object" &&
                !Array.isArray(parsed)
            ) {
                return parsed as JsonObject;
            }
        } catch {
            // ignore
        }
    }

    return null;
}

function resolveModel(): string {
    const defaultModel =
        safeText(process.env.CASIO_AI_OPENAI_MODEL) || "gpt-5.4";
    return defaultModel;
}

function resolveReasoningEffort(
    value?: string
): "minimal" | "low" | "medium" | "high" {
    const raw = safeText(value).toLowerCase();
    if (raw === "minimal") return "minimal";
    if (raw === "low") return "low";
    if (raw === "high") return "high";
    return "medium";
}

function normalizeUsage(raw: unknown): CasioUsage {
    const usage = (raw || {}) as Record<string, unknown>;
    const promptTokens = Number(usage.prompt_tokens || usage.input_tokens || 0);
    const completionTokens = Number(
        usage.completion_tokens || usage.output_tokens || 0
    );
    const totalTokens = Number(
        usage.total_tokens || promptTokens + completionTokens
    );

    return {
        promptTokens: Number.isFinite(promptTokens) ? promptTokens : 0,
        completionTokens: Number.isFinite(completionTokens)
            ? completionTokens
            : 0,
        totalTokens: Number.isFinite(totalTokens) ? totalTokens : 0
    };
}

function buildUserPrompt(
    contextTail: string,
    mode: "default" | "rescue"
): string {
    const compactContext = safeText(contextTail).slice(
        0,
        MAX_CONTEXT_TAIL_CHARS
    );
    const prefix = compactContext
        ? `Recent context:\n${compactContext}\n\n`
        : "";

    const basePrompt = `${prefix}你是专业考试解题老师。请仔细阅读所有照片，把可辨认的题目逐题解出来。

解题要求：
- 像考试标准答案一样写，直接给题目解法。
- 计算过程不要跳步骤，关键代入、化简、比较都要写出来。
- 语言不要冗余，不要长篇解释，优先公式、计算、结论。
- 如果是选择题，每个小题/选项逐一给答案；一行一个答案即可。
- 如果照片稍微模糊但还能推断题意，要尽力解，不要轻易叫用户重拍。
- 不要在答案最后要求用户补拍/重拍；除非整组题目几乎完全不可辨认，否则直接给可辨认部分的标准答案。

输出格式：
- 只返回严格 JSON：
{
  "answer_full": "full learning explanation in Markdown/plain text"
}
- 不要 markdown code fence。
- 不要额外 key。
- 第一波只负责解题，不要设计 OLED 页面，不要生成 bitmap。`;

    if (mode === "rescue") {
        return `${basePrompt}

Rescue policy (strict):
- Do not reject because image is slightly blurry.
- If any question fragment is readable, provide best-effort solved answer.
- If uncertain, state short assumptions and continue solving.
- Return unreadable message only when ALL visible questions are impossible to identify.`;
    }

    return basePrompt;
}

function buildOledBitmapPrompt(answer: string): string {
    const answerChars = safeText(answer).length;
    const minimumPages = Math.max(
        6,
        Math.min(OLED_MAX_DIRECT_BLOCKS, Math.ceil(answerChars / 90))
    );

    return `You are the final OLED page designer and 1-bit bitmap generator for a Casio AI learning machine.

TASK:
Convert the solved answer below into excellent ESP32-ready OLED display pages, then generate the final 1-bit XBM bitmap blocks.
The OLED screen is exactly 128×32 px.
The solved answer has about ${answerChars} characters; produce at least ${minimumPages} OLED pages unless the answer is genuinely shorter after removing repeated filler.

IMPORTANT:
- This second pass is responsible for display quality: semantic paging, line breaking, math simplification, formula splitting, and 1-bit bitmap generation.
- Follow the provided strategy strictly. The goal is readability on a tiny screen, not preserving Markdown layout.
- The server will only validate JSON/base64/byte sizes and use oled_pages as fallback. Do not rely on the server for layout decisions.
- Output STRICT JSON only. No markdown fences. No extra commentary.
- Do not summarize away the answer. Cover every numbered question/section and every important equation/conclusion from the solved answer.
- Remove only redundant prose, uncertainty disclaimers, and photo-retake comments.

BITMAP FORMAT:
- Return display_blocks only as 1-bit XBM packed bytes, base64 encoded.
- Each block MUST decode to exactly 512 bytes.
- Each block MUST be width=128, height=32, format="1bit_xbm".
- Convention: 1 = OLED pixel ON, 0 = background OFF.
- Byte packing: XBM bit order, row-major, 8 horizontal pixels per byte, least significant bit is leftmost pixel in that byte.
- Do not invert colors. Normal OLED should be black/off background with lit/on text strokes.
- Most pixels should be OFF. Only text/formula strokes should be ON.

OUTPUT JSON SHAPE:
{
  "display_text": "short plain-text preview",
  "oled_pages": [
    { "kind": "text", "lines": ["line 1", "line 2"] }
  ],
  "display_blocks": [
    {
      "type": "bitmap",
      "kind": "text",
      "width": 128,
      "height": 32,
      "format": "1bit_xbm",
      "data": "base64..."
    }
  ]
}

oled_pages is required as a readable fallback plan:
- It must contain the exact OLED pages before bitmap encoding and must match display_blocks one-to-one whenever possible.
- Each page has 1-2 lines only.
- Avoid blank lines and standalone transition pages.
- The server will use oled_pages only if your direct display_blocks are invalid.
- oled_pages must be complete, not a short preview. If the answer has sections 1-6, oled_pages must cover sections 1-6.

OLED PAGING STRATEGY:
- Follow this style very closely, based on the successful test example:
  1. Segment by meaning, not by original paragraphs: 题号/定义 -> 条件 -> 等价转化 -> 公式 -> 试算/计算 -> 结论.
  2. A normal page is 2 lines × 16px high. Use both lines whenever possible.
  3. Do NOT create pages containing only “即：”, “其中：”, “所以：”, “题意要求：”, “经计算：”, “且”. Merge that cue with the next useful formula/line.
  4. Do NOT leave an empty second line unless it is a title page or final answer page.
  5. If spacing is truly needed between sections, make a full 16px line gap by starting the next information unit on a new page; do not waste half a page.
  6. Chinese/English text should visually occupy about 16px line height. Do not make tiny text.
  7. Simple math must be shown as compact text over 1-2 lines, not rendered as giant image formulas.
  8. Long formulas must be split into multiple readable pages, not one ultra-wide horizontal-scroll formula.
  9. A math expression can use one 32px-high screen if it benefits readability.
  10. If a formula genuinely cannot fit in 32px height, use a 64px-high logical formula split into TWO consecutive 128×32 pages: top half page first, bottom half page second.
  11. For 64px logical formulas, oled_pages should still list the two visible 32px pages in order and display_blocks should contain the same two 128×32 bitmaps.
  12. Use compact readable notation when better for OLED: Σ, √(), ^, /, [], ±, ≤, ≥, ≈, ω, Ω.
  13. Final answer must be clear, preferably one strong final page.
  14. Never make blocks wider than 128px. Avoid horizontal scrolling in this AI-generated mode.
  15. Do not output a short summary like only 3-10 pages when the solved answer has many equations/sections.
  16. Ignore final “photo unclear / retake” disclaimers unless the whole answer is unreadable.

GOOD OLED TEXT PLANNING EXAMPLES:
- "设次品数为 X" / "X~B(n,0.1)"
- "题意要求：" / "P(X>10)>=0.9"
- "Σ k=0..10" / "C(n,k)(0.1)^k"
- "×(0.9)^(n-k)" / "≤0.1"
- "n=152 时" / "P≈0.9031>0.9"
- "第16题" as a title page is allowed.
- "答案：152件" as a final page is allowed.

BAD:
- A page containing only “即：” or “其中：”.
- Very wide formula blocks requiring many left/right scrolls.
- White filled background with dark holes; use normal OLED lit strokes on off background.
- Tiny text that wastes vertical space.
- Formula rendered too tall so the bottom half of characters is cut off.
- Random blank vertical space between line 1 and line 2.
- One long sentence squeezed into unreadable small font.
- Only showing the first half or a high-level summary of the answer.
- Including “please retake photo” pages after already giving a usable answer.

SOLVED ANSWER TO RENDER:
${answer}`;
}

type OpenAiCallResult = {
    status: number;
    payload: JsonObject;
};

function openAiErrorMessage(payload: JsonObject): string {
    const errorPayload = (payload?.error || {}) as JsonObject;
    return (
        safeText(errorPayload.message) ||
        safeText(errorPayload.code) ||
        safeText(payload.raw)
    );
}

function isUnknownPromptVariableError(payload: JsonObject): boolean {
    const message = openAiErrorMessage(payload).toLowerCase();
    return message.includes("unknown prompt variables");
}

function resolvePromptConfig(kind: "solve" | "layout"): {
    id: string;
    version: string;
} {
    if (kind === "solve") {
        return {
            id:
                safeText(process.env.CASIO_AI_OPENAI_SOLVE_PROMPT_ID) ||
                DEFAULT_SOLVE_PROMPT_ID,
            version:
                safeText(process.env.CASIO_AI_OPENAI_SOLVE_PROMPT_VERSION) ||
                DEFAULT_SOLVE_PROMPT_VERSION
        };
    }

    return {
        id:
            safeText(process.env.CASIO_AI_OPENAI_LAYOUT_PROMPT_ID) ||
            DEFAULT_LAYOUT_PROMPT_ID,
        version:
            safeText(process.env.CASIO_AI_OPENAI_LAYOUT_PROMPT_VERSION) ||
            DEFAULT_LAYOUT_PROMPT_VERSION
    };
}

async function callOpenAiPromptResponses(args: {
    apiKey: string;
    promptId: string;
    promptVersion: string;
    input?: unknown;
    variables?: JsonObject;
    timeoutMs?: number;
}): Promise<OpenAiCallResult> {
    const prompt: JsonObject = {
        id: args.promptId,
        version: args.promptVersion
    };

    if (args.variables && Object.keys(args.variables).length > 0) {
        prompt.variables = args.variables;
    }

    const requestBody: JsonObject = { prompt };
    if (args.input !== undefined) {
        requestBody.input = args.input;
    }

    const controller = new AbortController();
    const timeout = setTimeout(
        () => controller.abort(),
        args.timeoutMs && args.timeoutMs > 0 ? args.timeoutMs : 240000
    );

    try {
        const response = await fetch("https://api.openai.com/v1/responses", {
            method: "POST",
            headers: {
                "Content-Type": "application/json",
                Authorization: `Bearer ${args.apiKey}`
            },
            body: JSON.stringify(requestBody),
            signal: controller.signal
        });

        const rawText = await response.text();
        let payload: JsonObject = {};
        if (rawText) {
            try {
                payload = JSON.parse(rawText) as JsonObject;
            } catch {
                payload = { raw: rawText };
            }
        }

        return {
            status: response.status,
            payload
        };
    } finally {
        clearTimeout(timeout);
    }
}

async function callOpenAiChatCompletions(args: {
    apiKey: string;
    model: string;
    imageUrls: string[];
    contextTail: string;
    promptMode: "default" | "rescue";
    forceNoJsonMode?: boolean;
    maxOutputTokens?: number;
}): Promise<OpenAiCallResult> {
    const contentBlocks: JsonObject[] = [
        { type: "text", text: buildUserPrompt(args.contextTail, args.promptMode) }
    ];

    for (const imageUrl of args.imageUrls) {
        contentBlocks.push({
            type: "image_url",
            image_url: { url: imageUrl }
        });
    }

    const requestBody: JsonObject = {
        model: args.model,
        messages: [
            {
                role: "system",
                content:
                    "You are a study assistant for a tiny 128x32 OLED device. Always respond in valid JSON."
            },
            {
                role: "user",
                content: contentBlocks
            }
        ]
    };
    if (args.maxOutputTokens && args.maxOutputTokens > 0) {
        requestBody.max_completion_tokens = args.maxOutputTokens;
    }

    if (!args.forceNoJsonMode) {
        requestBody.response_format = { type: "json_object" };
    }

    const controller = new AbortController();
    const timeout = setTimeout(() => controller.abort(), 120000);

    try {
        const response = await fetch(
            "https://api.openai.com/v1/chat/completions",
            {
                method: "POST",
                headers: {
                    "Content-Type": "application/json",
                    Authorization: `Bearer ${args.apiKey}`
                },
                body: JSON.stringify(requestBody),
                signal: controller.signal
            }
        );

        const rawText = await response.text();
        let payload: JsonObject = {};

        if (rawText) {
            try {
                payload = JSON.parse(rawText) as JsonObject;
            } catch {
                payload = { raw: rawText };
            }
        }

        return {
            status: response.status,
            payload
        };
    } finally {
        clearTimeout(timeout);
    }
}

function extractResponsesText(payload: JsonObject): string {
    const outputText = safeText(payload.output_text);
    if (outputText) return outputText;

    const output = Array.isArray(payload.output)
        ? (payload.output as JsonObject[])
        : [];
    const message =
        output.find((item) => safeText(item.type) === "message") || {};
    const content = Array.isArray(message.content)
        ? (message.content as JsonObject[])
        : [];
    const textParts = content
        .filter((item) => safeText(item.type) === "output_text")
        .map((item) => safeText(item.text))
        .filter(Boolean);
    return textParts.join("\n");
}

function extractModelText(payload: JsonObject): string {
    const responsesText = extractResponsesText(payload);
    if (responsesText) return responsesText;

    const choices = Array.isArray(payload.choices)
        ? (payload.choices as JsonObject[])
        : [];
    const firstChoice = choices[0] || {};
    const messageObject = (firstChoice.message || {}) as JsonObject;
    const rawContent = messageObject.content;

    if (typeof rawContent === "string") return rawContent;
    if (Array.isArray(rawContent)) {
        return rawContent
            .map((part) => {
                if (typeof part === "string") return part;
                if (part && typeof part === "object") {
                    const typed = part as JsonObject;
                    return safeText(typed.text);
                }
                return "";
            })
            .filter(Boolean)
            .join("\n");
    }

    return "";
}

function shouldRetryEmptyModelOutput(
    call: OpenAiCallResult,
    contentText: string,
    maxOutputTokens?: number
): boolean {
    if (safeText(contentText)) return false;

    const status = safeText(call.payload.status).toLowerCase();
    const incompleteDetails = (call.payload.incomplete_details || {}) as JsonObject;
    const incompleteReason = safeText(incompleteDetails.reason).toLowerCase();
    const usage = normalizeUsage(call.payload.usage);

    return (
        status === "incomplete" ||
        incompleteReason.includes("max_output") ||
        (typeof maxOutputTokens === "number" &&
            maxOutputTokens > 0 &&
            usage.completionTokens >= Math.floor(maxOutputTokens * 0.9))
    );
}

function looksLikeUnreadableReply(answer: string): boolean {
    const s = safeText(answer).toLowerCase();
    if (!s) return false;
    const hints = [
        "看不清",
        "不清晰",
        "过于模糊",
        "无法辨认",
        "无法识别",
        "重拍",
        "照片太糊",
        "too blurry",
        "cannot read",
        "can't read",
        "unreadable",
        "retake"
    ];
    return hints.some((hint) => s.includes(hint));
}

async function callOpenAiResponses(args: {
    apiKey: string;
    model: string;
    imageUrls: string[];
    contextTail: string;
    promptMode: "default" | "rescue";
    reasoningEffort: "minimal" | "low" | "medium" | "high";
    enableWebSearch: boolean;
    maxOutputTokens?: number;
}): Promise<OpenAiCallResult> {
    const userContent: JsonObject[] = [
        { type: "input_text", text: buildUserPrompt(args.contextTail, args.promptMode) }
    ];

    for (const imageUrl of args.imageUrls) {
        userContent.push({
            type: "input_image",
            image_url: imageUrl
        });
    }

    const requestBody: JsonObject = {
        model: args.model,
        reasoning: {
            effort: args.reasoningEffort
        },
        input: [
            {
                role: "user",
                content: userContent
            }
        ]
    };

    if (args.enableWebSearch) {
        requestBody.tools = [{ type: "web_search" }];
        requestBody.tool_choice = "auto";
    }

    if (args.maxOutputTokens && args.maxOutputTokens > 0) {
        requestBody.max_output_tokens = args.maxOutputTokens;
    }

    const controller = new AbortController();
    const timeout = setTimeout(() => controller.abort(), 140000);

    try {
        const response = await fetch("https://api.openai.com/v1/responses", {
            method: "POST",
            headers: {
                "Content-Type": "application/json",
                Authorization: `Bearer ${args.apiKey}`
            },
            body: JSON.stringify(requestBody),
            signal: controller.signal
        });

        const rawText = await response.text();
        let payload: JsonObject = {};
        if (rawText) {
            try {
                payload = JSON.parse(rawText) as JsonObject;
            } catch {
                payload = { raw: rawText };
            }
        }

        return {
            status: response.status,
            payload
        };
    } finally {
        clearTimeout(timeout);
    }
}

async function callOpenAiTextResponses(args: {
    apiKey: string;
    model: string;
    prompt: string;
    reasoningEffort: "minimal" | "low" | "medium" | "high";
    timeoutMs?: number;
}): Promise<OpenAiCallResult> {
    const requestBody: JsonObject = {
        model: args.model,
        reasoning: {
            effort: args.reasoningEffort
        },
        input: [
            {
                role: "user",
                content: [{ type: "input_text", text: args.prompt }]
            }
        ]
    };

    const controller = new AbortController();
    const timeout = setTimeout(
        () => controller.abort(),
        args.timeoutMs && args.timeoutMs > 0 ? args.timeoutMs : 140000
    );

    try {
        const response = await fetch("https://api.openai.com/v1/responses", {
            method: "POST",
            headers: {
                "Content-Type": "application/json",
                Authorization: `Bearer ${args.apiKey}`
            },
            body: JSON.stringify(requestBody),
            signal: controller.signal
        });

        const rawText = await response.text();
        let payload: JsonObject = {};
        if (rawText) {
            try {
                payload = JSON.parse(rawText) as JsonObject;
            } catch {
                payload = { raw: rawText };
            }
        }

        return {
            status: response.status,
            payload
        };
    } finally {
        clearTimeout(timeout);
    }
}

function validateAiBitmapBlocks(rawBlocks: unknown): CasioDisplayBlock[] {
    if (!Array.isArray(rawBlocks)) return [];

    const output: CasioDisplayBlock[] = [];
    for (const rawBlock of rawBlocks) {
        if (rawBlock && typeof rawBlock === "object") {
            const block = rawBlock as JsonObject;
            const type = safeText(block.type);
            const rawKind = safeText(block.kind).toLowerCase();
            const kind = rawKind === "formula" ? "formula" : "text";
            const width = Number(block.width);
            const height = Number(block.height);
            const format = safeText(block.format);
            const data = safeText(block.data)
                .replace(/\s+/g, "")
                .replace(/-/g, "+")
                .replace(/_/g, "/");
            let decodedLength = -1;

            try {
                decodedLength = Buffer.from(data, "base64").length;
            } catch {
                decodedLength = -1;
            }

            if (
                type === "bitmap" &&
                width === OLED_BITMAP_WIDTH &&
                height === OLED_BITMAP_HEIGHT &&
                format === "1bit_xbm" &&
                data &&
                /^[A-Za-z0-9+/]+={0,2}$/.test(data) &&
                decodedLength === OLED_BITMAP_BYTES
            ) {
                output.push({
                    type: "bitmap",
                    kind,
                    width,
                    height,
                    format,
                    data
                });
            }
        }
    }

    return output;
}

function countSetBits(byte: number): number {
    let value = byte;
    let count = 0;
    while (value > 0) {
        count += value & 1;
        value >>= 1;
    }
    return count;
}

function normalizeOledPolarity(blocks: CasioDisplayBlock[]): CasioDisplayBlock[] {
    return blocks.map((block) => {
        const decoded = Buffer.from(block.data, "base64");
        const onBits = decoded.reduce(
            (total, byte) => total + countSetBits(byte),
            0
        );
        const totalBits = block.width * block.height;
        const density = totalBits > 0 ? onBits / totalBits : 0;

        if (density <= 0.5) return block;

        const inverted = Buffer.from(decoded.map((byte) => byte ^ 0xff));
        return {
            ...block,
            data: inverted.toString("base64")
        };
    });
}

function countOledPageTextChars(rawPages: unknown): number {
    if (!Array.isArray(rawPages)) return 0;
    return rawPages.reduce((total, rawPage) => {
        if (!rawPage || typeof rawPage !== "object") return total;
        const page = rawPage as JsonObject;
        const lines = Array.isArray(page.lines)
            ? page.lines.map((line) => safeText(line)).join("")
            : safeText(page.text);
        return total + lines.length;
    }, 0);
}

function combineUsage(...items: CasioUsage[]): CasioUsage {
    return items.reduce(
        (total, item) => ({
            promptTokens: total.promptTokens + item.promptTokens,
            completionTokens: total.completionTokens + item.completionTokens,
            totalTokens: total.totalTokens + item.totalTokens
        }),
        { promptTokens: 0, completionTokens: 0, totalTokens: 0 }
    );
}

async function generateOledBitmapsWithOpenAi(args: {
    apiKey: string;
    model: string;
    answer: string;
}): Promise<{
    displayText: string;
    displayBlocks: CasioDisplayBlock[];
    usage: CasioUsage;
}> {
    const startedAt = Date.now();
    const prompt = buildOledBitmapPrompt(args.answer);
    const attempts: Array<{
        reasoningEffort: "minimal" | "low" | "medium" | "high";
        timeoutMs: number;
    }> = [
        { reasoningEffort: "medium", timeoutMs: 75000 },
        { reasoningEffort: "low", timeoutMs: 65000 }
    ];
    let lastError: unknown = null;

    for (let attemptIndex = 0; attemptIndex < attempts.length; attemptIndex += 1) {
        const attempt = attempts[attemptIndex];
        try {
            const call = await callOpenAiTextResponses({
                apiKey: args.apiKey,
                model: args.model,
                prompt,
                reasoningEffort: attempt.reasoningEffort,
                timeoutMs: attempt.timeoutMs
            });

            console.log(`${LOG_PREFIX} oled_bitmap_ai_done`, {
                ms: Date.now() - startedAt,
                attempt: attemptIndex + 1,
                attempts: attempts.length,
                reasoningEffort: attempt.reasoningEffort,
                status: call.status
            });

            if (call.status < 200 || call.status >= 300) {
                const errorPayload = (call.payload?.error || {}) as JsonObject;
                const message =
                    safeText(errorPayload.message) ||
                    safeText(errorPayload.code) ||
                    "oled_bitmap_ai_failed";
                lastError = new Error(message);
                console.warn(`${LOG_PREFIX} oled_bitmap_ai_retryable_status`, {
                    attempt: attemptIndex + 1,
                    status: call.status,
                    message
                });
                continue;
            }

            const contentText = extractModelText(call.payload);
            const parsed = parseJsonFromModelText(contentText);
            let displayBlocks = validateAiBitmapBlocks(parsed?.display_blocks);
            if (!displayBlocks.length) {
                displayBlocks = validateAiBitmapBlocks(parsed?.blocks);
            }
            const displayText =
                safeText(parsed?.display_text) ||
                safeText(parsed?.displayText) ||
                args.answer;

            console.log(`${LOG_PREFIX} oled_bitmap_ai_validated`, {
                ms: Date.now() - startedAt,
                attempt: attemptIndex + 1,
                blocks: displayBlocks.length,
                bytes: displayBlocks.reduce(
                    (total, block) => total + Buffer.byteLength(block.data, "base64"),
                    0
                ),
                rawChars: contentText.length,
                rawPreview: safeText(contentText).slice(0, 300)
            });

            if (!displayBlocks.length) {
                displayBlocks = await buildDisplayBlocksFromOledPages(parsed?.oled_pages);
            }

            if (!displayBlocks.length && displayText !== args.answer) {
                displayBlocks = await buildDisplayBlocksFromModel({
                    answer: displayText
                });
            }

            displayBlocks = normalizeOledPolarity(displayBlocks);

            if (!displayBlocks.length) {
                lastError = new Error("oled_bitmap_ai_empty_blocks");
                console.warn(`${LOG_PREFIX} oled_bitmap_ai_empty_blocks`, {
                    attempt: attemptIndex + 1,
                    rawChars: contentText.length
                });
                continue;
            }

            console.log(`${LOG_PREFIX} oled_bitmap_or_pages_ready`, {
                ms: Date.now() - startedAt,
                blocks: displayBlocks.length,
                oledPageChars: countOledPageTextChars(parsed?.oled_pages),
                answerChars: args.answer.length,
                source: validateAiBitmapBlocks(parsed?.display_blocks).length
                    ? "ai_direct_bitmap"
                    : "ai_oled_pages_or_preview"
            });

            return {
                displayText,
                displayBlocks,
                usage: normalizeUsage(call.payload.usage)
            };
        } catch (error) {
            lastError = error;
            console.warn(`${LOG_PREFIX} oled_bitmap_ai_attempt_failed`, {
                attempt: attemptIndex + 1,
                attempts: attempts.length,
                reasoningEffort: attempt.reasoningEffort,
                message:
                    error && typeof error === "object" && "message" in error
                        ? safeText((error as { message?: unknown }).message)
                        : safeText(error)
            });
        }
    }

    if (lastError instanceof Error) {
        throw lastError;
    }
    throw new Error("oled_bitmap_ai_failed_after_retries");
}

export async function solveCasioAnswerWithOpenAi(args: {
    imageUrls: string[];
    contextTail: string;
    options?: CasioAiOptions;
}): Promise<CasioAnswerResult> {
    const apiKey = safeText(process.env.OPENAI_API_KEY);
    if (!apiKey) {
        const err = new Error("openai_api_key_missing") as Error & {
            status?: number;
        };
        err.status = 500;
        throw err;
    }

    if (!args.imageUrls.length) {
        const err = new Error("photo_ids_required") as Error & {
            status?: number;
        };
        err.status = 400;
        throw err;
    }

    const solvePrompt = resolvePromptConfig("solve");
    const userContent: JsonObject[] = [
        {
            type: "input_text",
            text:
                "Please solve the attached question photos according to the configured prompt. Use the photos as the source of truth."
        }
    ];
    const contextTail = safeText(args.contextTail).slice(0, MAX_CONTEXT_TAIL_CHARS);
    if (contextTail) {
        userContent.push({
            type: "input_text",
            text: `Recent context:\n${contextTail}`
        });
    }
    for (const imageUrl of args.imageUrls) {
        userContent.push({
            type: "input_image",
            image_url: imageUrl
        });
    }

    console.log(`${LOG_PREFIX} openai_prompt_solve_start`, {
        promptId: solvePrompt.id,
        promptVersion: solvePrompt.version,
        images: args.imageUrls.length,
        contextTailChars: contextTail.length
    });
    const openAiStartedAt = Date.now();
    const call = await callOpenAiPromptResponses({
        apiKey,
        promptId: solvePrompt.id,
        promptVersion: solvePrompt.version,
        input: [
            {
                role: "user",
                content: userContent
            }
        ],
        timeoutMs: 260000
    });
    console.log(`${LOG_PREFIX} openai_prompt_solve_done`, {
        ms: Date.now() - openAiStartedAt,
        status: call.status,
        model: safeText(call.payload.model)
    });

    if (call.status < 200 || call.status >= 300) {
        const errorPayload = (call.payload?.error || {}) as JsonObject;
        const message =
            safeText(errorPayload.message) ||
            safeText(errorPayload.code) ||
            "openai_request_failed";
        const err = new Error(message) as Error & { status?: number };
        err.status = call.status;
        throw err;
    }

    const contentText = extractModelText(call.payload);
    const outputParsed =
        call.payload.output_parsed &&
        typeof call.payload.output_parsed === "object" &&
        !Array.isArray(call.payload.output_parsed)
            ? (call.payload.output_parsed as JsonObject)
            : null;
    const parsed = parseJsonFromModelText(contentText) || outputParsed;
    if (!safeText(contentText) && !parsed) {
        const err = new Error("openai_empty_output") as Error & { status?: number };
        err.status = 502;
        throw err;
    }
    const answer =
        safeText(parsed?.answer_full) ||
        safeText(parsed?.answer) ||
        safeText(parsed?.full_answer) ||
        safeText(contentText) ||
        "No answer generated.";

    const previewLine = safeText(
        parsed?.display_text ||
            parsed?.displayText ||
            parsed?.answer_full ||
            parsed?.answer
    );
    const displayTextRaw = previewLine || answer;

    return {
        answer,
        displayText: normalizeDisplayText(displayTextRaw),
        model:
            safeText(call.payload.model) ||
            `prompt:${solvePrompt.id}@${solvePrompt.version}`,
        usage: normalizeUsage(call.payload.usage),
        rawBlocks: parsed?.blocks
    };
}

export async function renderCasioAnswerForOledWithOpenAi(args: {
    answer: string;
    displayText?: string;
    model?: string;
    rawBlocks?: unknown;
}): Promise<CasioRenderResult> {
    const startedAt = Date.now();
    const apiKey = safeText(process.env.OPENAI_API_KEY);
    if (!apiKey) {
        const err = new Error("openai_api_key_missing") as Error & {
            status?: number;
        };
        err.status = 500;
        throw err;
    }

    let displayTextRaw = safeText(args.displayText) || args.answer;
    let displayBlocks: CasioDisplayBlock[] = [];
    let layoutUsage: CasioUsage = {
        promptTokens: 0,
        completionTokens: 0,
        totalTokens: 0
    };
    const layoutPrompt = resolvePromptConfig("layout");

    const attempts: Array<{
        timeoutMs: number;
        mode: "variables" | "input";
    }> = [
        { timeoutMs: 210000, mode: "variables" },
        { timeoutMs: 210000, mode: "input" },
        { timeoutMs: 70000, mode: "input" }
    ];

    for (let attemptIndex = 0; attemptIndex < attempts.length; attemptIndex += 1) {
        const attempt = attempts[attemptIndex];
        try {
            const layoutStartedAt = Date.now();
            console.log(`${LOG_PREFIX} openai_prompt_layout_start`, {
                promptId: layoutPrompt.id,
                promptVersion: layoutPrompt.version,
                attempt: attemptIndex + 1,
                attempts: attempts.length,
                mode: attempt.mode,
                answerChars: args.answer.length
            });

            const call = await callOpenAiPromptResponses({
                apiKey,
                promptId: layoutPrompt.id,
                promptVersion: layoutPrompt.version,
                variables:
                    attempt.mode === "variables"
                        ? {
                              answer_full: args.answer
                          }
                        : undefined,
                input:
                    attempt.mode === "input"
                        ? [
                              {
                                  role: "user",
                                  content: [
                                      {
                                          type: "input_text",
                                          text: `answer_full:\n${args.answer}`
                                      }
                                  ]
                              }
                          ]
                        : undefined,
                timeoutMs: attempt.timeoutMs
            });

            console.log(`${LOG_PREFIX} openai_prompt_layout_done`, {
                ms: Date.now() - layoutStartedAt,
                attempt: attemptIndex + 1,
                status: call.status,
                mode: attempt.mode,
                model: safeText(call.payload.model)
            });

            if (call.status < 200 || call.status >= 300) {
                const message = openAiErrorMessage(call.payload);
                console.warn(`${LOG_PREFIX} openai_prompt_layout_status_retry`, {
                    attempt: attemptIndex + 1,
                    status: call.status,
                    mode: attempt.mode,
                    message: message || "layout_prompt_failed"
                });
                if (
                    attempt.mode === "variables" &&
                    !isUnknownPromptVariableError(call.payload)
                ) {
                    break;
                }
                continue;
            }

            const contentText = extractModelText(call.payload);
            const outputParsed =
                call.payload.output_parsed &&
                typeof call.payload.output_parsed === "object" &&
                !Array.isArray(call.payload.output_parsed)
                    ? (call.payload.output_parsed as JsonObject)
                    : null;
            const parsed = parseJsonFromModelText(contentText) || outputParsed;
            const layout = parsed?.oled_layout;

            displayBlocks = await buildDisplayBlocksFromOledLayout(layout);
            displayBlocks = normalizeOledPolarity(displayBlocks);
            layoutUsage = normalizeUsage(call.payload.usage);
            displayTextRaw =
                safeText(parsed?.display_text) ||
                getDisplayTextFromOledLayout(layout) ||
                displayTextRaw;

            console.log(`${LOG_PREFIX} server_oled_layout_render_done`, {
                ms: Date.now() - layoutStartedAt,
                totalMs: Date.now() - startedAt,
                attempt: attemptIndex + 1,
                mode: attempt.mode,
                layoutBlocks: Array.isArray(layout) ? layout.length : 0,
                displayBlocks: displayBlocks.length,
                answerChars: args.answer.length,
                source: "openai_prompt_layout"
            });

            if (displayBlocks.length) break;
        } catch (error) {
            console.warn(`${LOG_PREFIX} openai_prompt_layout_attempt_failed`, {
                attempt: attemptIndex + 1,
                attempts: attempts.length,
                message:
                    error && typeof error === "object" && "message" in error
                        ? safeText((error as { message?: unknown }).message)
                        : safeText(error)
            });
        }
    }

    if (!displayBlocks.length) {
        const renderStartedAt = Date.now();
        displayBlocks = await buildDisplayBlocksFromModel({
            blocks: args.rawBlocks,
            answer: args.answer
        });
        console.warn(`${LOG_PREFIX} render_done`, {
            ms: Date.now() - renderStartedAt,
            totalMs: Date.now() - startedAt,
            displayBlocks: displayBlocks.length,
            answerChars: args.answer.length,
            hasModelBlocks: Array.isArray(args.rawBlocks),
            source: "server_emergency_fallback"
        });
    }

    if (!displayBlocks.length) {
        const err = new Error("oled_display_blocks_empty_after_fallback") as Error & {
            status?: number;
        };
        err.status = 502;
        throw err;
    }

    displayBlocks = normalizeOledPolarity(displayBlocks);

    return {
        displayText: normalizeDisplayText(displayTextRaw),
        displayBlocks,
        usage: layoutUsage
    };
}

export async function solveCasioWithOpenAi(args: {
    imageUrls: string[];
    contextTail: string;
    options?: CasioAiOptions;
}): Promise<CasioSolveResult> {
    const answerResult = await solveCasioAnswerWithOpenAi(args);
    const {
        displayText: finalDisplayText,
        displayBlocks: finalDisplayBlocks,
        usage: finalUsage
    } = await renderCasioAnswerForOledWithOpenAi({
        answer: answerResult.answer,
        displayText: answerResult.displayText,
        model: answerResult.model,
        rawBlocks: answerResult.rawBlocks
    });

    return {
        answer: answerResult.answer,
        displayText: finalDisplayText,
        displayBlocks: finalDisplayBlocks,
        model: answerResult.model,
        usage: combineUsage(answerResult.usage, finalUsage)
    };
}
