# Casio OLED Math Answer Display Blocks

Generated `45` OLED pages.

- Screen: 128×32
- Format: `1bit_xbm`
- Each decoded block length: `ceil(128/8)*32 = 512 bytes`
- Convention: `1 = OLED pixel ON`, `0 = background OFF`
- Use with current ESP `drawXBMP()` path.

Files:
- `casio_oled_math_answer_display_blocks.json` — API-style response body.
- `casio_oled_preview_pages/` — enlarged PNG previews for human checking only.

Note: Preview PNGs are not for ESP32; ESP32 should consume JSON `display_blocks`.

我的分块策略大概是这个：

## 核心策略

### 1. **按语义切块，不按原文机械切**

我不是单纯每 16 个字切一页，而是按解题逻辑切：

```text
定义变量
→ 分布
→ 条件转化
→ 公式
→ 计算比较
→ 最终答案
```

这样每页不是碎片，而是一个“信息单元”。

---

### 2. **每页最多 2 行，每行尽量短**

因为 OLED 是：

```text
128×32
```

所以一页就是：

```text
2 行 × 每行约 8~12 个中文字/符号
```

我尽量让每页变成：

```text
第一行：概念/步骤
第二行：公式/补充
```

例如：

```text
n=152 时，
P(X>10)≈0.9031
```

---

### 3. **长公式拆成多页**

比如：

```text
Σ k=0..10 C(n,k)(0.1)^k(0.9)^(n-k)
```

太长，所以拆成：

```text
Σ k=0..10
C(n,k)(0.1)^k
```

下一页：

```text
×(0.9)^(n-k)
<= 0.1
```

这样比横向滚动舒服。

---

### 4. **保留所有关键信息**

我尽量没有删掉原文里的：

```text
变量定义
分布
概率要求
等价变形
求和公式
试算结果
最终答案
```

但把 Markdown 结构转成 OLED 结构。

---

### 5. **公式优先可读，不强行一页塞满**

公式如果太复杂，我会牺牲页数，保证每一页能看懂。

---

## 你说的“空行”来源

主要是这些页面只有一行：

```text
题意要求：
其中：
即：
经计算：
且
```

我为了保持语义停顿，把它们单独放了一页，所以第二行空了。

你不喜欢空行的话，下一版可以改策略：

## 新策略：减少空行

把提示词和下一条公式合并：

原本：

```text
题意要求：
```

下一页：

```text
P(X > 10) >= 0.9
```

改成一页：

```text
题意要求：
P(X>10)>=0.9
```

再比如：

```text
即：
100-m <= X <= m
```

这样会更紧凑。

---

## 最推荐最终策略

```text
1. 标题单独一页
2. “其中/即/所以/要求”不要单独占页
3. 能合并就合并到下一行
4. 长公式拆页
5. 最终答案单独一页
```

也就是：

```text
语义清楚 > 原文排版
但不要浪费第二行
```

你之后给 worker 的规则可以写：

```text
OLED page must use both lines when possible.
Avoid single-line pages unless it is a title or final answer.
Merge transition words like “即/其中/所以/题意要求” with the next formula.
```
