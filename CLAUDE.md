# figmalib — AI 工作指南

把 Figma 文件当作 UI 运行时：**一个 app = 设计(.fig) + 逻辑(.js)**。
C++ 库（解析 → 布局 → ThorVG 光栅化 → raylib 上屏）+ QuickJS 脚本层。

## 构建与测试

构建目录是 `build/`（CMake + Ninja + MSVC /MD）。**必须先加载 VS 环境**，
PowerShell 里用临时 cmd 脚本包一层：

```powershell
$bat = 'call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1' + "`r`n" +
       'cd /d D:\work_open\figmalib\build' + "`r`n" + 'cmake --build . --config Release -j'
Set-Content build\bw.cmd $bat -Encoding ascii; cmd /c "D:\work_open\figmalib\build\bw.cmd"; Remove-Item build\bw.cmd
```

验证（都在 `build/` 下运行）：

- `render_test.exe` — 离屏渲染自检，必须 `RESULT: OK`
- `layout_test.exe` — 布局数学自测
- `demo_wallet.exe --selfdrive sd` — 全功能巡演（滚动/惯性/选区/转场/固定 tab 栏），
  产出 `sd_*.png` 截图用 Read 工具目检
- `figmaplay.exe <fig> <js> --shot out.png [--frames N]` — 渲 N 帧截图退出

链接失败 LNK1104 = exe 正在运行，先 `Stop-Process`。临时截图用完即删。

## AI 开发 app 的闭环

> 从零做一个 app 走 **`/new-app` skill**（`.claude/skills/new-app/`）——脚手架
> (`tools/figmanew.py`) → 设计（figmaedit MCP，套 `design-systems/` 的审美 token）
> → 写 app.js → `--shot` 自验 → 迭代。一个 app 也可是标准目录
> `<dir>/app.json`（`figmaplay <dir>`，字段见 README "app 工程"）。下面是底层速查：

1. **设计**：启动 `build\figmaedit.exe <file.fig>`，仓库根 `.mcp.json` 已配置
   figmaedit 的 MCP（127.0.0.1:9223），用它的 15 个工具直接读改设计
   （get_node_tree / create_node / update_nodes / get_screenshot / save_document…）。
2. **逻辑**：写 `app.js`。完整 JS API 见 `include/figmalib/script.h` 头部注释。速查：
   - `ui.onClick/onHover/onUpdate`、`ui.navigateTo(name, "slideLeft", 0.3)` / `navigateBack`
   - `ui.bindList(name, count, (item, i) => …)`，节点：`.find/.child/.parent/.index/.text/.type`
   - `ui.setText/setVisible/setOpacity/setVariant/setScroll/setEditable/focusText`
   - `ui.find/findAll/tap(nameOrNode)`、`ui.setResizeMode("reflow")`
   - `setTimeout/setInterval`（app 时间）、`fetch()`（Promise）、`localStorage`（持久化到
     `<script>.storage.json`）
3. **验证**：`figmaplay.exe design.fig app.js --shot out.png` 后 Read 截图；
   交互验证参考 `examples/scripts/wallet.js` 的 SELFDRIVE 模式（`ui.tap` 合成点击 +
   `--selfdrive` 前缀截图）。figmaplay 运行中改 .js 会热重载，脚本需可重入
   （重跑一遍应幂等）。

## 关键约定与坑

- 设计数据修补（如 wallet.fig 的 Home 漏标固定导航）在脚本里做：
  `ui.findAll("Bottom Nav Bar").forEach(n => n.scrollFixed = true)`。
- 巡演/测试脚本**数帧不要累计 dt**——首帧 GetFrameTime 含文件加载耗时。
- 合成多帧指针手势必须在一个 tick 内完成（pointerDown/Move/update/Up 一气呵成），
  跨帧会和后端的真实鼠标喂入打架。
- 转场/滚动动画时钟每帧最多计 1/30s；转场中途截图按 `ui.transitionProgress()` 门控。
- ThorVG `Picture::load(path)` 自带按路径解码缓存；滚动/转场都不重建场景
  （ScrollBinding 变换重定向 / 后端贴图合成），性能敏感改动先看 `src/renderer.cpp`。
- .fig 输入靠 fig2json（`D:\work_open\fig2json`，Rust）转 canvas.json，缓存于
  `<file>.fig.export/`；测试素材在 `D:\work_open\fig2psd\test\figma\`（wallet.fig 最全）。
- Web 验证：渲染用 `msedge --headless=new --timeout=30000 --screenshot=...`
  （`--virtual-time-budget` 会在 wasm 首帧前触发，不可用）；输入/日志用
  `tools/web_test/`（serve.py 的 /slow 拖住 load 事件让 --dump-dom 晚截，
  clicktest.html 派发真实 DOM 鼠标事件——**mousedown/up 必须隔几百 ms**，
  同一帧间隙内会在 raylib 的状态轮询中互相抵消）。Web 画布固定 420x900：
  raylib 的 RESIZABLE 在 web 上会造成 framebuffer/canvas 属性/CSS 三方尺寸
  不一致，GLFW shim 鼠标坐标错位（点击全部偏移）。
