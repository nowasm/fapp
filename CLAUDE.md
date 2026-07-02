# figo — AI 工作指南

把 Figma 文件当作 UI 运行时：**一个 app = 设计(.fig) + 逻辑(.js)**。
C++ 库（解析 → 布局 → ThorVG 光栅化 → raylib 上屏）+ QuickJS 脚本层。

## 构建与测试

构建目录是 `build/`（CMake + Ninja）。CMake 工程名与静态库目标统一为 **figo**
（`libfigo.a` / `libfigo_script.a` / `libfigo_raylib.a`；导出工具 `figo2godot` → Godot 4、
`figo2cocos` → Cocos Creator 3.x 预制体，走 `/figo2cocos` skill）。

**macOS**（实测可用，clang++ + Ninja）——首次 configure 后增量编译只需在 `build/` 下：

```bash
cd build && cmake . && cmake --build . -j
```

**Windows**（MSVC /MD）**必须先加载 VS 环境**，PowerShell 里用临时 cmd 脚本包一层：

```powershell
$bat = 'call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1' + "`r`n" +
       'cd /d <repo>\build' + "`r`n" + 'cmake --build . --config Release -j'
Set-Content build\bw.cmd $bat -Encoding ascii; cmd /c "<repo>\build\bw.cmd"; Remove-Item build\bw.cmd
```

验证（都在 `build/` 下运行；Windows 加 `.exe` 后缀）：

- `render_test` — 离屏渲染自检，应 `RESULT: OK`
  （注：缺系统字体时 `auto-height text did not grow` 会单点失败，属环境问题非回归）
- `layout_test` — 布局数学自测
- `demo_wallet --selfdrive sd` — 全功能巡演（滚动/惯性/选区/转场/固定 tab 栏），
  产出 `sd_*.png` 截图用 Read 工具目检
- `figoplay <fig> <js> --shot out.png [--frames N]` — 渲 N 帧截图退出
- `python tools/bench.py` — benchmark app 套件跑批（`examples/apps/` 下标
  `"benchmark": true` 的 app 自驱断言，协议见 docs/benchmark-gaps.md），应全 PASS

链接失败（Windows LNK1104 / macOS 占用）= exe 正在运行，先结束进程。临时截图用完即删。

## AI 开发 app 的闭环

> 从零做一个 app 走 **`/new-app` skill**（`.claude/skills/new-app/`）——脚手架
> (`tools/figmanew.py`) → 设计（figoedit MCP，套 `design-systems/` 的审美 token）
> → 写 app.js → `--shot` 自验 → 迭代。一个 app 也可是标准目录
> `<dir>/app.json`（`figoplay <dir>`，字段见 README "app 工程"）。**要让设计不长成
> AI 默认款，设计步骤套 `/figo-design` skill**（`.claude/skills/figo-design/`）——它默认
> 加载 `design-systems/skills/taste/` 的反模板审美纪律并翻译到 figoedit 节点操作。下面是底层速查：

1. **设计**：启动 `build\figoedit.exe <file.fig>`，仓库根 `.mcp.json` 已配置
   figoedit 的 MCP（127.0.0.1:9223），用它的 21 个工具直接读改设计
   （get_node_tree / create_node / update_nodes / import_image / import_svg /
   audit_design / make_component / create_instance / sync_instances /
   get_screenshot / save_document…）。**复用**：把建好的卡片/按钮/列表行用
   `make_component` 标成 master，再 `create_instance` 盖章复用（保持一致）；改完
   master 调 `sync_instances` 把样子传播到所有实例（figo 实例无活链，按需同步；
   同步会覆盖实例内容，逐实例的文案/颜色 override 需同步后重打）。插画支持两条路：
   - **位图**：`import_image`（base64 `data` 或本地 `path`）把 PNG/JPEG/WebP 收进文档
     （落在同级 `<doc>.assets/`，存进 .figo.json 后重开仍可解析），默认顺手建一个带
     IMAGE fill 的节点；也可给任意节点设 `fill:{type:"IMAGE",imageRef,scaleMode}`。
   - **矢量**：`import_svg`（`data` 是 SVG 文本或 `path` 指 .svg）把 SVG 拆成一个 FRAME +
     每个图形一个 VECTOR（支持 path/rect/circle/ellipse/line/polygon、transform、实色
     填充/描边、尽力而为的线性/径向渐变）。`monochrome:"#RRGGBB"` 把所有实色填充统一
     成一色（适合图标）；`palette:{"oldhex":"#NEW"}` 按色值改色对接设计 token。保持矢量、
     可缩放可改色，导引擎更干净。不支持 text/`<image>`/`<use>`/clipPath/mask/filter。
     **开箱图标**：`design-systems/icons/`（35 个 Lucide 描边图标，ISC）——
     `import_svg {path:"design-systems/icons/<name>.svg", monochrome:"<token色>"}` 即可，
     清单与加图标方法见该目录 README。
2. **逻辑**：写 `app.js`。完整 JS API 见 `include/figo/script.h` 头部注释。速查：
   - `ui.onClick/onHover`（回调带 viewport 坐标 x,y）、`ui.onLongPress`（≥0.5s，
     消费 click）、`ui.onSwipe`（"left"/"right"）、`ui.onScroll`（同帧合并、惯性每帧派发）、
     `ui.onUpdate`、`ui.navigateTo(name, "slideLeft", 0.3)` / `navigateBack`
   - `ui.bindList(name, count, (item, i) => …)`，节点：`.find/.child/.parent/.index/.text/.type`、
     `.scrollX/.scrollY`（可读写）、`.maxScrollX/.maxScrollY`
   - `ui.setText/setVisible/setOpacity/setVariant/setScroll/setEditable/focusText`
   - `ui.find/findAll/tap/longPress(nameOrNode)`、`ui.pointerDown/Move/Up(x, y)`
     （合成手势，多 move 手势限一个 tick 内）、`ui.setResizeMode("reflow")`
   - `setTimeout/setInterval`（app 时间）、`fetch()`（Promise）、`localStorage`（持久化到
     `<script>.storage.json`）
3. **验证**：`figoplay.exe design.fig app.js --shot out.png` 后 Read 截图；
   交互验证参考 `examples/scripts/wallet.js` 的 SELFDRIVE 模式（`ui.tap` 合成点击 +
   `--selfdrive` 前缀截图）。figoplay 运行中改 .js 会热重载，脚本需可重入
   （重跑一遍应幂等）。
4. **设计质量评审**：走 **`/design-critic` skill**（`.claude/skills/design-critic/`）——
   截图(看) + figoedit MCP 的 `audit_design`(量：离色板填充、不在字阶的字号、低于 WCAG AA
   的对比度) → 给具体修改项 → `update_nodes` 改 → 重审/重截图验证。`audit_design` 的
   `tokensPath` 指向 `design-systems/<name>/design-tokens.json`。

## 关键约定与坑

- 设计数据修补（如 wallet.fig 的 Home 漏标固定导航）在脚本里做：
  `ui.findAll("Bottom Nav Bar").forEach(n => n.scrollFixed = true)`。
- 巡演/测试脚本**数帧不要累计 dt**——首帧 GetFrameTime 含文件加载耗时。
- 合成多帧指针手势必须在一个 tick 内完成（pointerDown/Move/update/Up 一气呵成），
  跨帧会和后端的真实鼠标喂入打架。
- 转场/滚动动画时钟每帧最多计 1/30s；转场截图门控用
  `ui.transitionProgress() >= 1`（转场中 eased [0,1)，**空闲返回 1**）。
- 事件处理器里调 bindList/setVariant 会**立即停止本次冒泡**（结构变更消费
  事件，同导航语义），且处理器持有的 node 参数随之失效——需要时重新 find。
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
