/* eslint-disable no-console */
import "@/app/utils/casioFontconfig";
import sharp from "sharp";
import { mathjax } from "mathjax-full/js/mathjax.js";
import { TeX } from "mathjax-full/js/input/tex.js";
import { SVG } from "mathjax-full/js/output/svg.js";
import { liteAdaptor } from "mathjax-full/js/adaptors/liteAdaptor.js";
import { RegisterHTMLHandler } from "mathjax-full/js/handlers/html.js";
import { AllPackages } from "mathjax-full/js/input/tex/AllPackages.js";
import fs from "node:fs";
import path from "node:path";
import type { OverlayOptions } from "sharp";

const OLED_WIDTH = 128;
const OLED_HEIGHT = 32;
const TALL_FORMULA_HEIGHT = 64;
const TEXT_UNIT_LIMIT = 22;
const OLED_FIXED_PAGE_UNIT_LIMIT = 16;
const MIN_TEXT_LINE_HEIGHT = 16;
const TEXT_ROW_HEIGHT = 16;
const FORMULA_TEXT_HEIGHT = 26;
const TEXT_LINES_PER_PAGE = 2;
const DEFAULT_TEXT_FONT = "Noto Sans CJK SC";
const TEXT_FONT_SIZE = 16;
const LOG_PREFIX = "[CASIO_DISPLAY]";
const RENDER_LATEX_BITMAPS =
    safeText(process.env.CASIO_AI_RENDER_LATEX_BITMAPS).toLowerCase() === "true";

type RawModelBlock = {
    type: "text" | "formula";
    text?: string;
    latex?: string;
};

export type CasioDisplayBlock = {
    type: "bitmap";
    kind: "text" | "formula";
    width: number;
    height: number;
    format: "1bit_xbm";
    data: string;
};

export type CasioOledPage = {
    kind?: unknown;
    lines?: unknown;
    text?: unknown;
};

export type CasioOledLayoutBlock = {
    type?: unknown;
    height?: unknown;
    lines?: unknown;
    latex?: unknown;
};

const adaptor = liteAdaptor();
RegisterHTMLHandler(adaptor);
const tex = new TeX({ packages: AllPackages });
const svg = new SVG({ fontCache: "none" });
const mathDocument = mathjax.document("", {
    InputJax: tex,
    OutputJax: svg
});

function safeText(value: unknown): string {
    return typeof value === "string" ? value.trim() : "";
}

function isWideChar(char: string): boolean {
    return /[\u1100-\u115f\u2e80-\ua4cf\uac00-\ud7a3\uf900-\ufaff\ufe10-\ufe6f\uff00-\uff60\uffe0-\uffe6]/u.test(
        char
    );
}

function wrapLineByUnits(line: string, unitLimit: number): string[] {
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

function countVisualUnits(line: string): number {
    let units = 0;
    for (const char of line) {
        units += isWideChar(char) ? 2 : 1;
    }
    return units;
}

function splitTextIntoPages(text: string): string[] {
    const normalized = safeText(text);
    if (!normalized) return [];

    const physicalLines = normalized
        .replace(/\r/g, "")
        .split("\n")
        .map((line) => line.trim())
        .filter(Boolean);

    const wrapped: string[] = [];
    for (const line of physicalLines) {
        wrapped.push(...wrapLineByUnits(line, TEXT_UNIT_LIMIT));
    }

    const pages: string[] = [];
    for (let index = 0; index < wrapped.length; index += TEXT_LINES_PER_PAGE) {
        const pageLines = wrapped.slice(index, index + TEXT_LINES_PER_PAGE);
        if (pageLines.length) pages.push(pageLines.join("\n"));
    }

    return pages;
}

function resolveFontFile(): string | undefined {
    const custom = safeText(process.env.CASIO_AI_OLED_FONT_FILE);
    if (custom && fs.existsSync(custom)) return custom;

    const cjkFallback = path.resolve(
        process.cwd(),
        "app/utils/fonts/NotoSansCJKsc-Regular.otf"
    );
    if (fs.existsSync(cjkFallback)) return cjkFallback;

    const unicodeFallback = path.resolve(
        process.cwd(),
        "app/utils/fonts/unifont-17.0.04.otf"
    );
    if (fs.existsSync(unicodeFallback)) return unicodeFallback;
    return undefined;
}

function escapeXml(text: string): string {
    return text
        .replace(/&/g, "&amp;")
        .replace(/</g, "&lt;")
        .replace(/>/g, "&gt;")
        .replace(/"/g, "&quot;")
        .replace(/'/g, "&apos;");
}

function fontFaceCss(fontFile?: string): string {
    if (!fontFile) return "";
    const fileUrl = `file:///${fontFile.replace(/\\/g, "/")}`;
    return `@font-face{font-family:'CasioOledFont';src:url('${fileUrl}') format('opentype');}`;
}

function extractSvgRoot(markup: string): string {
    const source = safeText(markup);
    if (!source) return "";

    const start = source.indexOf("<svg");
    const end = source.lastIndexOf("</svg>");
    if (start < 0 || end < 0 || end <= start) return "";

    let svgOnly = source.slice(start, end + 6);
    if (!svgOnly.includes("xmlns=")) {
        svgOnly = svgOnly.replace(
            "<svg",
            '<svg xmlns="http://www.w3.org/2000/svg"'
        );
    }
    return svgOnly;
}

function packToXbmBase64(
    pixels: Uint8Array,
    width: number,
    height: number,
    channels: number
): string {
    const stride = Math.ceil(width / 8);
    const packed = Buffer.alloc(stride * height, 0);

    for (let y = 0; y < height; y += 1) {
        for (let x = 0; x < width; x += 1) {
            const baseIndex = (y * width + x) * channels;
            const luminance = pixels[baseIndex] ?? 255;
            if (luminance < 128) {
                const byteIndex = y * stride + Math.floor(x / 8);
                packed[byteIndex] |= 1 << (x % 8);
            }
        }
    }

    const onBits = packed.reduce((total, byte) => {
        let value = byte;
        let count = 0;
        while (value > 0) {
            count += value & 1;
            value >>= 1;
        }
        return total + count;
    }, 0);
    const density = onBits / Math.max(1, width * height);
    if (density > 0.5) {
        for (let index = 0; index < packed.length; index += 1) {
            packed[index] ^= 0xff;
        }
    }

    return packed.toString("base64");
}

function stripLatexBraces(value: string): string {
    return value.replace(/[{}]/g, "");
}

function latexToOledMathText(latex: string): string {
    let output = safeText(latex);

    output = output.replace(/\\boxed\s*\{([^{}]+)\}/g, "$1");
    output = output.replace(
        /\\frac\s*\{([^{}]+)\}\s*\{([^{}]+)\}/g,
        "($1)/($2)"
    );
    output = output.replace(/\\sqrt\s*\{([^{}]+)\}/g, "√($1)");
    output = output.replace(
        /\\sum_\{([^{}]+)\}\^\{([^{}]+)\}/g,
        "Σ $1..$2"
    );
    output = output.replace(
        /\\prod_\{([^{}]+)\}\^\{([^{}]+)\}/g,
        "Π $1..$2"
    );
    output = output.replace(/\\binom\s*\{([^{}]+)\}\s*\{([^{}]+)\}/g, "C($1,$2)");
    output = output.replace(/C_\{?([^{}\s^]+)\}?\^\{?([^{}\s]+)\}?/g, "C($1,$2)");

    output = output
        .replace(/\\omega_c/g, "ωc")
        .replace(/\\Omega_1/g, "Ω1")
        .replace(/\\Omega_2/g, "Ω2")
        .replace(/\\omega/g, "ω")
        .replace(/\\Omega/g, "Ω")
        .replace(/\\alpha/g, "α")
        .replace(/\\beta/g, "β")
        .replace(/\\gamma/g, "γ")
        .replace(/\\theta/g, "θ")
        .replace(/\\pi/g, "π")
        .replace(/\\cos/g, "cos")
        .replace(/\\sin/g, "sin")
        .replace(/\\tan/g, "tan")
        .replace(/\\log/g, "log")
        .replace(/\\ln/g, "ln")
        .replace(/\\exp/g, "exp")
        .replace(/\\max/g, "max")
        .replace(/\\min/g, "min")
        .replace(/\\leq?/g, "≤")
        .replace(/\\geq?/g, "≥")
        .replace(/\\neq/g, "≠")
        .replace(/\\approx/g, "≈")
        .replace(/\\times/g, "×")
        .replace(/\\cdot/g, "·")
        .replace(/\\pm/g, "±")
        .replace(/\\sim/g, "~")
        .replace(/\\text\s*\{([^{}]+)\}/g, "$1")
        .replace(/\\quad/g, " ")
        .replace(/\\,/g, " ")
        .replace(/\\left/g, "")
        .replace(/\\right/g, "")
        .replace(/\\[a-zA-Z]+/g, "")
        .replace(/\s+/g, " ")
        .trim();

    return stripLatexBraces(output);
}

function shouldUseTallFormulaBitmap(latex: string): boolean {
    const source = safeText(latex);
    return (
        source.length > 80 &&
        /\\begin|\\matrix|\\cases|\\overbrace|\\underbrace/u.test(source)
    );
}

async function rasterSingleLineTextToXbm(args: {
    text: string;
    kind: "text" | "formula";
    targetHeight: number;
}): Promise<CasioDisplayBlock | null> {
    const lineText = safeText(args.text);
    if (!lineText) return null;

    const fontFile = resolveFontFile();
    const textInput: sharp.CreateText = {
        text: escapeXml(lineText),
        font: DEFAULT_TEXT_FONT,
        height: args.targetHeight,
        align: "left",
        rgba: false
    };

    if (fontFile) {
        textInput.fontfile = fontFile;
    }

    const rendered = await sharp({ text: textInput })
        .flatten({ background: "#ffffff" })
        .grayscale()
        .trim({ background: "#ffffff" })
        .extend({
            top: 0,
            bottom: 0,
            left: 0,
            right: 1,
            background: "#ffffff"
        })
        .raw()
        .toBuffer({ resolveWithObject: true });

    const bitmapWidth = Math.ceil(Math.max(8, rendered.info.width) / 8) * 8;
    let { data, info } = await sharp(rendered.data, {
        raw: {
            width: rendered.info.width,
            height: rendered.info.height,
            channels: rendered.info.channels
        }
    })
        .extend({
            top: Math.max(0, Math.floor((OLED_HEIGHT - rendered.info.height) / 2)),
            bottom: Math.max(
                0,
                OLED_HEIGHT -
                    rendered.info.height -
                    Math.max(0, Math.floor((OLED_HEIGHT - rendered.info.height) / 2))
            ),
            left: 0,
            right: Math.max(0, bitmapWidth - rendered.info.width),
            background: "#ffffff"
        })
        .raw()
        .toBuffer({ resolveWithObject: true });

    if (info.height !== OLED_HEIGHT) {
        const normalized = await sharp(data, {
            raw: {
                width: info.width,
                height: info.height,
                channels: info.channels
            }
        })
            .resize({
                height: OLED_HEIGHT,
                fit: "contain",
                position: "left top",
                background: "#ffffff",
                withoutEnlargement: false
            })
            .raw()
            .toBuffer({ resolveWithObject: true });
        data = normalized.data;
        info = normalized.info;
    }

    return {
        type: "bitmap",
        kind: args.kind,
        width: info.width,
        height: info.height,
        format: "1bit_xbm",
        data: packToXbmBase64(data, info.width, info.height, info.channels)
    };
}

async function rasterFallbackFormulaText(latex: string): Promise<CasioDisplayBlock[]> {
    const textBitmap = await rasterSingleLineTextToXbm({
        text: latexToOledMathText(latex),
        kind: "formula",
        targetHeight: FORMULA_TEXT_HEIGHT
    });
    return textBitmap ? [textBitmap] : [];
}

async function rasterFormulaTextPages(latex: string): Promise<CasioDisplayBlock[]> {
    const pages = splitTextIntoPages(latexToOledMathText(latex));
    if (!pages.length) return [];

    const rendered = await Promise.all(
        pages.map((page) => rasterTextToXbm(page, "formula"))
    );

    return rendered.filter(
        (item): item is CasioDisplayBlock =>
            item !== null && item.width > 0 && item.height > 0
    );
}

async function rasterFormulaToXbm(latex: string): Promise<CasioDisplayBlock[]> {
    if (!RENDER_LATEX_BITMAPS) {
        return rasterFormulaTextPages(latex);
    }

    let svgMarkup = "";
    try {
        const node = mathDocument.convert(latex, { display: true });
        const rawMarkup = adaptor.outerHTML(node);
        svgMarkup = extractSvgRoot(rawMarkup);
    } catch {
        // fallback below
    }

    if (!svgMarkup) {
        const escaped = escapeXml(latex);
        svgMarkup = `<svg xmlns="http://www.w3.org/2000/svg" width="128" height="32" viewBox="0 0 128 32">
<rect x="0" y="0" width="128" height="32" fill="white"/>
<text x="0" y="20" font-family="Arial, sans-serif" font-size="14" fill="black">${escaped}</text>
</svg>`;
    }

    if (!svgMarkup) return rasterFallbackFormulaText(latex);

    const targetHeight = shouldUseTallFormulaBitmap(latex)
        ? TALL_FORMULA_HEIGHT
        : OLED_HEIGHT;
    const transformed = sharp(Buffer.from(svgMarkup))
        .flatten({ background: "#ffffff" })
        .grayscale()
        .resize({
            height: targetHeight,
            fit: "inside",
            withoutEnlargement: false
        });

    let { data, info } = await transformed.raw().toBuffer({
        resolveWithObject: true
    });

    const alignedWidth = Math.ceil(info.width / 8) * 8;
    const targetWidth = Math.max(8, alignedWidth);

    if (info.width !== targetWidth || info.height !== targetHeight) {
        const normalized = await sharp(data, {
            raw: {
                width: info.width,
                height: info.height,
                channels: info.channels
            }
        })
            .extend({
                top: 0,
                left: 0,
                right: Math.max(0, targetWidth - info.width),
                bottom: Math.max(0, targetHeight - info.height),
                background: "#ffffff"
            })
            .raw()
            .toBuffer({ resolveWithObject: true });
        data = normalized.data;
        info = normalized.info;
    }

    const sliceCount = Math.max(1, Math.ceil(info.height / OLED_HEIGHT));
    const sliceJobs = Array.from({ length: sliceCount }, async (_, index) => {
        const top = index * OLED_HEIGHT;
        const sliceHeight = Math.min(OLED_HEIGHT, info.height - top);
        const slice = await sharp(data, {
            raw: {
                width: info.width,
                height: info.height,
                channels: info.channels
            }
        })
            .extract({
                left: 0,
                top,
                width: info.width,
                height: sliceHeight
            })
            .extend({
                top: 0,
                left: 0,
                right: 0,
                bottom: Math.max(0, OLED_HEIGHT - sliceHeight),
                background: "#ffffff"
            })
            .raw()
            .toBuffer({ resolveWithObject: true });

        return {
            type: "bitmap",
            kind: "formula",
            width: slice.info.width,
            height: slice.info.height,
            format: "1bit_xbm",
            data: packToXbmBase64(
                slice.data,
                slice.info.width,
                slice.info.height,
                slice.info.channels
            )
        } satisfies CasioDisplayBlock;
    });
    const blocks = await Promise.all(sliceJobs);

    if (blocks.length) return blocks;
    return rasterFallbackFormulaText(latex);
}

async function rasterFormulaLayoutToXbm(
    latex: string,
    targetHeight: 16 | 32 | 64
): Promise<CasioDisplayBlock | null> {
    const source = safeText(latex);
    if (!source) return null;

    let svgMarkup = "";
    try {
        const node = mathDocument.convert(source, { display: true });
        const rawMarkup = adaptor.outerHTML(node);
        svgMarkup = extractSvgRoot(rawMarkup);
    } catch {
        // fallback below
    }

    if (!svgMarkup) {
        const escaped = escapeXml(latexToOledMathText(source) || source);
        svgMarkup = `<svg xmlns="http://www.w3.org/2000/svg" width="128" height="${targetHeight}" viewBox="0 0 128 ${targetHeight}">
<rect x="0" y="0" width="128" height="${targetHeight}" fill="white"/>
<text x="0" y="${Math.max(12, targetHeight - 4)}" font-family="Arial, sans-serif" font-size="${Math.max(10, Math.min(18, targetHeight - 4))}" fill="black">${escaped}</text>
</svg>`;
    }

    let { data, info } = await sharp(Buffer.from(svgMarkup))
        .flatten({ background: "#ffffff" })
        .grayscale()
        .resize({
            height: targetHeight,
            fit: "inside",
            withoutEnlargement: false
        })
        .raw()
        .toBuffer({ resolveWithObject: true });

    const targetWidth = Math.max(8, Math.ceil(info.width / 8) * 8);
    if (info.width !== targetWidth || info.height !== targetHeight) {
        const normalized = await sharp(data, {
            raw: {
                width: info.width,
                height: info.height,
                channels: info.channels
            }
        })
            .extend({
                top: 0,
                left: 0,
                right: Math.max(0, targetWidth - info.width),
                bottom: Math.max(0, targetHeight - info.height),
                background: "#ffffff"
            })
            .raw()
            .toBuffer({ resolveWithObject: true });
        data = normalized.data;
        info = normalized.info;
    }

    return {
        type: "bitmap",
        kind: "formula",
        width: info.width,
        height: info.height,
        format: "1bit_xbm",
        data: packToXbmBase64(data, info.width, info.height, info.channels)
    };
}

async function rasterTextLine(text: string): Promise<{
    data: Buffer;
    width: number;
    height: number;
} | null> {
    const lineText = safeText(text);
    if (!lineText) return null;

    const fontFile = resolveFontFile();
    const textInput: sharp.CreateText = {
        text: escapeXml(lineText),
        font: DEFAULT_TEXT_FONT,
        height: TEXT_ROW_HEIGHT,
        align: "left",
        rgba: false
    };

    if (fontFile) {
        textInput.fontfile = fontFile;
    }

    const rendered = await sharp({ text: textInput })
        .flatten({ background: "#ffffff" })
        .grayscale()
        .trim({ background: "#ffffff" })
        .extend({
            top: 0,
            bottom: 0,
            left: 0,
            right: 1,
            background: "#ffffff"
        })
        .resize({
            height: MIN_TEXT_LINE_HEIGHT,
            fit: "inside",
            withoutEnlargement: false
        })
        .png()
        .toBuffer({ resolveWithObject: true });

    return {
        data: Buffer.from(rendered.data),
        width: rendered.info.width,
        height: rendered.info.height
    };
}

async function rasterTextPageSvgToXbm(
    lines: string[],
    kind: "text" | "formula",
    fixedWidth: boolean
): Promise<CasioDisplayBlock | null> {
    const cleanLines = lines.map((line) => safeText(line)).filter(Boolean);
    if (!cleanLines.length) return null;

    const fontFile = resolveFontFile();
    const fontFamily = fontFile
        ? "CasioOledFont"
        : `${DEFAULT_TEXT_FONT}, Arial, sans-serif`;
    const estimatedWidths = cleanLines.map((line) => {
        const units = countVisualUnits(line);
        return Math.ceil(units * (TEXT_FONT_SIZE / 2 + 1));
    });
    const bitmapWidth = fixedWidth
        ? OLED_WIDTH
        : Math.ceil(Math.max(OLED_WIDTH, ...estimatedWidths) / 8) * 8;
    const yPositions =
        cleanLines.length === 1
            ? [Math.floor(OLED_HEIGHT / 2 + TEXT_FONT_SIZE / 2 - 1)]
            : [15, 31];
    const textNodes = cleanLines
        .slice(0, TEXT_LINES_PER_PAGE)
        .map(
            (line, index) =>
                `<text x="0" y="${yPositions[index]}" font-size="${TEXT_FONT_SIZE}" font-family="${fontFamily}" fill="black">${escapeXml(line)}</text>`
        )
        .join("");
    const svgMarkup = `<svg xmlns="http://www.w3.org/2000/svg" width="${bitmapWidth}" height="${OLED_HEIGHT}" viewBox="0 0 ${bitmapWidth} ${OLED_HEIGHT}">
<style>${fontFaceCss(fontFile)}</style>
<rect x="0" y="0" width="${bitmapWidth}" height="${OLED_HEIGHT}" fill="white"/>
${textNodes}
</svg>`;

    const { data, info } = await sharp(Buffer.from(svgMarkup))
        .flatten({ background: "#ffffff" })
        .grayscale()
        .raw()
        .toBuffer({ resolveWithObject: true });

    if (info.width <= 0 || info.height <= 0) return null;

    return {
        type: "bitmap",
        kind,
        width: info.width,
        height: info.height,
        format: "1bit_xbm",
        data: packToXbmBase64(data, info.width, info.height, info.channels)
    };
}

async function fitTextLineForCanvas(
    line: {
        data: Buffer;
        width: number;
        height: number;
    },
    canvasWidth: number
): Promise<{
    data: Buffer;
    width: number;
    height: number;
}> {
    if (line.width <= canvasWidth) return line;

    const cropped = await sharp(line.data)
        .extract({
            left: 0,
            top: 0,
            width: canvasWidth,
            height: line.height
        })
        .png()
        .toBuffer({ resolveWithObject: true });

    return {
        data: Buffer.from(cropped.data),
        width: cropped.info.width,
        height: cropped.info.height
    };
}

async function rasterTextToXbm(
    text: string,
    kind: "text" | "formula" = "text",
    fixedWidth = false
): Promise<CasioDisplayBlock | null> {
    const pageText = safeText(text);
    if (!pageText) return null;

    const lines = pageText
        .split("\n")
        .map((line) => line.trim())
        .filter(Boolean)
        .slice(0, TEXT_LINES_PER_PAGE);

    const svgRendered = await rasterTextPageSvgToXbm(lines, kind, fixedWidth);
    if (svgRendered) return svgRendered;

    const renderedLines = await Promise.all(lines.map((line) => rasterTextLine(line)));
    const bitmapWidth = fixedWidth
        ? OLED_WIDTH
        : Math.ceil(
              Math.max(
                  OLED_WIDTH,
                  ...renderedLines.map((line) => line?.width || 0)
              ) / 8
          ) * 8;
    const fittedLines = await Promise.all(
        renderedLines.map((line) =>
            line ? fitTextLineForCanvas(line, bitmapWidth) : null
        )
    );
    const canvas = sharp({
        create: {
            width: bitmapWidth,
            height: OLED_HEIGHT,
            channels: 3,
            background: { r: 255, g: 255, b: 255 }
        }
    });

    const composites: OverlayOptions[] = fittedLines
        .map((line, index) => {
            if (!line) return null;
            return {
                input: line.data,
                left: 0,
                top:
                    lines.length === 1
                        ? Math.max(0, Math.floor((OLED_HEIGHT - line.height) / 2))
                        : index * TEXT_ROW_HEIGHT
            };
        })
        .filter((item): item is NonNullable<typeof item> => item !== null);

    const { data, info } = await canvas
        .composite(composites)
        .grayscale()
        .raw()
        .toBuffer({ resolveWithObject: true });

    if (info.width <= 0 || info.height <= 0) return null;

    return {
        type: "bitmap",
        kind,
        width: info.width,
        height: info.height,
        format: "1bit_xbm",
        data: packToXbmBase64(data, info.width, info.height, info.channels)
    };
}

function normalizeOledPageKind(value: unknown): "text" | "formula" {
    return safeText(value).toLowerCase() === "formula" ? "formula" : "text";
}

function normalizeOledPageLines(page: CasioOledPage): string[] {
    const lines = Array.isArray(page.lines)
        ? page.lines.map((line) => safeText(line)).filter(Boolean)
        : [];
    if (lines.length) return lines;

    const text = safeText(page.text);
    if (!text) return [];
    return text
        .replace(/\r/g, "")
        .split("\n")
        .map((line) => line.trim())
        .filter(Boolean);
}

function wrapOledFallbackLines(lines: string[]): string[] {
    return lines.flatMap((line) => wrapLineByUnits(line, OLED_FIXED_PAGE_UNIT_LIMIT));
}

export async function buildDisplayBlocksFromOledPages(
    pages: unknown
): Promise<CasioDisplayBlock[]> {
    const startedAt = Date.now();
    if (!Array.isArray(pages)) return [];

    const renderJobs: Array<Promise<CasioDisplayBlock | null>> = [];
    for (const rawPage of pages) {
        if (rawPage && typeof rawPage === "object") {
            const page = rawPage as CasioOledPage;
            const kind = normalizeOledPageKind(page.kind);
            const lines = wrapOledFallbackLines(normalizeOledPageLines(page));
            for (
                let index = 0;
                index < lines.length;
                index += TEXT_LINES_PER_PAGE
            ) {
                const chunk = lines.slice(index, index + TEXT_LINES_PER_PAGE);
                if (chunk.length) {
                    renderJobs.push(rasterTextToXbm(chunk.join("\n"), kind, true));
                }
            }
        }
    }

    const rendered = await Promise.all(renderJobs);
    const output = rendered.filter(
        (item): item is CasioDisplayBlock =>
            item !== null &&
            item.width === OLED_WIDTH &&
            item.height === OLED_HEIGHT
    );

    console.log(`${LOG_PREFIX} oled_pages_render`, {
        ms: Date.now() - startedAt,
        inputPages: pages.length,
        outputBlocks: output.length,
        bytes: output.reduce(
            (total, block) => total + Buffer.byteLength(block.data, "base64"),
            0
        )
    });

    return output;
}

function normalizeLayoutHeight(
    value: unknown,
    fallback: 16 | 32 | 64
): 16 | 32 | 64 {
    const height = Number(value);
    if (height === 16 || height === 32 || height === 64) return height;
    return fallback;
}

function normalizeLayoutLines(value: unknown): string[] {
    if (!Array.isArray(value)) return [];
    return value
        .map((line) => safeText(line))
        .filter(Boolean)
        .slice(0, TEXT_LINES_PER_PAGE);
}

function displayTextFromLayout(layout: unknown): string {
    if (!Array.isArray(layout)) return "";
    const lines: string[] = [];
    for (const rawBlock of layout) {
        if (rawBlock && typeof rawBlock === "object") {
            const block = rawBlock as CasioOledLayoutBlock;
            const type = safeText(block.type).toLowerCase();
            if (type === "text") {
                lines.push(...normalizeLayoutLines(block.lines));
            } else if (type === "formula") {
                const latex = safeText(block.latex);
                if (latex) lines.push(latexToOledMathText(latex));
            }
        }
        if (lines.join("\n").length >= 220) break;
    }
    return lines.join("\n");
}

export function getDisplayTextFromOledLayout(layout: unknown): string {
    return displayTextFromLayout(layout);
}

export async function buildDisplayBlocksFromOledLayout(
    layout: unknown
): Promise<CasioDisplayBlock[]> {
    const startedAt = Date.now();
    if (!Array.isArray(layout)) return [];

    const renderJobs: Array<Promise<CasioDisplayBlock | null>> = [];
    let acceptedBlocks = 0;

    for (const rawBlock of layout) {
        if (rawBlock && typeof rawBlock === "object") {
            const block = rawBlock as CasioOledLayoutBlock;
            const blockType = safeText(block.type).toLowerCase();

            if (blockType === "text") {
                const lines = normalizeLayoutLines(block.lines);
                if (lines.length) {
                    acceptedBlocks += 1;
                    renderJobs.push(rasterTextToXbm(lines.join("\n"), "text", true));
                }
            } else if (blockType === "formula") {
                const latex = safeText(block.latex);
                if (latex) {
                    acceptedBlocks += 1;
                    renderJobs.push(
                        rasterFormulaLayoutToXbm(
                            latex,
                            normalizeLayoutHeight(block.height, OLED_HEIGHT)
                        )
                    );
                }
            }
        }
    }

    const rendered = await Promise.all(renderJobs);
    const output = rendered.filter(
        (item): item is CasioDisplayBlock =>
            item !== null &&
            item.width > 0 &&
            (item.height === 16 ||
                item.height === OLED_HEIGHT ||
                item.height === TALL_FORMULA_HEIGHT)
    );

    console.log(`${LOG_PREFIX} oled_layout_render`, {
        ms: Date.now() - startedAt,
        inputBlocks: layout.length,
        acceptedBlocks,
        outputBlocks: output.length,
        bytes: output.reduce(
            (total, block) => total + Buffer.byteLength(block.data, "base64"),
            0
        ),
        maxWidth: output.reduce((max, block) => Math.max(max, block.width), 0),
        maxHeight: output.reduce((max, block) => Math.max(max, block.height), 0)
    });

    return output;
}

function collectInputBlocks(rawBlocks: unknown): RawModelBlock[] {
    if (!Array.isArray(rawBlocks)) return [];
    const output: RawModelBlock[] = [];

    for (const item of rawBlocks) {
        if (item && typeof item === "object") {
            const rawType = safeText((item as RawModelBlock).type).toLowerCase();
            if (rawType === "formula") {
                const latex = safeText((item as RawModelBlock).latex);
                if (latex) output.push({ type: "formula", latex });
            } else if (rawType === "text") {
                const text = safeText((item as RawModelBlock).text);
                if (text) output.push({ type: "text", text });
            }
        }
    }

    return output;
}

function normalizeTransitionLabel(text: string): string {
    return safeText(text).replace(/[：:]\s*$/u, "");
}

function isTransitionOnlyText(text: string): boolean {
    const normalized = normalizeTransitionLabel(text);
    const knownTransition = [
        "即",
        "其中",
        "所以",
        "题意要求",
        "经计算",
        "逐个试算",
        "因此",
        "则",
        "且",
        "因为",
        "要求",
        "条件",
        "可得"
    ].includes(normalized);

    return (
        knownTransition ||
        (/[:：]\s*$/u.test(safeText(text)) && countVisualUnits(normalized) <= 16)
    );
}

function lineCount(text: string): number {
    return safeText(text)
        .split("\n")
        .map((line) => line.trim())
        .filter(Boolean).length;
}

function prepareInputBlocks(rawBlocks: unknown): RawModelBlock[] {
    const blocks = collectInputBlocks(rawBlocks);
    const prepared: RawModelBlock[] = [];

    for (let index = 0; index < blocks.length; index += 1) {
        const current = blocks[index];
        const next = blocks[index + 1];

        if (
            current.type === "text" &&
            isTransitionOnlyText(safeText(current.text)) &&
            next?.type === "formula"
        ) {
            const formulaText = latexToOledMathText(safeText(next.latex));
            prepared.push({
                type: "text",
                text: `${normalizeTransitionLabel(safeText(current.text))}：\n${formulaText}`
            });
            index += 1;
        } else if (
            current.type === "text" &&
            isTransitionOnlyText(safeText(current.text)) &&
            next?.type === "text"
        ) {
            prepared.push({
                type: "text",
                text: `${safeText(current.text)}\n${safeText(next.text)}`
            });
            index += 1;
        } else if (
            current.type === "text" &&
            next?.type === "text" &&
            lineCount(safeText(current.text)) === 1 &&
            lineCount(safeText(next.text)) === 1
        ) {
            prepared.push({
                type: "text",
                text: `${safeText(current.text)}\n${safeText(next.text)}`
            });
            index += 1;
        } else {
            prepared.push(current);
        }
    }

    return prepared;
}

export async function buildDisplayBlocksFromModel(args: {
    blocks?: unknown;
    answer?: string;
}): Promise<CasioDisplayBlock[]> {
    const startedAt = Date.now();
    const parsedBlocks = prepareInputBlocks(args.blocks);
    if (!parsedBlocks.length) {
        const fallbackPages = splitTextIntoPages(safeText(args.answer));
        if (!fallbackPages.length) return [];
        const fallbackRendered = await Promise.all(
            fallbackPages.map((page) => rasterTextToXbm(page))
        );
        const output = fallbackRendered.filter(
            (item): item is CasioDisplayBlock =>
                item !== null && item.width > 0 && item.height > 0
        );
        console.log(`${LOG_PREFIX} fallback_render`, {
            ms: Date.now() - startedAt,
            pages: fallbackPages.length,
            outputBlocks: output.length,
            bytes: output.reduce(
                (total, block) => total + Buffer.byteLength(block.data, "base64"),
                0
            ),
            maxWidth: output.reduce((max, block) => Math.max(max, block.width), 0)
        });
        return output;
    }

    const renderJobs: Array<Promise<Array<CasioDisplayBlock | null>>> = [];
    for (const block of parsedBlocks) {
        if (block.type === "formula") {
            renderJobs.push(rasterFormulaToXbm(safeText(block.latex)));
        } else {
            const textPages = splitTextIntoPages(safeText(block.text));
            for (const page of textPages) {
                renderJobs.push(rasterTextToXbm(page, "text").then((item) => [item]));
            }
        }
    }

    const rendered = (await Promise.all(renderJobs)).flat();
    const output = rendered.filter(
        (item): item is CasioDisplayBlock =>
            item !== null && item.width > 0 && item.height > 0
    );
    console.log(`${LOG_PREFIX} model_render`, {
        ms: Date.now() - startedAt,
        inputBlocks: Array.isArray(args.blocks) ? args.blocks.length : 0,
        preparedBlocks: parsedBlocks.length,
        renderJobs: renderJobs.length,
        outputBlocks: output.length,
        bytes: output.reduce(
            (total, block) => total + Buffer.byteLength(block.data, "base64"),
            0
        ),
        maxWidth: output.reduce((max, block) => Math.max(max, block.width), 0)
    });
    return output;
}
