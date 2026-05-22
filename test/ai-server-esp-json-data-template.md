# 1. AI → Server JSON

AI 只负责：**完整解答 + OLED 布局计划**。
AI 不生成 bitmap/base64。

```json
{
  "answer_full": "完整考试标准解答",
  "oled_layout": [
    {
      "type": "text",
      "height": 32,
      "lines": ["第一行", "第二行"]
    },
    {
      "type": "formula",
      "height": 16,
      "latex": "X\\sim B(n,0.1)"
    },
    {
      "type": "formula",
      "height": 32,
      "latex": "x=\\frac{-b\\pm\\sqrt{b^2-4ac}}{2a}"
    },
    {
      "type": "formula",
      "height": 64,
      "latex": "\\begin{aligned} ... \\end{aligned}"
    }
  ]
}
```

## AI JSON keys

| Key           | 必须 | Options / 类型 | 说明                     |
| ------------- | -: | ------------ | ---------------------- |
| `answer_full` |  是 | `string`     | 完整考试标准解答，服务器存 DB/debug |
| `oled_layout` |  是 | `array`      | 给 renderer 的小屏布局计划     |

## `oled_layout[]` block keys

### Text block

```json
{
  "type": "text",
  "height": 32,
  "lines": ["第一行", "第二行"]
}
```

| Key      | 必须 | Options / 类型        | 说明                 |
| -------- | -: | ------------------- | ------------------ |
| `type`   |  是 | `"text"`            | 普通文字 block         |
| `height` |  是 | `32`                | text block 固定 32px |
| `lines`  |  是 | `string[]`，长度 `1~2` | 每屏最多两行             |

### Formula block

```json
{
  "type": "formula",
  "height": 32,
  "latex": "P(X\\le 10)=\\sum_{k=0}^{10}C_n^k(0.1)^k(0.9)^{n-k}"
}
```

| Key      | 必须 | Options / 类型       | 说明         |
| -------- | -: | ------------------ | ---------- |
| `type`   |  是 | `"formula"`        | 数学公式 block |
| `height` |  是 | `16` / `32` / `64` | 公式渲染高度     |
| `latex`  |  是 | `string`           | 合法 LaTeX   |

## AI 不要输出这些

```text
id
role
scroll
width
bitmap
base64
format
viewport_width
viewport_height
```

这些都由 server renderer 自动处理。

---

# 2. Server 内部处理

服务器收到 AI JSON 后：

```text
answer_full → 存 DB
oled_layout → renderer → 1bit_xbm bitmap blocks
```

Renderer 自动做：

```text
text → 128×32 bitmap
formula → 按 LaTeX 实际宽度和 height 渲染 bitmap
width → 自动计算
base64 → 自动生成
```

---

# 3. Server → ESP32 JSON

ESP 只需要显示数据，越短越好。

## 成功格式

```json
{
  "ok": true,
  "b": [
    ["t", 128, 32, "base64..."],
    ["f", 260, 32, "base64..."],
    ["f", 128, 64, "base64..."]
  ]
}
```

## 失败格式

```json
{
  "ok": false,
  "e": "upload failed"
}
```

---

# 4. Server → ESP keys

| Key  |   必须 | Options / 类型     | 说明            |
| ---- | ---: | ---------------- | ------------- |
| `ok` |    是 | `true` / `false` | 请求是否成功        |
| `b`  | 成功时是 | `array`          | bitmap blocks |
| `e`  | 失败时是 | `string`         | 错误信息          |

## `b[]` 格式

每个 block 是数组：

```json
["t", 128, 32, "base64..."]
```

含义：

```text
[kind, width, height, data]
```

| Index | 名称     | Options / 类型  | 说明                                   |
| ----: | ------ | ------------- | ------------------------------------ |
|   `0` | kind   | `"t"` / `"f"` | `t=text`, `f=formula`                |
|   `1` | width  | `number`      | bitmap 实际宽度，可超过 128                  |
|   `2` | height | `number`      | `32` for text；`16/32/64` for formula |
|   `3` | data   | `string`      | base64 encoded `1bit_xbm` bytes      |

## ESP 默认规则

ESP 直接默认：

```text
format = 1bit_xbm
screen = 128×32
1 = pixel ON
0 = pixel OFF
```

ESP 校验：

```cpp
expected = ((width + 7) / 8) * height;
decoded.size() == expected;
```

---

# 最终数据链

```text
AI → Server:
answer_full + oled_layout

Server 保存:
answer_full

Server → ESP:
{ ok, b: [[kind,width,height,data], ...] }

ESP:
decode base64 → drawXBMP → scroll block
```
