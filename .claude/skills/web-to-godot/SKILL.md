---
name: web-to-godot
description: 把一个 React/HTML 页面变成可在 Godot 4 打开的工程（截图、字体、组件复用都保真）。当用户要"把这个 React 页面/网页/HTML 导成 Godot"、"web 转 Godot 工程"、"用 GOGO KILL/HUD 这类页面生成 .tscn 场景"，或提到 web2canvas / fapp2godot / html2godot 时使用。覆盖：装环境 → 一条命令跑 → Godot 打开自验 → 看图迭代。
---

# 把 React/HTML 页面变成 Godot 4 工程

一条链，已在真实页面（GOGO KILL HUD，8 屏）端到端验证：

```
React/HTML ──web2canvas──► canvas.json + images/ ──fapp2godot──► Godot .tscn + sprites
```

页面在**真实无头浏览器**里渲染再采集，所以 flex/grid 布局、`oklch` 颜色、
`clip-path` 切角、渐变、阴影、字体宽度都按浏览器算好的来。flat fill 表达不了的
（渐变/图片/`<svg>`/clip-path/旋转元素/虚线边框/外发光）会光栅化成 PNG `IMAGE`
fill——和"矢量→贴图"目标一致。

底层细节见 `tools/web2canvas/README.md`；构建坑见
`.claude/skills/web-to-godot/` 同级的记忆 [[fapp-build-dir-gotcha]]。

## 0. 先确认（一次性）
- **node_modules 在吗**：`tools/web2canvas/` 需要 `npm install` 过
  （playwright-core + 内置 react/react-dom/@babel/standalone，驱动**已装的
  Edge/Chrome**，不下载 Chromium）。缺了就 `cd tools/web2canvas && npm install`。
- **fapp2godot.exe 在吗**：默认找 `build_godot/fapp2godot.exe`（core-only 目标）。
  没有就按 [[fapp-build-dir-gotcha]] 配 `build_godot` 编出来——只编 ~12 个核心
  cpp + 工具，秒级，**必须在 vcvars64 下**用 CLAUDE.md 的 PowerShell cmd 包装跑
  （别后台 Bash，会挂）。

## 1. 想清楚输入（30 秒）
看一眼目标页面，定四件事：
- **入口**：本地 `file.html` 还是 `http://` URL。本地文件会被起一个临时 HTTP
  服务托管（Babel 的 XHR 加载 .jsx 需要 http://）。
- **采集根** `--root`：整页就 `body`（默认）；有固定舞台就指它，如 `#stage`。
- **视口** `--viewport WxH`：**对准设计的舞台尺寸才有 1:1**（HUD 是 1280x720）。
- **多屏** `--states`：页面如果用 `window.__nav(state)` 切屏，列出所有屏名一次
  全采（每屏一个顶层 frame / 一个 .tscn）。非 `__nav` 用 `--nav-fn` 指定。

## 2. 一条命令跑
```
node tools/web2canvas/html2godot.js <url|file.html> --out <godotDir> \
     [--states "a,b,c"] [--fonts DIR] [--root SEL] [--viewport WxH] \
     [--wait MS] [--prefabs] [--browser msedge|chrome] [--fapp2godot <exe>]
```
`<godotDir>/` 即一个可直接 Godot 4 打开的工程：每屏一个 `.tscn`、去重 sprites、
打包字体、`manifest.json`、`project.godot`。中间产物在 `<godotDir>/.web2canvas/`。

关键参数：
- `--fonts DIR`：**几乎必加**。指向页面用的字体根（含 `fonts.css`）。Google
  Fonts 在沙箱里被掐 → 不给字体则浏览器用 fallback 测宽、Godot 用默认字体渲，
  文字会重叠/错位。给了 web2canvas 会注入 fonts.css + 等 `document.fonts.ready`
  再测量，fapp2godot 把字体绑进 `theme_override_fonts`。
- `--prefabs`：把重复组件（卡片/按钮/行）抽成 `components/*.tscn`（PackedScene），
  每处实例化 + 按实例覆盖文本——真正的 prefab 复用，不是 inline 复制。**可选，
  默认关**。
- `--wait MS`：等页面 JS 渲染完（Babel-in-browser 的 React 给 2500~5000ms）。

完整示例（GOGO KILL HUD，8 屏 → 8 个 Godot 场景）：
```
node tools/web2canvas/html2godot.js "<...>/html_ui_export/app/HUD C.html" --out hud_app \
  --root "#stage" --viewport 1280x720 --wait 2500 \
  --fonts "<...>/html_ui_export/fonts" \
  --states "lobby,search,room,role,game,meeting,victory,aftermath"
```

> 只要 canvas.json 不要 Godot：`node tools/web2canvas/index.js <input> -o out.canvas.json [同上参数]`。
> 该 canvas.json 还能喂 figmalib 原生链（`render_test` 自验、figmaedit、figmaplay）。

## 3. 自验（必做）
按可用性从高到低：
- **导入自检**：`<godot> --headless --editor --quit --path <godotDir>` —— 干净导入
  无报错即结构 OK。
- **目检渲染**：用 godot MCP（`run_project` / `game_screenshot`，或 `launch_editor`
  打开某个 `.tscn`）截图，Read 出来对着原页面看：布局/字体/颜色/发光/切角对不对。
- **像素级地真值**：要分辨"解析 bug vs Godot 专有 bug（z 序/裁剪/文本）"，拿
  figmalib 自己的渲染器当 oracle——`render_test`（`-DFIGMALIB_BUILD_EXAMPLES=ON`
  编出）出 BMP，转 PNG 对比。和 Godot 一致 = 链路对；不一致 = Godot 侧问题。

发现不对 → 多半落在下面的"已知 limits"或保真长尾，回到第 2 步调参或记问题。

## 4. 已知 limits（先告诉用户，别让他以为全覆盖）
- **弹窗/点击态没覆盖**：只采 screen 级（`window.__nav`）。只靠点击打开的
  popup/overlay 现在采不到（需 click-sim 或 deep-link，属 P1 待办）。
- **滚动区外没覆盖**：只截可视视口，每屏滚动超出部分丢失。
- **保真长尾**：letter-spacing（Godot Label 无原生支持）、每角独立圆角的
  NinePatch（现用最大角）。
- 多屏依赖程序化 nav 钩子（`window.__nav` 或 `--nav-fn`）；没有就只能单屏。

---
**闭环**：装环境 → 一条命令 `html2godot.js` → Godot 打开 + 截图目检 → 调参/记 limit
→ 再跑。每步都有"眼睛"（Godot 截图 / render_test 真值），不要盲交付。
