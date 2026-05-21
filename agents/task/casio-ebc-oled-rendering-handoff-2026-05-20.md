# Casio x EBC OLED Rendering Refactor Handoff (2026-05-20)

## 1. 背景与问题

本次改动涉及两个仓库：

- `Casio-AI-Machine`（ESP32 device）
- `EstateBoostCopilot/general`（server/backend）

原本流程是：

- ESP32 拍照并上传
- `/api/casio-ai/solve` 调 OpenAI
- 返回 `display_text`（纯文本）给设备

核心问题：

- OLED 只有 `128x32`，纯文本方案无法稳定承载复杂公式和混合排版
- ESP32 不适合做 Markdown/LaTeX 解析与排版

目标改成：

- 服务器负责把回答转成 OLED 可直接显示的 `1-bit bitmap blocks`
- ESP32 只做解码、绘制和滚动

---

## 2. 本次方案（最终落地）

### 2.1 服务器职责

1. 调 OpenAI，要求结构化返回：
   - `answer_full`
   - `blocks`（`text` / `formula`）
2. 服务器将 `blocks` 渲染为 bitmap：
   - text page -> `128x32`
   - formula -> 高度固定 `32`，宽度可大于 `128`
3. 生成 `display_blocks`：
   - `format: "1bit_xbm"`
   - `data: base64`
4. 保存到 DB 并返回给 ESP32

### 2.2 设备职责

1. 每拍一张立即调用 upload API
2. 按 Enter 调 solve API
3. 读取 `display_blocks`
4. base64 解码后 `drawXBMP`
5. 按键滚动：
   - 上下：切换 block / 垂直阅读
   - 左右：公式横向滚动
6. `42` 打开题目 sidebar 选择历史题目

---

## 3. 按键布局变更（已实现）

- `14`: scroll 上
- `21`: scroll 下
- `40`: scroll 左
- `41`: scroll 右
- `38`: 拍照
- `39`: enter / 确定（触发 solve）
- `42`: 切换页面（sidebar）

---

## 4. EstateBoostCopilot 改动清单

### 4.1 新增文件

- `app/utils/casioDisplay.ts`
  - text/formula -> 1-bit bitmap pipeline
  - MathJax 渲染公式，`sharp` rasterize，pack 为 XBM bit-order，再 base64

### 4.2 修改文件

- `app/utils/casioAi.ts`
  - Prompt 升级为要求 `answer_full + blocks`
  - 调用渲染器得到 `displayBlocks`
  - 返回 `displayText`（兼容）+ `displayBlocks`（主数据）
- `app/api/casio-ai/solve/route.ts`
  - 将 `display_blocks` 回给设备
  - 将 `displayBlocks` 存进 DB
- `prisma/schema.prisma`
  - `CasioAiQuestion` 增加 `displayBlocks Json @default("[]")`
- `prisma/migrations/20260519100000_ebc_add_casio_display_blocks/migration.sql`
  - 新增 `displayBlocks` 列
- `package.json`
  - 增加 `mathjax-full`

### 4.3 现有 API 契约变化

`POST /api/casio-ai/solve` 成功响应新增：

- `display_blocks: Array<{ type, kind, width, height, format, data }>`

并保留：

- `answer`
- `display_text`
- `usage`

---

## 5. Casio-AI-Machine 改动清单

### 5.1 主文件重构

- `src/main.cpp`
  - 从“纯文本聊天渲染”改为“block bitmap viewer”
  - 新增 sidebar 题目选择模式
  - 新按键映射生效
  - 每拍即传（upload），Enter 才 solve
  - 解析 `display_blocks` 并绘制

### 5.2 关键实现点

1. `parseDisplayBlocksFromResponse(...)`：
   - 读取 `display_blocks`
   - base64 -> bytes
   - 校验 `((width+7)/8)*height` 长度
2. `renderBitmapBlock(...)`：
   - `u8g2.drawXBMP(...)`
   - 公式宽图支持 `xOffset`
3. `actionLeft/actionRight`：
   - 横向步进滚动（`H_SCROLL_STEP=64`）
4. `actionPage`：
   - 进入/退出 sidebar

---

## 6. 这次为何这样改

1. ESP32 资源有限，不适合做 LaTeX/Markdown 排版
2. 服务器端渲染可控、可迭代、可统一中英数与公式风格
3. `1-bit bitmap` 传输体积远小于 ASCII 像素串，且和 OLED 像素模型天然匹配
4. 后续可扩展：
   - 中文全部转 bitmap
   - 公式抗锯齿/阈值策略优化
   - block 缓存与回放

---

## 7. 当前限制与已知风险

1. 服务器字体环境会影响中文效果（建议部署机安装可用 CJK 字体）
2. 超复杂公式会变宽，依赖左右滚动阅读
3. 当前设备端不做持久化，断电后历史丢失（设计如此）
4. 本地环境未跑 `pio` 编译（当前执行环境无 `pio` 命令）

---

## 8. 下一步建议（给接手 worker）

1. 在 EBC 先执行 migration，再做线上验证：
   - upload -> solve -> 返回 `display_blocks`
2. 在真实板子验证：
   - 纯中文题
   - 中英混排
   - 长公式（需要左右滚）
3. 如中文显示仍不稳定，升级为：
   - server 把 text 也统一渲染 bitmap（目前已经接近此模式）
4. 增加 debug endpoint（可选）：
   - 拉取某题 `displayBlocks` 做离线查看

---

## 9. 参考文件入口

- Device:
  - `Casio-AI-Machine/src/main.cpp`
  - `Casio-AI-Machine/server-api-integration.md`
- Backend:
  - `EstateBoostCopilot/general/app/utils/casioDisplay.ts`
  - `EstateBoostCopilot/general/app/utils/casioAi.ts`
  - `EstateBoostCopilot/general/app/api/casio-ai/solve/route.ts`
  - `EstateBoostCopilot/general/prisma/schema.prisma`
