我问 : okk 那么我们的 新的架构里面的 ai 部分完成了, 我们变成问 ai 第一轮解题, 第二轮 做好分块oled 动作, 然后打算进去服务器做render. 那么ai 做好了, 我这里有两份文件是暂时旧的架构 旧的workflow 旧的prompt 等的文件, 里面也残留了一些render的方式和痕迹, 以我们现在做好了ai 来看 这个render 能适配我们的 新的workflow , json等吗? 这个render 本身有问题吗? 这个render 有什么根本逻辑上是不完善啥的, 我想给我的服务器的worker 知道自己大概有啥问题 看需不需要改

gpt5.5 thinking 回复 :
结论：**现在这套 render 不能直接适配新 workflow，需要改。**
但里面有不少可复用的底层能力：字体渲染、Sharp、MathJax、XBM packing、base64 输出这些可以保留。

## 现在最大问题

### 1. 旧 workflow 还在让 AI 直接生成 bitmap

`casioAi.ts` 里的第二阶段还是 `buildOledBitmapPrompt()`，它要求 AI 直接输出 `display_blocks` 的 1-bit XBM base64。这个已经和我们新架构冲突了。新架构应该是：

```text
AI 第二轮：只输出 oled_layout JSON
Server renderer：把 oled_layout 渲染成 bitmap
ESP：只收 bitmap
```

但旧代码仍写着让 AI 做 “1-bit bitmap generator”，并要求每个 block 固定 `width=128,height=32,data=base64`。

### 2. 旧 renderer 不认识新的 JSON Schema block

新 schema 的 block 是：

```json
{
  "type": "text",
  "height": 32,
  "lines": ["...", "..."],
  "latex": null
}
```

或：

```json
{
  "type": "formula",
  "height": 32,
  "lines": null,
  "latex": "..."
}
```

但旧 `collectInputBlocks()` 只认：

```ts
{ type: "text", text: "..." }
{ type: "formula", latex: "..." }
```

它不会读取 `lines`，所以新 structured output 直接丢进去会导致 text block 基本失效。

### 3. `renderCasioAnswerForOledWithOpenAi()` 还是旧逻辑

现在它先调用 `generateOledBitmapsWithOpenAi()`，失败后才 fallback 到 `buildDisplayBlocksFromModel()`。这代表它还把 AI bitmap 生成当主路径，而不是把 server renderer 当主路径。

新逻辑应该改成：

```text
answer_full
→ call AI second pass with JSON Schema
→ get oled_layout
→ server render oled_layout
→ output display blocks
```

---

## render 本身的问题

### 1. LaTeX 真渲染默认可能没开

`rasterFormulaToXbm()` 里如果 `CASIO_AI_RENDER_LATEX_BITMAPS` 不是 `"true"`，它不会走 MathJax SVG 真渲染，而是走 `latexToOledMathText()`，把公式转换成近似文本。

也就是说，如果你想要“像电脑/手机那样的公式”，必须：

```env
CASIO_AI_RENDER_LATEX_BITMAPS=true
```

否则它显示的是：

```text
frac → (a)/(b)
sum → Σ...
```

不是漂亮公式。

### 2. 公式高度没有真正按新 schema 使用

新 schema 里公式有：

```text
height = 16 / 32 / 64
```

但旧 `rasterFormulaToXbm()` 主要根据公式内容自己判断 `targetHeight` 是 `32` 还是 `64`，不是使用 AI 给的 height。

要改成：

```ts
rasterFormulaToXbm(latex, height)
```

然后严格按 schema：

```text
formula height=16 → render 16px 高 bitmap
formula height=32 → render 32px 高 bitmap
formula height=64 → render 64px 高 bitmap
```

### 3. 64px 公式现在会被切成多个 32px blocks

旧代码会把高于 32 的公式 slice 成多个 32px block。

但我们新设计是：

```text
一个 formula block 可以 height=64
ESP 用上下 scroll 看完整 block
```

所以不要把 64px 公式切成两个 block。应该输出：

```json
["f", width, 64, "base64..."]
```

而不是两个 32px block。

### 4. `buildDisplayBlocksFromOledPages()` 是旧 fallback，不适合新 schema

它读的是旧结构：

```ts
{ kind, lines, text }
```

然后强制输出 `width=128,height=32` 的 blocks。

新 schema 没有 `kind`，而是 `type`；formula 也不该被当普通 lines 渲染。

---

## 需要 worker 改的核心点

### A. 删除 / 停用 AI 直接生成 bitmap

停用这个旧主路径：

```text
generateOledBitmapsWithOpenAi()
buildOledBitmapPrompt()
AI direct display_blocks
```

新第二轮 AI 应该只生成：

```json
{
  "oled_layout": [...]
}
```

不要让 AI 输出 bitmap/base64。

---

### B. 新增 `buildDisplayBlocksFromOledLayout()`

输入：

```ts
type OledLayoutBlock =
  | {
      type: "text";
      height: 32;
      lines: string[];
      latex: null;
    }
  | {
      type: "formula";
      height: 16 | 32 | 64;
      lines: null;
      latex: string;
    };
```

输出内部 display blocks：

```ts
{
  type: "bitmap";
  kind: "text" | "formula";
  width: number;
  height: number;
  format: "1bit_xbm";
  data: string;
}
```

逻辑：

```text
text block:
  lines → render 128×32

formula block:
  latex + height → MathJax/SVG → sharp → 1bit_xbm
  width 自动计算，可 >128
  height 使用 schema 的 16/32/64
```

---

### C. Text renderer 可以保留，但要适配 `lines`

现在文字渲染函数 `rasterTextToXbm()` 基本可复用。它会画 1–2 行，并 pack 成 XBM。
但新入口不要再用 `text` 字段，而是：

```ts
block.lines.join("\n")
```

---

### D. Formula renderer 要强制走 MathJax

如果目标是漂亮数学公式：

```text
CASIO_AI_RENDER_LATEX_BITMAPS=true
```

并且建议不要再 fallback 到 `latexToOledMathText()` 作为正常路径。fallback 可以保留在 MathJax 失败时用。

---

### E. Server → ESP 输出要改成极简格式

现在内部 `CasioDisplayBlock` 很详细：

```json
{
  "type": "bitmap",
  "kind": "formula",
  "width": 260,
  "height": 32,
  "format": "1bit_xbm",
  "data": "..."
}
```

给 ESP 建议压缩成：

```json
{
  "ok": true,
  "b": [
    ["t", 128, 32, "..."],
    ["f", 260, 32, "..."],
    ["f", 180, 64, "..."]
  ]
}
```

ESP 默认：

```text
format = 1bit_xbm
1 = OLED pixel ON
0 = background OFF
```

---

## 这套旧 renderer 可复用的部分

可以保留：

```text
sharp 渲染文字
MathJax → SVG
SVG → raw pixels
XBM LSB-first packing
base64 输出
Noto Sans CJK font fallback
颜色 polarity normalize
```

尤其 `packToXbmBase64()` 这类打包逻辑可以继续用。它当前是按黑字白底渲染后，把黑色像素设为 on bit，并且有 density 检测防止黑白反转，方向是对的。

---

## 最关键的改造清单

给 worker 可以这样说：

```text
1. 不再让 AI 生成 display_blocks/base64。
2. 第二轮 AI 用 JSON Schema 只输出 oled_layout。
3. 新建 buildDisplayBlocksFromOledLayout(oled_layout)。
4. renderer 支持新 schema：type,height,lines,latex。
5. text block: lines → 128×32 bitmap。
6. formula block: latex + height(16/32/64) → MathJax SVG → bitmap。
7. formula width 自动计算，可以 >128。
8. height=64 不要切成两个 block，保留一个 64px 高 bitmap。
9. Server 给 ESP 输出 {ok:true,b:[[kind,w,h,data],...]}。
10. 旧 buildOledBitmapPrompt / generateOledBitmapsWithOpenAi 主路径删除或只保留 debug，不再用于正式流程。
```

## 判断

```text
旧 renderer 底层能力：可以复用
旧 workflow：不适配
最大缺口：没有新 oled_layout schema → bitmap 的正式 renderer
根本逻辑问题：还在让 AI 做 bitmap，而不是 server 做 bitmap
```

所以不是“render 完全不能用”，而是要把它从 **AI bitmap generator fallback** 改成 **server-side deterministic renderer**。
