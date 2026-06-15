---
name: new-app
description: 用 figmalib 从零做一个 app（设计 .fig + 逻辑 .js）的端到端工作流。当用户要"做一个 app / 一个页面 / 一个原型"，或要新建/改写一个 figmaplay app 工程时使用。覆盖：脚手架 → 设计（figmaedit MCP）→ 写逻辑 → 截图自验 → 迭代。
---

# 用 figmalib 做一个 app

一个 app = **设计（.fig/canvas.json）+ 逻辑（app.js）+ `app.json`**，渲染走
ThorVG，逻辑跑在 QuickJS。下面是从零做出来的闭环，**按顺序走，每步都自验**。

完整 JS API 见 `include/figmalib/script.h` 头注；CLAUDE.md 有构建/坑位速查。

## 0. 先想清楚（30 秒）
- **形态** → 选模板：列表类 `list-detail`，多页带底部导航 `tab-shell`，表单 `form`。
- **审美** → 选设计系统：见 `design-systems/`（金融选 coinbase/revolut，极简选
  linear-app/stripe，活泼选 duolingo…）。

## 1. 脚手架
```
python tools/figmanew.py --list                       # 看有哪些模板
python tools/figmanew.py <dir> -t <template> -n "<App Name>" -d <design-system>
```
产出标准工程 `<dir>/{app.json, app.js, design.json}`。**从模板改，不要从零生成。**
不贴合任何模板时，复制 `examples/apps/sample` 起步。

## 2. 设计（figmaedit MCP）
仓库根 `.mcp.json` 已配 figmaedit（`127.0.0.1:9223`）。启动编辑器连上：
```
build\figmaedit.exe <dir>\design.json
```
用 15 个 MCP 工具改设计（都在主线程帧间执行，与用户共享 undo）：
- **读**：`get_node_tree` / `get_node` / `get_editor_state`、`get_screenshot`（AI 的眼睛）
- **写**：`create_node` / `update_nodes`（批量=一次 undo）/ `delete_nodes` /
  `duplicate_node` / `move_node`
- **联动/存**：`set_selection` / `set_page` / `save_document` / `undo` / `redo`

要点：
- **套设计系统 token**：建节点前读 `design-systems/<sys>/design-tokens.json` +
  `DESIGN.md` 的 "Agent Prompt Guide"，按 `design-systems/TOKEN_MAPPING.md` 把
  token 折算成 figmaedit 属性（`fill` 用 `#RRGGBB`、`cornerRadius`、文本 `fontFamily`
  /`fontSize`/`fontWeight`、`effects` 阴影）。pill 圆角=高/2；rgba→`#RRGGBBAA`。
- **节点命名是契约**：脚本靠**唯一图层名**找节点（`ui.find("xxx")`）。给要被脚本
  操作的节点起稳定、唯一的名字。
- **列表容器**：要用 `ui.bindList` 的列表，容器必须开**垂直 auto-layout**
  （`update_nodes` 的 `autoLayout{mode:VERTICAL,itemSpacing}`）且**只放一个 item
  模板子节点**——bindList 拿首个子节点克隆 N 份、靠 auto-layout 竖排，多放会重叠。
- 改完 `save_document` 写回 `<dir>/design.json`。

> 不开编辑器也能直接编辑 `design.json`（REST 风格：node 有 `id/name/type/size/`
> `relativeTransform[[1,0,tx],[0,1,ty]]/fills/children`，TEXT 有 `characters/style`）。
> 颜色是 0..1 的 `{r,g,b,a}`。可参考 `tools/gen_templates.py` 的生成逻辑。

## 3. 逻辑（app.js）
对着 `script.h` 写。常用：
- 事件：`ui.onClick(name, fn)` / `ui.onHover(name, fn)` / `ui.onUpdate(dt=>…)`
- 导航：`ui.navigateTo(name, "slideLeft", 0.28)` / `ui.navigateBack()` /
  `ui.selectFrame(name)`
- 数据：`ui.bindList(name, count, (item,i)=>{ item.find("x").text = … })`
- 改属性：`ui.setText/setVisible/setOpacity/setVariant/setScroll`、
  `ui.setEditable/focusText/blur`（表单）
- 异步/存储：`setTimeout/setInterval`、`fetch()`（Promise）、`localStorage`
- manifest：`globalThis.APP`（`APP.entryFrame` 等）

规则：
- **可重入/幂等**——figmaplay 改 .js 会热重载、整段重跑，重复跑结果必须一致。
- **设计数据修补在脚本里做**（别改设计源），如
  `ui.findAll("Bottom Nav Bar").forEach(n => n.scrollFixed = true)`。
- node 句柄别跨 `bindList/setVariant/navigation` 持有（底层节点会重建）。

## 4. 自验（必做，每次改完都验）
```
figmaplay <dir> --shot out.png [--frames N]     # 渲 N 帧截图退出
```
然后 **Read 这张截图**目检。交互验证用 SELFDRIVE（见
`examples/scripts/wallet.js` / 模板里的 `app.js`）：脚本里 `if (globalThis.SELFDRIVE)`
用 `ui.tap(name)` 合成点击驱动巡演，`figmaplay <dir> --selfdrive sd` 产出
`sd_home.png` / `sd_nav.png`。
- figmaplay **运行中改 .js 会热重载**，可边看边改。
- 合成多帧手势必须**一个 tick 内**完成（down/move/update/up 一气呵成）。
- 截图有 HiDPI 黑边是 Scale 模式 + DPI 缩放所致，非 bug；要随视口重排用
  `ui.setResizeMode("reflow")`。

## 5. 按设计系统审计（可选但推荐）
对照 `design-systems/skills/design-review` 的思路，拿截图对着所选系统的 token
自评：配色/间距/层级/对齐对不对，逐条修 → 回到第 4 步。

## 6. 打包（Phase 2，尚未就绪）
`app.json` 的 `package` 段（id/version/icon）是给后续 `figmapack` 统一打包
（win/web/android/ios…）预留的，当前先占位。

---
**闭环**：脚手架 → figmaedit 套 token 建设计 → 写 app.js → `--shot` 看图 → 迭代。
每一步都有"眼睛"（get_screenshot / --shot），不要盲改。
