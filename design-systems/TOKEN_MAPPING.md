# design-tokens.json → figmaedit 映射

`<system>/design-tokens.json` 是 W3C Design Tokens 风格的 token 表，每条形如
`{name, value, type}`。本文件规定 AI 怎么把这些 token 折算成 figmaedit MCP 的
`create_node` / `update_nodes` 属性。**这是上游"产 HTML/CSS"和我们"建 Figma 节点"
之间的唯一翻译层。**

figmaedit 属性以 `apps/editor/editor_mcp.cpp` 的工具 schema 为准（见 create_node /
update_nodes 描述）。颜色一律 `#RRGGBB` 或 `#RRGGBBAA`。

## 按 token type 映射

| token `type` | 例子 | figmaedit 落点 |
|---|---|---|
| `color` | `--accent = #5e6ad2` | 节点 `fill` / `stroke`，或 `effects[].color`。背景=容器 `fill`，文字=TEXT 节点 `fill`，边框=`stroke`+`strokeWeight` |
| `dimension`(px) | `--text-xl = 24px` | 按语义分流（见下）：字号→`fontSize`，圆角→`cornerRadius`，间距→`autoLayout.itemSpacing`/`padding`，描边→`strokeWeight` |
| `fontFamily` | `"Inter Variable", "Inter", …` | `fontFamily` = **取第一个具体字体名**，去引号、丢 fallback 链。若该字体未装，靠 figmalib 的字形级回退兜底（见 README 字体一节） |
| `number` | `--leading-tight = 1.00` | 行高倍数 → `lineHeight`(px) = `round(倍数 × fontSize)`。figmaedit 的 `lineHeight` 是**像素**，不是倍数 |
| `shadow` | `rgba(0,0,0,.4) 0 2px 4px` | 解析为 `effects[{type:"DROP_SHADOW", color, offsetX, offsetY, radius}]`（解析规则见下） |
| `duration` / `cubicBezier` | `--motion-base = 200ms` | **不进设计层**。归到 JS 逻辑层：`ui.navigateTo(name, transition, durationSec)` 的时长/曲线。设计阶段忽略 |

### dimension 的语义分流
token 名是分流依据（各系统命名一致）：
- `--text-*` → `fontSize`
- `--radius-*` → `cornerRadius`（`--radius-pill / 9999px` → 见 pill 规则）
- `--space-*` → auto-layout 的 `itemSpacing` 与 `padding`
- `--section-y-*` / `--container-gutter-*` → 容器 `padding`
- `--tracking-*`（em）→ `letterSpacing`(px) = `em × fontSize`（figmaedit `letterSpacing` 是像素）

## CSS-ism 折算坑（必看）

上游 token 值带 CSS 语法，**不能原样塞进 figmaedit**，按下表先归一化：

| CSS 写法 | 折算 |
|---|---|
| `rgba(r,g,b,a)` | → `#RRGGBBAA`（a×255 取整为末两位 hex）。例 `rgba(255,255,255,0.08)` → `#FFFFFF14` |
| `var(--x)` | 别名，**先在同表里解引用**到具体值再用（如 `--fg-2 = var(--fg)`） |
| `color-mix(in oklab, A, transparent N%)` | 近似为 A 的 `#RRGGBBAA`，alpha = `1 - N%`。拿不准就退化为不透明 A |
| `9999px` / pill 圆角 | figmaedit `cornerRadius` 无"全圆"语义 → 设为 `cornerRadius = 节点高度 / 2` |
| 行高 `1.00`（无单位） | × fontSize 转像素（见 number 行） |
| `box-shadow` 多层串 | 逗号拆成多条 `effects[]`，逐层解析（见下） |
| ring 阴影 `0 0 0 1px <c>` | 不是投影，是描边 → 改用 `stroke=<c>` + `strokeWeight=1` + `strokeAlign:"OUTSIDE"`，不要塞进 effects |

### box-shadow → effect 解析
单层格式 `<offsetX> <offsetY> <blur> [spread] <color>`（color 可能在串首）：
- `offsetX/offsetY` → effect 同名字段（px）
- `blur` → `radius`
- `spread` → `spread`（注意：renderer 对 INNER_SHADOW 的 spread **不渲染**，DROP_SHADOW 的 spread 支持有限，能省则省）
- 多层各自一条 effect；`0 0 0 Npx` 这种 0 偏移 0 模糊的当 ring → 走描边规则，不进 effects

我们**不支持** BACKGROUND_BLUR（毛玻璃）；遇到背景模糊类 token 退化为半透明纯色面板。

## 一个完整例子（coinbase，钱包场景）

token（已解引用 / 归一化后）：
```
--accent = #0052ff   --bg = #ffffff   --fg = #0a0b0d   --surface = #eef0f3
--radius-pill 主 CTA   --text-base = 16px   --font-body = CoinbaseSans
```
建一个主 CTA 按钮（高 56）：
```jsonc
// create_node
{ "type": "frame", "name": "CTA / Buy", "width": 320, "height": 56,
  "fill": "#0052ff", "cornerRadius": 28 }          // 28 = 56/2，pill
// 子 TEXT
{ "type": "text", "name": "CTA Label", "text": "Buy crypto",
  "fill": "#ffffff", "fontFamily": "CoinbaseSans", "fontSize": 16,
  "fontWeight": 600, "textAlignH": "CENTER", "textAlignV": "CENTER" }
```
卡片表面用 `--surface #eef0f3`，正文用 `--fg #0a0b0d`，强调链接/数值用 `--accent`。

## 流程

1. 读 `<system>/design-tokens.json`，把 `var()` 全部解引用成具体值，建一张
   `语义 → 值` 的小表。
2. 读 `<system>/DESIGN.md` 的 **"Agent Prompt Guide"** 段——它给了这套系统该怎么
   组合 token 的人话指引（哪些场景用深色区、按钮形态等）。
3. 按本文件把值折算成 figmaedit 属性，`create_node` / `update_nodes` 建设计。
4. `figmaplay <app> --shot` 截图，对照 `skills/design-review` 自评后迭代。
