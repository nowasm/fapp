---
name: design-critic
description: 对一个 figo 设计做"视觉评审 + token 合规"闭环，并直接改好。当用户要"评审/审一下这个设计"、"看看哪里不好看"、"按 design system 检查/对齐 token"、"打磨/优化界面"、"设计 review"，或做完一个 app/页面想拔高质量时使用。覆盖：截图(看) + audit_design(量) → 评分与具体修改项 → 用 MCP 改 → 重新截图/重审验证。
---

# design-critic — 设计评审闭环

把"AI 看截图凭感觉"升级成**感性(截图) + 理性(token 量化)双通道**，给出可执行修改并改好、验证。
两个证据源缺一不可：

- **截图**（figoedit MCP `get_screenshot`，或 app 工程 `figoplay <dir> --shot out.png`）——
  看层级、留白节奏、对齐、构图、视觉重量。
- **`audit_design`**（figoedit MCP）——量化 token 合规：离色板的填充/描边、不在字阶上的字号、
  不在圆角尺度上的圆角、对比度低于 WCAG AA 的文字。眼睛测不准的，交给它。

## 0. 定位目标与设计系统
- **跑在 figoedit 里**（有 MCP，127.0.0.1:9223）→ 直接用 MCP 工具。
- **app 工程**（`<dir>/app.json`）→ 读 `designSystem` 字段定系统；截图用
  `build/figoedit <design> &` 起一个再走 MCP，或 `figoplay <dir> --shot`。
- 设计系统目录：`design-systems/<name>/`，tokens 在
  `design-systems/<name>/design-tokens.json`，审美口径在 `DESIGN.md` 的 *Agent Prompt Guide*。
  拿不准选哪套就问用户一句（金融 coinbase/revolut，极简 linear-app/stripe，活泼 duolingo…）。

## 1. 采证
```
get_node_tree                       # 先认识结构
get_screenshot {maxSize:1024}       # 整页；再对关键帧逐个 get_screenshot {nodeId}
audit_design {tokensPath:"design-systems/<name>/design-tokens.json"}
```
读 `DESIGN.md` 的 Agent Prompt Guide，记住这套系统的"性格"（深/浅、圆/方、紧/松、克制/张扬）。

## 2. 评审（按这套 rubric 逐条过，结合两个证据源）
1. **层级**：一眼能找到主操作/主信息？字重/字号/颜色有没有拉开主次？
2. **留白与节奏**：间距是否成体系（同一尺度反复出现），还是随手凑的数？呼吸感够不够？
3. **对齐**：左右边距、基线、网格——有没有 1~2px 的脏对齐？
4. **对比度**：`audit_design` 的 `low-contrast` 全部要修（error 级）。
5. **色彩纪律**：`off-palette` 项——是该 snap 到 token，还是有意为之？品牌强调色用得克制吗？
6. **字体排印**：`off-typescale` 项；行高、字距、段落是否舒服。
7. **一致性**：相同语义的元素（卡片/按钮/标签）样式是否统一。
8. **形状语言**：圆角/描边/阴影是否贴合这套系统的性格（`off-radius` + 目检）。

## 3. 定级并修
- 排序：**error（对比度/可读性）> warn（off-token、不一致）> polish（留白、对齐微调）**。
- 用 `update_nodes` 批量改（一批 = 一个可撤销步；用户能 Ctrl+Z）。优先 `audit_design` 给的
  `suggestion`（已经是具体数值/token）。
- token 折算（颜色→fill/stroke、字号→fontSize、圆角→cornerRadius、间距→autoLayout、
  阴影→effects）以 `design-systems/TOKEN_MAPPING.md` 为准。

## 4. 验证（必须闭环）
```
audit_design {…}        # 数字应下降：offPalette/offTypeScale/lowContrast 趋近 0
get_screenshot {…}      # 目检：改完更清晰、更统一、更有呼吸
```
没有"改完即完成"——**改完一定重审 + 重截图**，确认问题真的没了、且没引入新问题。
迭代到 `audit_design` 干净（或剩下的都是有意为之并能说明理由）为止。

## 5. 汇报
给用户：发现了什么（按 rubric 分类、附 nodeId）、改了什么、前后对比截图、还剩哪些"有意保留"的偏离及理由。

## 坑
- `audit_design` 的对比度按"最近的不透明祖先填充"当背景估算；玻璃/图片背景上的文字它会偏保守，
  以截图目检为准。
- `off-palette` 不是都要改——渐变、插画、品牌色本就可能不在色板里；它只负责**指出**，由你判断。
- 改之前先 `set_selection` 让用户看到你在动哪些节点。
