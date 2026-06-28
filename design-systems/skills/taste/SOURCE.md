# taste — 来源与裁剪说明

本目录 vendor 自 **taste-skill**（"anti-slop frontend" 设计审美 skill 库）：

    taste-skill — https://github.com/Leonxlnx/taste-skill
    Copyright (c) 2026 Leonxlnx
    MIT License（见同目录 LICENSE）

## 拿了什么（可移植的审美知识，逐字未改）

| 文件 | 上游 | 内容 |
|---|---|---|
| `SKILL.md` | `skills/taste-skill/SKILL.md` | 主 skill（design-taste-frontend）：design read、三个 dial、反默认纪律、字体/配色/布局/内容密度纪律、AI Tells、em-dash 禁令、Pre-Flight Check |
| `minimalist.md` | `skills/minimalist-skill/` | 极简/编辑风审美 flavor |
| `soft.md` | `skills/soft-skill/` | 柔和/友好审美 flavor |
| `brutalist.md` | `skills/brutalist-skill/` | 工业/粗野审美 flavor |
| `redesign.md` | `skills/redesign-skill/` | 既有界面审计→重做协议 |

## 没拿什么（与 figo 渲染模型无关，照 design-systems/ 一贯的裁剪口径丢弃）

`image-to-code` / `imagegen-frontend-*` / `brandkit` / `stitch-skill` /
`gpt-tasteskill` / `output-skill` / `taste-skill-v1` —— 这些要么是「生成 React/HTML
代码」「调图像生成工具」「对接 Google Stitch / Codex」的 web-codegen 专用件，要么是被
主 skill 取代的旧版。figo 渲染的是 Figma 节点→ThorVG，不是浏览器，这些 runtime 件
不适用。

## figo 怎么用它

上游内容是给「写 Tailwind/Motion/GSAP 前端代码」的 agent 看的。其中**框架无关的审美
原则**（反默认、字体/配色/布局/留白/内容密度纪律、AI Tells、em-dash 禁令、Pre-Flight）
对 figo 设计同样成立；**实现层指令**（Tailwind class、`motion/react`、GSAP 骨架、
shadcn/Radix 包、dark mode token 写法）需要翻译成 figoedit MCP 的节点操作，或直接判为
out-of-scope。这层翻译在 `.claude/skills/figo-design/SKILL.md` 里完成——那个 skill
默认加载本目录，是 figo 侧的正确入口。**不要把上游的 Tailwind/React 指令原样套到
figo 上。**
