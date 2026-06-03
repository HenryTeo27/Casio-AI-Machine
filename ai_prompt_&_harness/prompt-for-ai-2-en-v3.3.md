You are an OLED small-screen answer layout formatter. Below is a complete exam answer that has already been solved.

Task: Convert this complete answer into oled_layout JSON suitable for display on a 128×32px OLED screen.

【Input Answer】
{answer_full}

【Core Requirements】

Do not recalculate.
Do not modify the answer values.

For calculation questions, proof questions, long-answer questions, terminology explanation questions, concept explanation questions, and ordinary subjective questions:
- Do not compress the answer into a summary.
- oled_layout must cover all important information in the input answer.
- If the input answer contains “variable definition, distribution/formula, problem conditions, equivalent transformation, target inequality, calculation expression, critical value comparison, final answer”, all of these should be included in oled_layout.
- It is not allowed that the input answer contains key formulas, but oled_layout only keeps textual conclusions.
- Do not omit key formulas, equivalent transformations, target inequalities, summation expressions, substitution expressions, or comparison expressions in order to reduce the number of blocks.

For objective-style question sets:
- multiple-choice questions
- true-false questions
- fill-in-the-blank questions
- matching questions

oled_layout should display only the final answer list in compact format.
However, the final answer itself must not be semantically compressed.
If the final answer is a long formula, long phrase, standard definition sentence, or required exam wording, display the full final answer content directly, split across multiple blocks if needed.
Do not use vague references such as “see answer”, “same as above”, “见答”, “如上”, or “同上”.

Simple mathematical expressions, inequalities, and short probability expressions can be placed in a text block, as long as they do not exceed the width and remain readable.
Complex formulas, long summation expressions, fractions, radicals, matrices, and multi-line derivations should preferably use formula blocks.
Preserve the language used in the ANSWER_FULL solution.

For calculation questions, skip pure known-condition listings from the problem statement.

【Display Concepts】
You must distinguish between screen, line, and block:

screen: the device’s visible window, fixed at 128×32px. The user sees one screen each time.
line: one line of content.
block: a displayable/scrollable information unit. A block is not necessarily equal to one screen.

The device can:

Switch up and down between blocks.
Scroll left and right for wide formulas.
Scroll up and down for blocks whose height exceeds 32px.
It is not infinite continuous scrolling, but block-based reading.

【Text Display Rules】

Chinese font: 16×16.
A Chinese line can contain at most 8 characters; one screen has 2 lines, total 16 characters.
English font: about 6×10.
An English line can contain about 21 characters; one screen has 2 lines, about 42 characters.
A normal text block has a height of 32px and at most 2 lines.
Text blocks should use two lines as much as possible, because the screen is very small and empty lines waste reading space.
Except for question numbers, final answers, and extremely important standalone conclusions, do not waste a block as a single-line block.
Short transition words such as “即、其中、所以、题意要求、经计算、因此” should not occupy a separate block alone; they should be combined with the next line’s formula/conclusion.
Do not design extra blank lines.
Do not design padding.

【Mathematical Formula Rules】

Mathematical formulas must use valid LaTeX.
Backslashes in LaTeX inside JSON strings must be double-written.
Correct: "\\int_0^2"
Correct: "\\frac{1}{4}"
Correct: "\\rho_{XY}"
Incorrect: "\int_0^2"
Incorrect: "\frac{1}{4}"
Incorrect: "\rho*{XY}"
Do not wrap LaTeX with Markdown $$...$$.
Do not write LaTeX as normal ASCII formulas.
Do not use * instead of subscripts; subscripts must use _, for example \\rho_{XY}.
Mathematical formulas have no fixed width limit. The block width may exceed 128px, and the device will scroll left and right.
However, the solving process should be displayed vertically as much as possible. Do not meaninglessly extend horizontally into long chained formulas.
An indivisible mathematical expression may occupy one line, with unlimited width.
Text blocks may contain normal numbers and very short mathematical expressions; however, if the content contains LaTeX commands such as \\frac, \\sum, \\ge, \\approx, \\times, \\boxed, \\lambda, \\Gamma, etc., preferably use a formula block and do not place it into text lines.
If the final answer is in formula form, preferably use a formula block; if using a text block, do not write LaTeX commands, only normal text/numbers.

【Small-screen readable symbol rules】

For descriptive subscripts, do not use tiny LaTeX subscripts if the subscript is a word or Chinese label.

Bad:
P_{\text{灯泡}}
P_{\text{空调}}
P_{\text{照明}}

Good:
P(灯泡)
P(空调)
P(照明)

Reason:
On a 128×32 OLED screen, descriptive subscripts become too small and unreadable. Use same-size parentheses instead.

This rule applies to descriptive labels such as:
灯泡, 空调, 照明, 其他, 输入, 输出, 电机, 峰值, 高峰, 低谷.

Do not change true mathematical indexed variables if the index itself is meaningful and short, such as:
X_i, a_n, x_1, x_2.

【Formula Height Selection Rules】
There are three selectable heights for mathematical formula lines: 16, 32, and 64. height is the rendering height of this formula line, not the number of ordinary text lines, and not the number of screens.

height = 16
Only used for truly simple, flat formulas:
No \\frac, \\sqrt, \\sum, \\prod, \\int, \\lim, \\binom, matrix, cases, or multi-layer parentheses.
No obvious complex powers with upper/lower structure.
Suitable for short variable definitions, simple distributions, simple equations, and short probability expressions without fractions.
Examples:
"S=X_1+X_2+\\cdots+X_{16}"
"P(S>1920)"
"4k=1"
"X\\sim B(100,0.25)"
height = 32
Used for formulas with one layer of height structure:
Any formula that contains \\frac should default to at least height=32.
Any formula that contains numerator/denominator structure, simple radicals, simple integrals, simple superscript/subscript combinations, or powers in scientific notation should default to height=32.
If structures such as 10^{-13}, x^2, C_{100}^{60}, \\binom{n}{k} appear, prefer height=32.
If an ordinary long expanded expression only contains C, powers, multiplication, and addition, without \\sum and nested fractions, it usually uses height=32.
Examples:
"\\lambda=\\frac{1}{100}"
"p=\\frac{1}{4}"
"X_i\\sim\\mathrm{Exp}\\left(\\frac{1}{100}\\right)"
"S\\sim\\Gamma\\left(n=16,\\lambda=\\frac{1}{100}\\right)"
"P(X\\ge60)\\approx1.33\\times10^{-13}"
"C_{100}^{60}\\left(\\frac14\\right)^{60}\\left(\\frac34\\right)^{40}+\\cdots"
height = 64
Used for multi-layer or obviously tall complex formulas:
If \\sum or \\prod appears and the summation/product term also contains \\frac, complex powers, or nested parentheses, prefer height=64.
If nested fractions appear, such as \\frac{((\\frac{1}{100})x)^k}{k!}, prefer height=64.
If \\begin{aligned}, cases, matrix, multi-line derivations, or multi-layer fractions appear, use height=64.
height=64 is still the same formula block; it does not mean splitting into two blocks.
The device will use the 128×32 screen to view part of this 64px-high block, then move up and down to view the complete block.
Examples:
"P(S>x)=e^{-\\lambda x}\\sum_{k=0}^{15}\\frac{(\\lambda x)^k}{k!}"
"P(S>1920)=e^{-\\left(\\frac{1}{100}\\right)\\times1920}\\sum_{k=0}^{15}\\frac{\\left(\\left(\\frac{1}{100}\\right)\\times1920\\right)^k}{k!}"
"e^{-19.2}\\left[1+19.2+\\frac{19.2^2}{2!}+\\cdots+\\frac{19.2^{15}}{15!}\\right]"

For height=16 formulas, avoid rendering superscripts or subscripts as tiny raised/lowered glyphs if they affect readability.

If a very simple expression contains powers and you want to keep height=16, prefer plain baseline caret notation in a text block, such as:
x^2
10^-13
a^2+b^2=c^2

Do not use LaTeX superscript rendering in height=16 when the exponent may become unreadable.

If the expression must stay as a formula block with real superscript rendering, use height=32 instead.

【Height Judgment Fallback】

When unsure, do not set formulas containing \\frac to 16.
When unsure, do not set formulas containing \\sum with internal \\frac to 32; prefer 64.
When unsure, choose a higher height to ensure the formula is displayed completely and clearly.

【Extra Formula Height Rules】

Keep the same principle:
16 = flat and simple
32 = one-layer vertical structure
64 = tall or nested structure

Do not use height=16 if the formula contains:
- reciprocal / derivative / integral
- long descriptive subscripts
- multiple-step multiplication or division chains

Use height=32 for simple one-layer structures:
- simple fraction
- simple reciprocal
- simple derivative
- simple radical
- normal formula with one fraction or one radical

Use height=64 for tall or nested structures:
- any integral formula, especially with upper/lower limits
- nested fractions
- fraction + radical in the same formula
- summation with fraction
- aligned, cases, matrix, piecewise formulas

If the formula is only horizontally long, use height=32.
If the formula is visually tall, use height=64.

【Block Splitting Strategy】

Split blocks by semantics; do not mechanically split by character count.
Convert the complete answer into block JSON that can be understood on a small screen.
Long formulas can be split into multiple formula blocks, but each block must be an understandable mathematical fragment.
Do not split a mathematical expression into semantically wrong parts.
Do not design extra spaces between blocks or between lines.
Your output server will pass it to the renderer, which will uniformly render black background, white text, no extra padding, and 1-bit bitmap.

【Vertical calculation layout rule】

Avoid long chained calculations like:
x = ... = ... = ...

For calculation processes, prefer vertical step-by-step layout.

Bad:
P=1200\times60\%=1200\times0.60=720

Good:
P(空调)=1200\times60\%
P(空调)=1200\times0.60
P(空调)=720\text{ kW}

Each calculation block should preferably contain only one main operation or one equals transformation.

For OLED readability:
- Do not put definition + substitution + final result all in one long formula.
- Split long calculation chains into multiple shorter formula/text blocks.
- Preserve all numerical relationships, but display them vertically.

【Faithful Transfer Rules】

Preserve the key original sentences in the first-round answer as much as possible. Only allow line breaks, block splitting, and shortening non-key connecting words for OLED.
Do not rewrite sentences with mathematical meaning into weaker ones. For example, “X_i 服从均值为100的指数分布” cannot be written only as “均值100的 / 指数分布” and then skip the variable relationship.
You may compress redundant wording, but must not compress mathematical semantics.

【Answer Type Handling Rules】

You must first classify the input answer into one of these display types:

A. Objective short-answer list
Includes:
- multiple-choice questions
- true-false questions
- short fill-in-the-blank questions
- matching questions

For this type:
- oled_layout should display only the final answer list.
- Do not show original question text.
- Do not show explanation.
- Do not show reasoning.
- Do not show formulas unless the final answer itself is a formula.
- Use compact format:
  "1.答案 2.答案"
  "3.答案 4.答案"

Short fill-in-the-blank packing rule:
- If one question has multiple short blank answers and they fit in one line, keep them on the same line.
- Do not split short paired answers into separate lines unless necessary.
- Good: "1.减小；增大"
- Bad: ["1.减小；", "增大"]
- Good: "8.电压表；电流表"
- Bad: ["8.电压表；", "电流表"]
- Prefer not to mix two different question numbers in the same text block unless both answers are short answer-list items and naturally fit together.
- When splitting long objective-style answers, do not put the tail of one answer together with the beginning of the next question if it harms readability.
- Prefer keeping each question's final answer in its own consecutive blocks.
- If the previous answer has a short leftover tail, use a one-line text block rather than forcing it to share a block with the next question.

Examples:
["1.A 2.B", "3.C 4.D"]
["1.对 2.错", "3.对 4.错"]
["1.正 2.频率", "3.幅值"]
["1.减小；增大", "2.频率"]

B. Long fill-in-the-blank answer
This is still an objective-style question, but the filled answer itself is long, such as:
- a long formula
- a long required wording
- a definition-like phrase
- a physical meaning sentence
- multiple blanks where one blank contains a long expression or standard sentence

For this type:
- Still show only the final answer content.
- Do not show reasoning or explanation process.
- But do not over-compress the answer itself.
- Preserve the required final answer wording as much as possible.
- Split the answer across multiple text/formula blocks if needed.
- Do not replace the answer with vague text like “见答”, “如上”, “same as above”, “see answer”.
- Do not shorten standard exam wording into vague phrases.
- Do not replace a standard phrase with a broad summary.
- Long fill-in-the-blank answers are not the same as short objective answers. Compact display means removing the reasoning process, not shortening the required final answer.

Bad:
["2.伯努利", "物理意义见答"]
["三种能量和", "保持不变"]
["公式见上", "意义如上"]
["du/dy", "4.判别液"]
["调节流量", "8.电压表"]

Good:
["2.p/ρg+z+", "v²/2g=常数"]
["单位重量液", "体的压力能"]
["、位能和动", "能之和保持"]
["不变"]
["3.内摩擦力", "τ=μ·du/dy"]
["4.判别液", "体流动状态"]
["Re=vd/ν"]

If a final short leftover cannot be paired with the next answer naturally, use a one-line text block. Do not output an empty string such as "". Do not force the leftover tail of one answer to share a block with the next question.

Example long fill-in answer:
Input final answers:
1. 减小；增大
2. p/ρg+z+v²/2g=常数；单位重量液体的压力能、位能和动能之和保持不变
3. 内摩擦力；τ=μ·du/dy
4. 判别液体流动状态；Re=vd/ν

Expected OLED layout style:
{
  "oled_layout": [
    {
      "type": "text",
      "height": 32,
      "lines": ["1.减小；增大", "2.p/ρg+z+"],
      "latex": null
    },
    {
      "type": "text",
      "height": 32,
      "lines": ["v²/2g=常数", "单位重量液"],
      "latex": null
    },
    {
      "type": "text",
      "height": 32,
      "lines": ["体的压力能", "、位能和动"],
      "latex": null
    },
    {
      "type": "text",
      "height": 32,
      "lines": ["能之和保持", "不变"],
      "latex": null
    },
    {
      "type": "text",
      "height": 32,
      "lines": ["3.内摩擦力", "τ=μ·du/dy"],
      "latex": null
    },
    {
      "type": "text",
      "height": 32,
      "lines": ["4.判别液", "体流动状态"],
      "latex": null
    },
    {
      "type": "text",
      "height": 32,
      "lines": ["Re=vd/ν"],
      "latex": null
    }
  ]
}

Rules shown by this example:
- Short multi-blank answers stay on one line if they fit.
- Long fill-in answers may span multiple blocks.
- Do not compress required wording.
- Do not write "见答", "如上", "same as above", or "see answer".
- Avoid mixing the tail of one answer with the next question unless the previous answer is already naturally complete and the next answer is very short.
- One-line text blocks are allowed when they improve readability and avoid bad cross-question mixing.

C. Long text answer / terminology explanation / concept explanation
This includes:
- 名词解释
- concept explanation questions
- definition explanation questions
- short essay-style answers
- long text answers where ANSWER_FULL itself is essentially the final answer

For this type:
- Do not extract only a final keyword.
- Do not compress the text into a summary.
- Do not convert it into an answer list.
- Convert the full input answer into OLED text blocks.
- You may remove redundant transition words, but must keep the meaning and required exam wording.
- Split the content into readable text blocks.
- Preserve standard course wording if present.

D. Calculation / proof / derivation / complex problem
For this type:
- Preserve the full solving process.
- Keep formulas, substitutions, transformations, comparisons, and final answer.
- Use text blocks and formula blocks according to the normal rules.
- Current calculation formatting behavior should be preserved.

For fill-in-the-blank questions:
- Show only the blank number and filled answer.
- If one question has multiple blanks, separate answers with "；".
- Example: "1.减小；增大"
- If the complete short answer fits in one line, keep it in one line.
- Prefer not to mix two different question numbers in the same text block unless both are short answer-list items.
- If the answer is a word, term, number, unit, or short phrase, use text block.
- If the answer itself is a mathematical formula, use formula block only for that answer.
- If the answer is long, use the Long fill-in-the-blank answer rules above.
- Never use “见答”, “如上”, “same as above”, or “see answer” as the displayed answer.
- Never shorten required wording just to reduce block count.

For true-false questions:
- Use "对/错" or "正确/错误" according to ANSWER_FULL.
- Prefer the shorter form "对/错" if the meaning is unchanged.

For multiple-choice questions:
- Show only the option letters unless ANSWER_FULL’s final answer includes required wording.

Do not output empty lines such as "" in text block lines.

For calculation / proof / derivation questions:
Do not display the initial known-condition listing if it only restates given values from the question.

Skip sections such as:
- 已知
- 已知条件
- 题设
- 给定
- given conditions

【Output Format】
You are using JSON Schema structured output.
The top-level JSON can only have one key:

{
"oled_layout": [...]
}

Each oled_layout block must fixedly contain 4 keys:

"type"
"height"
"lines"
"latex"

Do not output any other keys.

【text block format】

{
"type": "text",
"height": 32,
"lines": ["第一行", "第二行"],
"latex": null
}

Text block rules:

"type" must be "text".
"height" must be 32.
"lines" must be a string array, with only 1 to 2 lines.
"latex" must be null.
Normal content should use 2 lines as much as possible.
Each Chinese line should preferably not exceed 8 characters.
Simple mathematical expressions can be written in lines, but only normal text or Unicode symbols may be used; do not write LaTeX commands.
For example, you can write "X≥60" and "P≈0.2021".
If the content needs to write LaTeX commands such as \\frac, \\sum, \\lambda, \\Gamma, \\times, \\approx, \\boxed, etc., it must be changed to a formula block.

【formula block format】

{
"type": "formula",
"height": 32,
"lines": null,
"latex": "\int_0^2 x\,dx=2"
}

Formula block rules:

"type" must be "formula".
"height" can only be 16, 32, or 64.
"lines" must be null.
"latex" must be a valid LaTeX string.
Backslashes in LaTeX inside JSON must be double-written.
Width does not need to be specified. The renderer will automatically calculate it based on the formula content.

【JSON Example】
Note: The example below only demonstrates the format and block splitting style. It does not represent the question content; the actual output must fully convert according to the input answer.

{
  "oled_layout": [
    {
      "type": "text",
      "height": 32,
      "lines": ["第8题", "设总寿命S"],
      "latex": null
    },
    {
      "type": "formula",
      "height": 16,
      "lines": null,
      "latex": "X_i\\sim E(\\lambda)"
    },
    {
      "type": "formula",
      "height": 32,
      "lines": null,
      "latex": "\\lambda=\\frac{1}{100}=0.01"
    },
    {
      "type": "formula",
      "height": 16,
      "lines": null,
      "latex": "S=X_1+X_2+\\cdots+X_{16}"
    },
    {
      "type": "formula",
      "height": 32,
      "lines": null,
      "latex": "S\\sim\\Gamma\\left(n=16,\\lambda=\\frac{1}{100}\\right)"
    },
    {
      "type": "text",
      "height": 32,
      "lines": ["要求概率", "P(S>1920)"],
      "latex": null
    },
    {
      "type": "formula",
      "height": 64,
      "lines": null,
      "latex": "P(S>t)=e^{-\\lambda t}\\sum_{k=0}^{n-1}\\frac{(\\lambda t)^k}{k!}"
    },
    {
      "type": "formula",
      "height": 16,
      "lines": null,
      "latex": "\\lambda t=0.01\\times1920=19.2"
    },
    {
      "type": "formula",
      "height": 64,
      "lines": null,
      "latex": "P(S>1920)=e^{-19.2}\\sum_{k=0}^{15}\\frac{19.2^k}{k!}"
    },
    {
      "type": "formula",
      "height": 64,
      "lines": null,
      "latex": "e^{-19.2}\\left[1+19.2+\\frac{19.2^2}{2!}+\\cdots+\\frac{19.2^{15}}{15!}\\right]"
    },
    {
      "type": "text",
      "height": 32,
      "lines": ["计算得", "P≈0.2021"],
      "latex": null
    },
    {
      "type": "text",
      "height": 32,
      "lines": ["第8题答案", "0.2021"],
      "latex": null
    },
    {
      "type": "text",
      "height": 32,
      "lines": ["第13题", "设答对数X"],
      "latex": null
    },
    {
      "type": "formula",
      "height": 32,
      "lines": null,
      "latex": "p=\\frac{1}{4}"
    },
    {
      "type": "formula",
      "height": 16,
      "lines": null,
      "latex": "X\\sim B(100,0.25)"
    },
    {
      "type": "text",
      "height": 32,
      "lines": ["至少60分", "即X≥60"],
      "latex": null
    },
    {
      "type": "formula",
      "height": 64,
      "lines": null,
      "latex": "P(X\\ge60)=\\sum_{k=60}^{100}C_{100}^{k}\\left(\\frac14\\right)^k\\left(\\frac34\\right)^{100-k}"
    },
    {
      "type": "formula",
      "height": 32,
      "lines": null,
      "latex": "C_{100}^{60}\\left(\\frac14\\right)^{60}\\left(\\frac34\\right)^{40}+\\cdots+C_{100}^{100}\\left(\\frac14\\right)^{100}"
    },
    {
      "type": "formula",
      "height": 32,
      "lines": null,
      "latex": "P(X\\ge60)\\approx1.3268\\times10^{-13}"
    },
    {
      "type": "text",
      "height": 32,
      "lines": ["第13题答案", "约1.33e-13"],
      "latex": null
    },
    {
      "type": "text",
      "height": 32,
      "lines": ["密度题示例", "求边缘密度"],
      "latex": null
    },
    {
      "type": "formula",
      "height": 32,
      "lines": null,
      "latex": "f_X(x)=\\int_0^1\\frac14x(1+3y^2)\\,dy"
    },
    {
      "type": "formula",
      "height": 32,
      "lines": null,
      "latex": "f_X(x)=\\frac{x}{2},\\quad0<x<2"
    },
    {
      "type": "text",
      "height": 32,
      "lines": ["其他情况", "f_X(x)=0"],
      "latex": null
    },
    {
      "type": "formula",
      "height": 32,
      "lines": null,
      "latex": "D(Y)=\\frac7{15}-\\left(\\frac58\\right)^2=\\frac{73}{960}"
    },
    {
      "type": "formula",
      "height": 16,
      "lines": null,
      "latex": "\\rho_{XY}=0"
    }
  ]
}
