# Casio AI Machine 服务器 API 对接说明

本文给 `Casio-AI-Machine` 固件开发者使用，说明如何调用 `EstateBoostCopilot` 上的 Casio 专用接口。

---

## 1) Base URL

- `https://accelertechnology.my`

---

## 2) 认证方式（固定）

每个请求都要带：

- `X-Device-Id: CASIO_AI_MACHINE_001`
- `X-Device-Api-Key: <设备密钥>`

> 当前是服务器端固定校验设备 ID + 设备 Key。  
> 设备端不用也不能决定模型。

---

## 3) 接口一：上传照片

### Endpoint

- `POST /api/casio-ai/upload-photo?question_id={number}&index={number}`

### Headers

- `Content-Type: image/jpeg`
- `X-Device-Id: ...`
- `X-Device-Api-Key: ...`
- `X-Question-Id: {number}`（可选，建议与 query 一致）
- `X-Photo-Index: {number}`（可选，建议与 query 一致）

### Body

- 原始 JPEG 二进制（raw bytes）

### 成功返回示例

```json
{
  "ok": true,
  "photo_id": "uuid",
  "question_id": 12,
  "index": 0
}
```

### 失败常见

- `401 invalid_device_auth`
- `400 invalid_question_id`
- `400 invalid_photo_index`
- `400 empty_photo_body`
- `400 invalid_content_type`

---

## 4) 接口二：发起 AI 求解

### Endpoint

- `POST /api/casio-ai/solve`

### Headers

- `Content-Type: application/json`
- `X-Device-Id: ...`
- `X-Device-Api-Key: ...`

### Body

```json
{
  "device_id": "CASIO_AI_MACHINE_001",
  "question_id": 12,
  "photo_count": 2,
  "mode": "gpt-test",
  "photo_ids": ["photo_id_1", "photo_id_2"],
  "context_tail": "最近几题的摘要文本..."
}
```

说明：

- `photo_ids` 必须是前一步上传返回的 `photo_id`。
- `question_id` 要与上传时一致。
- `context_tail` 可选，建议截短后传。

### 成功返回示例

```json
{
  "ok": true,
  "answer": "完整回答（可较长）",
  "display_text": "文本预览（兼容字段）",
  "display_blocks": [
    {
      "type": "bitmap",
      "kind": "text",
      "width": 128,
      "height": 32,
      "format": "1bit_xbm",
      "data": "base64..."
    },
    {
      "type": "bitmap",
      "kind": "formula",
      "width": 360,
      "height": 32,
      "format": "1bit_xbm",
      "data": "base64..."
    }
  ],
  "usage": {
    "input_tokens": 123,
    "output_tokens": 456,
    "total_tokens": 579
  }
}
```

### 失败常见

- `401 invalid_device_auth`
- `400 invalid_question_id`
- `400 photo_ids_required`
- `400 photo_ids_not_found`
- `500 openai_api_key_missing`

---

## 5) 模型策略（当前）

- 服务器端固定模型：`gpt-5.4-nano`
- 设备端不控制模型
- 以后换模型：仅改服务器代码，不改设备

---

## 5.1) 渲染策略（当前）

- 服务器把 AI 回答拆成 `blocks`（text/formula）
- 服务器把 text 与 formula 都渲染成 `1-bit bitmap`
- ESP32 只做：
  - base64 解码
  - OLED 绘制
  - 上下切换 block
  - 左右横向滚动（超宽公式）

---

## 6) 服务器存储说明

- 图片上传到 Vercel Blob（服务器完成）
- Blob 路径格式：
  - `casio-ai/device/{deviceId}/{YYYY-MM-DD}/q{questionNo}/{questionNo}-{photoIndex}-{uuid}.jpg`
- DB 记录会保存 `photo_id` 与图片 URL 关联

---

## 7) 推荐调用顺序

1. 用户连拍 1~N 张
2. 每张调用 `/upload-photo`，收集 `photo_id`
3. 用户按 OK
4. 调用 `/solve`（带 `photo_ids`）
5. OLED 展示 `display_text`

---

## 8) 最小联调清单

- 设备请求包含 `X-Device-Id` + `X-Device-Api-Key`
- 上传请求 `Content-Type` 为 `image/jpeg`
- `question_id` 与 `photo_ids` 对齐
- 服务器已配置：
  - `OPENAI_API_KEY`
  - `BLOB_READ_WRITE_TOKEN`
