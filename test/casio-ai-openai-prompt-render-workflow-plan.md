# Casio AI OpenAI Prompt + Server Renderer Workflow Plan

## Goal

把 Casio AI solve workflow 改成：

```text
ESP32 upload photos
→ EBC server creates async question job
→ OpenAI Prompt 1 solves the exam/homework question
→ OpenAI Prompt 2 converts answer into OLED layout JSON
→ EBC server deterministically renders layout to 1-bit XBM blocks
→ ESP32 polls and displays bitmap blocks
```

这次不再让 AI 直接生成 bitmap/base64。AI 只负责两件事：

1. 第一轮：认真解题，输出完整答案。
2. 第二轮：根据完整答案做 OLED 分块 layout。

服务器只做机械渲染：

```text
oled_layout JSON → text/formula bitmap → display_blocks
```

## Why Change

之前让第二轮 AI 直接生成 1-bit bitmap，测试中出现：

- 字体不稳定。
- 黑白背景反复反相。
- 空白处有时全白有时全黑。
- 部分答案显示不完整。
- AI 很难稳定遵守 XBM byte packing。

新的方案保留 AI 擅长的语义分块能力，把像素级工作交给 TypeScript renderer。

## OpenAI Prompt IDs

第一轮解题 Prompt：

```text
id: pmpt_6a106f668b9c8190a9ffaac9fc0b7198025cea817cae6af5
version: 3
```

第二轮 OLED layout Prompt：

```text
id: pmpt_6a10657544288194bd83399ed179c8ce0ff0947071024441
version: 6
```

服务器代码提供 env override，但默认使用以上配置：

```text
CASIO_AI_OPENAI_SOLVE_PROMPT_ID
CASIO_AI_OPENAI_SOLVE_PROMPT_VERSION
CASIO_AI_OPENAI_LAYOUT_PROMPT_ID
CASIO_AI_OPENAI_LAYOUT_PROMPT_VERSION
```

重要：第二轮优先用 `prompt.variables.answer_full`。如果 OpenAI Platform
Prompt 变量配置不匹配并返回 `Unknown prompt variables: answer_full`，
服务器再 fallback 成普通 user input：

```text
answer_full:
...
```

## Data Contracts

### First OpenAI Result

第一轮 Prompt 输出完整解题答案。服务器接受以下字段之一：

```json
{
  "answer_full": "..."
}
```

兼容字段：

```text
answer
full_answer
raw text
```

### Second OpenAI Result

第二轮 Prompt 输出严格 JSON：

```json
{
  "oled_layout": [
    {
      "type": "text",
      "height": 32,
      "lines": ["第一行", "第二行"],
      "latex": null
    },
    {
      "type": "formula",
      "height": 32,
      "lines": null,
      "latex": "x=\\frac{-b\\pm\\sqrt{b^2-4ac}}{2a}"
    }
  ]
}
```

规则：

- `text` block 必须 `height=32`，`lines` 为 1-2 行。
- `formula` block 可用 `height=16 | 32 | 64`，`latex` 必须存在。
- 第二轮不输出 bitmap，不输出 base64。

### Server Output To ESP32

继续使用现有 `display_blocks` 协议：

```json
{
  "type": "bitmap",
  "kind": "text",
  "width": 128,
  "height": 32,
  "format": "1bit_xbm",
  "data": "base64..."
}
```

公式 block 可返回：

```json
{
  "type": "bitmap",
  "kind": "formula",
  "width": 128,
  "height": 16,
  "format": "1bit_xbm",
  "data": "base64..."
}
```

或宽度超过 `128`，或高度 `64`。ESP32 负责左右/上下 viewport 移动。

## Server Implementation Plan

### 1. Add Prompt-Based OpenAI Calls

在 `EstateBoostCopilot/general/app/utils/casioAi.ts`：

- 新增 prompt id/version resolver。
- 新增 Responses API prompt call helper。
- 第一轮 `solveCasioAnswerWithOpenAi` 改用 OpenAI Prompt 1。
- 第二轮 `renderCasioAnswerForOledWithOpenAi` 改用 OpenAI Prompt 2，并把完整答案作为 user input 传入。
- 删除正式路径里的 AI direct bitmap 生成；旧函数只保留为非主线 fallback 或 dead-path。

### 2. Add OLED Layout Renderer

在 `EstateBoostCopilot/general/app/utils/casioDisplay.ts`：

- 新增 `CasioOledLayoutBlock` type。
- 新增 `buildDisplayBlocksFromOledLayout(oled_layout)`。
- `text` block：按 `lines` 渲染成 128×32 text bitmap。
- `formula` block：按 `height=16/32/64` 渲染 LaTeX，不再强行切成 32px 两页。
- renderer 输出保持 XBM packed bytes + base64。

### 3. Keep Async Communication Stable

现有 async workflow 保持：

```text
/solve → 202 PROCESSING
/question → ANSWER_READY triggers render
/question → 202 RENDERING
/question → 200 SUCCEEDED
/question-block → chunked block fetch
```

这能避免 ESP32 长连接硬等两轮 AI。

### 4. Update ESP32 Bitmap Height Support

在 `Casio-AI-Machine/src/main.cpp` 和 `src/ai_solve_test.cpp`：

- `display_blocks` parser 接受 `height=16/32/64`。
- byte length 使用 `ceil(width/8) * height`。
- bitmap draw 支持 `yOffset`。
- UP/DOWN 对 64px block 先上下移动 viewport，再切 block。
- LEFT/RIGHT 继续横向移动宽 block。

## Fallback Strategy

如果第二轮 layout invalid 或为空：

1. 尝试从 `answer_full` 用 server fallback renderer 生成 text/formula blocks。
2. 不再回到 AI direct bitmap 作为主路径。
3. logs 必须标明 source：

```text
openai_prompt_solve
openai_prompt_layout
server_oled_layout_render
server_emergency_fallback
```

## Validation

完成后检查：

- TypeScript lint / compile 不出现 Casio 相关错误。
- `ai_solve_test` 编译通过。
- `main_system` 编译通过。
- 日志中应看到：

```text
openai_prompt_solve_done
openai_prompt_layout_done
oled_layout_render
```

并且不再依赖：

```text
oled_bitmap_ai_validated
ai_direct_bitmap
```
