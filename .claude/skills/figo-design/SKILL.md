---
name: figo-design
description: 用 figo（figoedit MCP + design-systems token）做设计/页面/app 界面时，默认套上 taste-skill 的"反模板审美"纪律——让产出不像 AI 默认那一套。当用户要"做个好看的界面/页面"、"设计一个 X 屏"、"按 taste 审美来做"、"别做得像 AI 生成的"、"提升设计品味"，或在 new-app 的设计步骤想拔高视觉时使用。覆盖：design read → 选 design-system → 套 taste 纪律用 figoedit 建 → --shot 自验 → audit/critic 闭环。
---

# figo-design — figo 设计 + taste 审美（默认开）

把 figo 的设计能力（figoedit MCP 直接读改节点 + `design-systems/` 的 token 下限）和
**taste-skill 的反模板审美纪律**焊在一起。目标只有一个：**产出不要长成 AI 默认那一套**
（居中 hero + 三张等宽卡 + Inter + 紫色渐变 + em-dash + 装饰小标签…）。

> **这个 skill 默认就带着 taste-skill。** 动手前先读
> **`design-systems/skills/taste/SKILL.md`**（vendor 自
> [taste-skill](https://github.com/Leonxlnx/taste-skill)，MIT；裁剪说明见同目录
> `SOURCE.md`）。它是给「写前端代码」的 agent 写的——你的工作是**把它框架无关的审美
> 原则照搬，把它的实现层指令翻译成 figoedit 节点操作**（见下方翻译表）。aesthetic
> flavor 另见 `taste/{minimalist,soft,brutalist}.md`，重做既有界面见 `taste/redesign.md`。

---

## 闭环（按顺序走，每步自验）

### 0. Design Read（动手前先读懂房间）
taste-skill §0 的核心，**框架无关，照搬**：先别建节点，先判断用户到底要什么。
一句话写出 **Design Read**：*"我把它读成：给 \<谁> 的 \<什么页面>，\<什么调性>，倾向
\<哪套 design-system / 审美家族>。"* 例：*"读成：给加密用户的钱包首屏，机构信任感，
倾向 coinbase。"* brief 真有歧义只问**一个**问题，能从上下文推断就别问，直接声明并往下走。

**反默认纪律（taste §0.D，最重要）**：不要默认伸手去拿——AI 紫渐变、深色 mesh 上的
居中 hero、三张等宽 feature 卡、到处玻璃拟物、Inter + slate-900。这些是 LLM 的默认，
按 design read 刻意绕过它们。

### 1. 三个 dial → 落到 design-system
taste §1 的三个 dial 仍然用来定调子（**这是你脑子里的旋钮，不写进文件**）：
`DESIGN_VARIANCE`（对称↔张扬）/ `MOTION_INTENSITY`（静↔影院级）/ `VISUAL_DENSITY`
（画廊↔座舱）。按 §1.A/§1.B 从 design read 推出数值。

然后把调子**落到一套 `design-systems/<name>/`**（金融 coinbase/revolut，极简
linear-app/stripe，活泼 duolingo，营销 framer，硬边 neobrutalism…）。读它的
`design-tokens.json` + `DESIGN.md` 的 *Agent Prompt Guide* 记住这套系统的性格。
**一套系统贯穿全页**（taste §2 "one system per project"）。dial 与 flavor 的对应：

| taste flavor 文件 | 大致对应 figo design-system | dial 基线 |
|---|---|---|
| `taste/minimalist.md` | linear-app / stripe / notion | V5-6 M3-4 D2-3 |
| `taste/soft.md` | duolingo / airbnb | V7-8 M5-7 D3-4 |
| `taste/brutalist.md` | neobrutalism | V8-10 M2-4 D3-5 |

### 2. 套 taste 纪律，用 figoedit 建
启动 `build/figoedit.exe <design.json>`（仓库根 `.mcp.json` 已配 MCP 127.0.0.1:9223），
用 21 个工具建/改节点。**建之前，把 taste-skill 这些框架无关的硬纪律加载进脑子**
（逐条对照 §4 / §9）：

- **字体（§4.1）**：Inter 不做默认（除非要中性/Linear 风/无障碍）；display 用紧字距、
  紧行高；serif 极度克制——只有真编辑/奢侈/出版品牌才用，且禁 Fraunces / Instrument
  Serif；同字族用粗体/斜体强调，**别为了花哨往 sans 标题里塞一个 serif 词**。
  → figoedit：`fontFamily` / `fontSize`（必须落在该系统字阶）/ `fontWeight` /
  `letterSpacing` / `lineHeight`。
- **配色（§4.2）**：最多 1 个强调色、全页锁死（Color Consistency Lock）；禁 AI 紫光；
  禁高级消费品的「米色+黄铜+暗红+浓咖」默认家族；阴影染背景色、别纯黑投影。
  → figoedit：`fill` / `stroke` 用 token `#RRGGBB`；阴影走 `effects`（染色、低不透明）。
- **形状（§4.4）**：一套圆角尺度贯穿全页（Shape Consistency Lock）。pill=高/2。
  → figoedit：`cornerRadius` 全部落在系统圆角尺度。
- **布局（§4.3 / §4.7）**：`DESIGN_VARIANCE>4` 就避免居中 hero，改 split / 非对称留白；
  hero 必须一屏装下（标题≤2 行、副文案≤20 词、CTA 不滚动可见，hero 顶 padding 别过大）；
  导航单行；**禁三张等宽 feature 卡**（§9.C）；同一种布局家族一页最多出现一次；
  zigzag 图文分栏连续最多 2 段；**eyebrow 小标签每 3 个 section 最多 1 个**（§4.7 头号违规）。
  → figoedit：用 autoLayout 排间距（同一尺度反复出现=有体系），别手凑数字。
- **内容密度（§4.9）**：标题≤8 词、副段≤25 词；禁 20 行数据表/规格表（每行一条 hairline
  是最懒布局）；长列表换组件别堆 list；**所有可见文案上线前自审一遍**（语病、AI 幻觉式
  俏皮话、假精度数字、Jane Doe / Acme 这类占位名）。
- **可读性（§4.5，error 级，必修）**：每个按钮文字对背景达 WCAG AA；CTA 文案桌面端不换行；
  表单 label/placeholder/focus ring 都过对比。
- **em-dash 禁令（§9.G，二元，零容忍）**：界面上任何可见文字**不出现 `—` / `–`**，
  用普通连字符 `-`。标题、标签、按钮、正文、署名、caption 全管。这是 AI 的头号 tell。
- **AI Tells（§9）**：版本号 eyebrow（`V0.6`/`BETA`）、`001 · Capabilities` 段号、
  到处中点 `·`、装饰性状态圆点、`Scroll ↓` 提示、图片上压标签、locale/天气条、
  `BRAND. MOTION. SPATIAL.` 装饰条——**默认全禁**，除非 brief 真要。

**复用即一致**：重复元素（卡/按钮/列表行）先做好一个 → `make_component` → `create_instance`
盖章；改 master 后 `sync_instances`。**节点命名唯一稳定**（脚本/critic 靠图层名找节点）。

### 3. --shot 自验（感性通道）
```
figoplay <dir> --shot out.png        # 或 figoedit MCP get_screenshot
```
Read 截图，对着 design read 问：层级一眼分得清吗？留白有呼吸吗？长得像不像 AI 默认？

### 4. audit + critic（理性通道，闭环）
量化 token 合规 + 对比度——眼睛测不准的交给它：
```
audit_design {tokensPath:"design-systems/<name>/design-tokens.json"}
```
`offPalette` / `offTypeScale` / `lowContrast` 趋近 0（剩下的要能说明是有意为之）。
要系统性评审/打磨就转 **`/design-critic`**（它本就是「截图+audit→评分→改→重审」闭环，
和这里的 taste 纪律同源互补）。

### 5. Pre-Flight（figo 裁剪版，出货前逐条过）
取 taste §14 里**对 figo 成立**的格子（丢掉纯 web 的 RSC/Tailwind/GSAP/Core-Web-Vitals 项）：

- [ ] **Design Read** 一句话声明过了？dial 从 brief 推、不是默认 8/6/4？
- [ ] **一套 design-system 贯穿**，没混系统？
- [ ] **零 em-dash**（`—`/`–`）——标题/标签/按钮/正文/署名/caption 全查过？
- [ ] **一个强调色锁全页**；**一套圆角锁全页**；主题不中途反色？
- [ ] **每个按钮/CTA 文字对背景过 WCAG AA**，CTA 桌面不换行，表单文字过对比？
- [ ] **字号全落字阶、圆角全落圆角尺度、间距成体系**（`audit_design` 验）？
- [ ] **hero 一屏装下**（标题≤2 行、副文≤20 词、CTA 免滚动可见）？
- [ ] **没有三张等宽卡**、没有连续 3 段同款图文分栏、eyebrow 数 ≤ ⌈section/3⌉？
- [ ] **没 AI Tells**（版本号 eyebrow、段号、装饰圆点、Scroll 提示、locale 条、装饰文字条）？
- [ ] **文案自审过**（无语病/AI 俏皮话/假精度数字/Jane Doe·Acme 占位名）？
- [ ] **重复元素走 component+instance**，命名唯一稳定？
- [ ] `audit_design` 干净（或剩下的偏离都能讲清理由）+ 截图目检通过？

哪一格不能诚实打勾，就还没做完。

---

## 翻译表：taste-skill 的 web 指令 → figo

taste-skill 大量内容是「写 React/Tailwind/Motion 代码」。**别原样套**，按此翻译：

| taste-skill（web） | figo 等价 / 处置 |
|---|---|
| Tailwind class（`text-6xl tracking-tighter`、`bg-zinc-950`、`rounded-xl`） | figoedit 节点属性 `fontSize`/`letterSpacing`/`fill`/`cornerRadius`，数值取自 `design-systems/<sys>/design-tokens.json`（折算见 `design-systems/TOKEN_MAPPING.md`） |
| §2.A 选官方组件库（Fluent/Carbon/shadcn/Radix…） | 不适用。选一套 `design-systems/<name>/` 代替「design system」 |
| `motion/react`、GSAP sticky-stack / horizontal-pan、`useMotionValue` | figo 没有 web 动画层。页面级转场用 `ui.navigateTo(name,"slideLeft",0.3)`/`navigateBack`；滚动/惯性由引擎给。复杂滚动劫持动画 = **out-of-scope**，做干净的静态构图 |
| `MOTION_INTENSITY` dial、§5 动效 | 退化成「转场 + 滚动」这点动效预算；"motion claimed = motion shown" 仍成立——别声称动效却交静态，也别半成品动效 |
| `next/font` / `@font-face` 字体加载 | figoedit 设 `fontFamily`（系统/文档已装的字族）；缺字回退见 CLAUDE.md 字体坑 |
| §4.8 图片策略（image-gen 工具、picsum、Simple Icons SVG logo） | figoedit `import_image`（位图，base64/path）/ `import_svg`（矢量，可 monochrome/palette 改色）；图标用 `design-systems/icons/`（35 个 Lucide）。**别让 UI 光秃，也别 div 假截图** |
| §8 dark mode token 切换、§11 redesign 代码审计 | 主题在一套 design-system 内锁定；重做既有 figo 设计走 `taste/redesign.md` 的审计思路 + `/design-critic` |
| §12 Block Library、§3 RSC/`'use client'`、Appendix 安装命令、Core Web Vitals | 纯 web，**忽略** |

**框架无关、直接成立、照搬**：§0 Design Read、§0.D 反默认、§1 三个 dial、§4.1 字体纪律、
§4.2 配色纪律、§4.3/§4.7 布局纪律、§4.9 内容密度与文案自审、§9 AI Tells、§9.G em-dash
禁令、§14 Pre-Flight（按上面第 5 步裁剪）。

---

## figoedit / MCP 前置检查与坑（重要——曾在别的工程踩到崩溃/掉字）

用 MCP 驱动 figoedit 设计前，先过这三条，否则容易踩到「崩溃 / 文字消失 / 读不到文件」：

1. **figoedit 必须是新 build（≥ 2026-06-26）。** 旧 build 被 MCP 驱动时有两个已修的坑：
   - `03a334c`（06-18）**SIGPIPE 守护**：在此之前，MCP 客户端在 figoedit 回包途中断开
     连接（HTTP 超时/重连），`send()` 到已关闭的 socket 会触发 SIGPIPE → **进程被直接终止
     =崩溃**。这就是「调 figo-design 时 figoedit 崩」最可能的真凶（尤其 macOS）。
   - `272d311`（06-26）**未知字体族 fallback**：在此之前，设了 figo 没有的字族会污染字体
     缓存，**整屏文字消失**（框/图标还在）。
   - 其他工程若用的是更早的 figoedit，**先重建**：`cd build && cmake --build . --target
     figoedit -j`。本仓库当前 build 已含两修，跑 `./figoedit --selftest` 应 `SELFTEST: OK`。
2. **字体是头号坑（taste-skill 在这里反而会害你）。** taste §4.1 推荐的 Geist / Satoshi /
   Outfit / Cabinet Grotesk **系统通常都没装**；figo 只认已安装或已注册的字族。`fontFamily`
   设成没有的字体会**掉字**（新 build 优雅回退到 Arial，旧 build 直接整屏空）。所以：
   - 把 taste 的字体建议当**「字形性格」参考**（紧凑 grotesque / 中性 sans / 编辑 serif），
     落地时换成**手头真有**的等价字族；拿不准就先用系统字族验证版式，再谈换字。
   - 要用特定字体：把 `.ttf/.otf` 丢进 design 文件**同级 `fonts/` 目录**，或设环境变量
     `FIGO_FONTS_DIR` 指向字体目录——figoedit 启动时会注册（见 main.cpp
     `registerConventionFonts`）。注册后 `fontFamily` 才认得。
3. **路径相对 figoedit 的启动目录，不是仓库根。** `import_svg` / `import_image` /
   `audit_design`（`tokensPath`）/ `open_document` 的 path 若传相对路径，是相对 figoedit
   的 CWD（通常是 `build/`）解析——会「cannot read file」。**一律传绝对路径**，例如
   `import_svg {path:"<repo>/design-systems/icons/check.svg", monochrome:"#0052FF"}`、
   `audit_design {tokensPath:"<repo>/design-systems/coinbase/design-tokens.json"}`。

> 已实测（2026-06-28，当前 build）：完整 figo-design 闭环（建节点→import_svg→audit→
> get_screenshot→save）+ 对抗输入（未知字体/畸形 SVG/坏参数/失效节点/截图边界/组件循环）
> + 14 线程并发 + 反复 open_document 热换文档，**figoedit 全程未崩**。崩溃只在旧 build 复现。

## 和别的 skill 的关系
- **new-app**：从零做 app 的端到端流程（脚手架→设计→逻辑→自验）。在它的「设计」步骤里
  **套本 skill 的 taste 纪律**，产出就不会是 AI 默认款。
- **design-critic**：做完/改完后的系统性视觉评审与打磨闭环。本 skill 管「建得有品味」，
  design-critic 管「事后审到干净」，二者同源（都吃截图+audit 双通道）。
- **细节**：figoedit 工具清单、token 折算、字体坑、滚动/转场时钟坑——见 CLAUDE.md 与
  `new-app` / `design-critic` 两个 skill，不在此重复。
