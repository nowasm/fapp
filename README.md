# figmalib

把 **Figma 设计稿直接作为游戏 UI** 渲染到游戏引擎中的 C++ 矢量 UI 库。

矢量光栅化基于 [ThorVG](https://github.com/thorvg/thorvg)，演示后端使用
[raylib](https://github.com/raysan5/raylib)。核心库与引擎完全解耦 —— 任何能上传
RGBA 纹理、转发鼠标事件的引擎（Unity native plugin、Unreal、Godot、自研引擎）都
可以用几十行代码接入。

```
.fig 文件 ──fig2json──▶ canvas.json ──┐
                                      │  figmalib::loadFigmaFile（自动识别格式）
Figma REST JSON (?geometry=paths) ────┤
                                      ▼
节点树  figmalib::Document (Frame / Rect / Ellipse / Vector / Text / ...)
        │  scene_builder
        ▼
ThorVG 场景图 ──SwCanvas──▶ RGBA8888 像素缓冲（直通 alpha）
        │                          │
        ▼                          ▼
   命中检测 / 输入 / 回调      引擎后端上传为纹理绘制
```

## 目录结构

```
include/figmalib/   公共 API（document / parser / renderer / ui / script）
src/                解析器、SVG path 解析、ThorVG 场景构建、字体、渲染器、脚本宿主
backends/raylib/    raylib 后端（其他引擎后端的参考模板，~200 行）
apps/editor/        figmaedit — Figma 风格的可视化编辑器（raylib + raygui + MCP）
apps/figmaplay/     figmaplay — 通用脚本播放器（app = .fig + .js，热重载）
examples/           demo_raylib / demo_wallet 演示程序、示例脚本与设计文件
  assets/           测试/演示用的 Figma JSON 与 .fig
  scripts/          figmaplay 示例脚本（wallet.js）
tests/              render_test（离屏渲染自检）、layout_test（布局数学自测）
third_party/        nlohmann/json 单头文件
```

## figmaedit 编辑器

```
build\figmaedit.exe [design.fig | canvas.json | file.json]   # 或拖放文件到窗口
build\figmaedit.exe --selftest [file]                        # 无窗口逻辑自测
```

操作对齐 Figma 原版：

| 操作 | 快捷键 |
|---|---|
| 平移画布 | 空格+拖拽 / 中键拖拽 / 滚轮（Shift=横向） |
| 缩放 | Ctrl+滚轮（以光标为中心）、Ctrl+= / Ctrl+-、Ctrl+0=100%、Shift+1=适应 |
| 选择 | 单击顶层对象、Ctrl+单击深选、双击进入容器、Esc 返回、Shift+单击加减选、空白拖拽框选 |
| 移动 | 拖拽（Shift 轴锁定）、方向键微移（Shift=10px） |
| 缩放对象 | 四角/边手柄（Shift 等比、Alt 中心对称） |
| 编辑 | Ctrl+D 复制、Delete 删除、Ctrl+Z / Ctrl+Shift+Z 撤销重做、Ctrl+S 保存 |
| 工具 | V 移动、H 抓手 |

左侧图层树（展开/可见性切换/点击选中），右侧检查器（X/Y/W/H、透明度、
圆角、填充色、文本内容），顶部工具栏（工具、页面切换、缩放显示）。
保存写出 `<原文件>.figmalib.json`（REST 格式，可直接被 figmalib 重新加载，
不覆盖原 .fig）。HiDPI 缩放自动检测，可用 `FIGMAEDIT_SCALE` 环境变量覆盖。

### MCP 服务器（AI 直接在编辑器里设计）

figmaedit 启动时内嵌一个 MCP 服务器（Streamable HTTP，仅监听本机
`127.0.0.1:9223`，端点 `/mcp`），AI 客户端连上后即可读取图层树、增删改节点、
截图自检，全部修改与用户共享同一条 undo 历史（Ctrl+Z 可撤销 AI 的任何编辑）。

```
build\figmaedit.exe --mcp-port 9300 design.fig    # 改端口
build\figmaedit.exe --no-mcp design.fig           # 关闭 MCP
set FIGMAEDIT_MCP_PORT=9300                       # 环境变量同效
```

Claude Code 接入（仓库根目录已带 `.mcp.json`，在本项目内会自动发现）：

```
claude mcp add --transport http figmaedit http://127.0.0.1:9223/mcp
```

工具一览：`get_editor_state` / `get_node_tree` / `get_node`（读），
`create_node` / `update_nodes` / `delete_nodes` / `duplicate_node` /
`move_node`（写，批量更新一次 undo），`get_screenshot`（离屏渲染当前页或
任意节点为 PNG——AI 的"眼睛"），`set_selection` / `set_page`（联动编辑器
UI），`save_document` / `open_document` / `undo` / `redo`。节点引用接受 id
或唯一图层名；颜色为 `#RRGGBB`/`#RRGGBBAA`；支持纯色/渐变填充、描边、阴影
/模糊效果、文本样式、约束与 auto-layout 元数据，VECTOR 节点可直接写 SVG
path（`svgPath`）绘制图标。

工具调用在主线程帧间执行（文档无锁互斥）；原生文件对话框打开时调用会
等待，超时返回 "editor busy"。协议层（initialize/tools/list）由网络线程
直接应答，任何时刻都可握手。`--selftest` 包含全套 MCP 工具的无窗口回归。

## 构建（Windows / MSVC）

1. 构建 ThorVG 静态库（同级目录 `../thorvg`）：

   ```
   meson setup build_static_gl --default-library=static -Dstatic=true ^
       -Dengines=cpu,gl -Dloaders=svg,lottie,ttf,png,jpg,webp -Dbuildtype=release
   ninja -C build_static_gl
   ```

   `engines=cpu,gl` 同时启用软件光栅化与 OpenGL 引擎（GPU 零拷贝路径）；
   只要 CPU 渲染可去掉 `,gl`（GPU 调用会自动回退）。

2. 构建 figmalib（raylib 由 CMake FetchContent 自动拉取）：

   ```
   cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
   cmake --build build
   build\demo_raylib.exe                       # 内置示例 UI
   build\demo_raylib.exe path\to\design.fig    # 任意 .fig / canvas.json / REST JSON
   build\demo_raylib.exe --gpu [file]          # ThorVG GL 引擎直渲 FBO（零拷贝）
   build\render_test.exe [file]                # 离屏渲染自检，输出 BMP
   build\layout_test.exe                       # 布局引擎数学自测
   ```

   demo 中 ←/→ 方向键切换设计稿里的各个 frame，R 切换响应式重排/等比缩放，
   G 切换 GPU/CPU 渲染；`--screenshot out.png` 渲 30 帧截图退出（自动化验证）。

ThorVG 路径可用 `-DTHORVG_INCLUDE_DIR=... -DTHORVG_LIBRARY=...` 覆盖。

## 使用

```cpp
#include <figmalib/figmalib.h>
#include <figmalib_raylib.h>

auto ui = figmalib::FigmaUI::fromFile("menu.json");   // Figma REST JSON
ui->onClick("btn-start", [&](figmalib::Node&) { startGame(); });
ui->onHover("btn-start", [&](figmalib::Node& n, bool in) {
    ui->setOpacity(n.name, in ? 0.8f : -1.0f);
});

figmalib::RaylibFigmaView view(*ui);
while (!WindowShouldClose()) {
    view.resize(GetScreenWidth(), GetScreenHeight());
    view.update();                       // 输入 + 脏区重绘 + 纹理上传
    BeginDrawing();
    drawGameWorld();                     // UI 半透明区域可透出游戏画面
    view.draw();
    EndDrawing();
}
```

### 脚本逻辑层（QuickJS）

不写 C++ 也能做 app：**设计是 .fig，逻辑是 .js**。`figmalib_script` 把
FigmaUI API 绑定进 QuickJS（quickjs-ng，CMake 自动拉取），`figmaplay`
是通用播放器：

```
figmaplay wallet.fig wallet.js
```

```js
// wallet.js — 完整 API 见 include/figmalib/script.h
ui.setResizeMode("reflow");
ui.selectFrame("Home");
ui.bindList("portfolio-list", coins.length, (item, i) => {
    item.find("Heading").child(0).text = coins[i].symbol;
});
ui.onClick("Card", (node) => {
    if (node.parent.name !== "portfolio-list") return;
    ui.navigateTo("Coin Info", "slideLeft", 0.28);
});
console.log("ready —", ui.frameNames().length, "frames");
```

`examples/scripts/wallet.js` 用纯脚本复刻了 demo_wallet 的全部行为
（数据绑定、导航、底部栏、可编辑文本、设计数据修补）。宿主只负责加载
两个文件 + 帧循环（`host.update(dt)` 驱动 `ui.onUpdate` 与 JS 任务队列）。

### app 工程（标准目录）

散文件之外，一个 app 也可以是**一个目录 + `app.json`**——把设计、逻辑、视口、
字体、设计系统、打包元数据收拢成一个工程，直接传目录给 figmaplay：

```
figmaplay examples/apps/sample            # 读 sample/app.json
```

```jsonc
// examples/apps/sample/app.json
{
  "name": "Starfall Menu",
  "design": "design.json",      // 相对本目录；.fig / canvas.json / REST JSON 均可
  "script": "app.js",
  "viewport": [420, 900],        // 窗口尺寸
  "entryFrame": "MainMenu",      // 启动选中的 frame（脚本仍可再导航）
  "fonts": "fonts",              // 可选；无系统字体的平台用
  "designSystem": "linear-app",  // 可选；指向 design-systems/ 里的审美 token
  "package": { "id": "com.figmalib.sample", "version": "1.0.0" }  // 预留给打包
}
```

manifest 会以 `globalThis.APP` 暴露给脚本（`APP.name` / `APP.entryFrame` …）。
不传目录、仍传两个散文件时行为完全不变（向后兼容）。`designSystem` 字段把
[`design-systems/`](design-systems/) 的设计审美知识接进创作闭环——AI 建设计前先读
对应 token，见 `design-systems/TOKEN_MAPPING.md`。

### 从模板起步（figmanew）

`templates/` 下有几个标准 app 工程模板，`tools/figmanew.py` 把模板复制成新工程
并改好 `app.json`——AI / 人从一个**能跑的 app** 改起，而不是从零生成：

```
python tools/figmanew.py --list                       # 列出模板
python tools/figmanew.py myapp --template list-detail  # 生成 myapp/
python tools/figmanew.py myapp -t tab-shell -n "My App" -d revolut
figmaplay myapp                                        # 跑起来
```

| 模板 | 结构 | 配色取自 |
|---|---|---|
| `tab-shell` | 底部 tab 框架（Home/Search/Profile，固定导航栏） | linear-app |
| `list-detail` | 可滚动列表 → 详情页（`bindList` + 导航） | coinbase |
| `form` | 可编辑输入 + 提交（`setEditable`/`focusText`） | stripe |

模板的 `design.json` 由 `tools/gen_templates.py` 用对应设计系统的 token 生成（配色/
圆角来自 `design-tokens.json`）。模板文本用 Segoe UI；要还原设计系统自带字体，把
对应 .ttf 放进 app 的 `fonts/` 目录。

### 获取 Figma 数据

**方式一：本地 .fig 文件（推荐，离线）**

Figma 中 File → Save local copy... 保存 `.fig`，然后直接：

```cpp
auto ui = figmalib::FigmaUI::fromFile("design.fig");
```

figmalib **进程内**完成 .fig→JSON 转换（[fig2json](https://github.com/kreako/fig2json)
编译为静态库直接链接，CMake `FIGMALIB_FIG2JSON_LIB` 指定 .lib 路径，
`cargo build --release` 产出），无外部进程依赖；结果缓存在
`design.fig.export/`（.fig 未变化时复用），图片自动从 .fig 内嵌数据解出并
接入渲染。静态库缺席时回退调用 fig2json CLI（查找顺序：`FIGMALIB_FIG2JSON`
环境变量 → CMake `FIGMALIB_FIG2JSON` 默认路径 → PATH）。也可以手动预转换
（`fig2json design.fig outdir`）后直接加载 `outdir/canvas.json`。

canvas.json 解析移植自 [fig2psd](../fig2psd) 的归一化逻辑：节点/填充/效果的
类型结构推断、组件实例从 master 克隆补全（含 symbolOverrides 与共享样式解析）、
derivedSymbolData 烘焙几何回退、vectorNetwork 缝合、逆矩阵渐变几何换算。

**方式二：REST API**

```
GET https://api.figma.com/v1/files/<FILE_KEY>?geometry=paths
    -H "X-Figma-Token: <PERSONAL_ACCESS_TOKEN>"
```

把返回的 JSON 存为文件即可被 `FigmaUI::fromFile` 加载。`geometry=paths` 会附带
矢量轮廓（`fillGeometry`/`strokeGeometry`），描边的 INSIDE/OUTSIDE 对齐也由此精确
还原。图片填充需另行下载（`/v1/images`），放入 `Renderer::setImageDirectory` 指定
目录，文件名为 `imageRef`。

## 已支持的 Figma 特性

- 节点：FRAME / GROUP / RECTANGLE / ELLIPSE / LINE / VECTOR / STAR / POLYGON /
  BOOLEAN_OPERATION / TEXT / COMPONENT / INSTANCE
- 填充：纯色、线性/径向渐变、角度（锥形）/菱形渐变（按 handle 基向量程序化
  光栅化为位图填充，支持旋转/偏心/非均匀拉伸）、图片（FILL/FIT/STRETCH）
- 描边：粗细、虚线、端点/拐角样式、INSIDE/OUTSIDE/CENTER 对齐（裁剪/遮罩模拟；
  有 `strokeGeometry` 时精确还原）
- 圆角（含四角独立半径）、整层透明度、`clipsContent` 裁剪
- 效果：投影（DROP_SHADOW）、内阴影（INNER_SHADOW，反形模糊+遮罩仿真，
  spread 不支持）、图层模糊（LAYER_BLUR）
- 文本：字体目录注册（解析 TTF name/OS/2 表）+ 系统字体匹配（Windows 注册表，
  含 .ttc 集合字体解包重组）、Figma 行盒垂直排版（lineHeight + half-leading）、
  对齐、字距、自动换行、省略号截断（textTruncation）、多行富文本（分词贪心
  换行 + 显式换行 + 混合字号基线对齐 + CJK 逐字断行 + 非 ASCII 分段，
  UTF-16 索引精确映射 UTF-8）、字形级字体回退（cmap 覆盖查询，西文字体里的
  CJK/符号自动换用雅黑/黑体等系统字体渲染）
- 字体约定：输入文件旁的 `fonts/` 目录与 `FIGMALIB_FONTS_DIR` 环境变量自动注册
- 运行时：帧切换、命中检测、hover/click 回调、显隐/透明度/文本动态修改
- **组件变体切换**：`ui->setVariant("btn-start", "State", "Hover")` 把实例切到
  同一组件集（COMPONENT_SET / .fig STATE_GROUP）下匹配属性的变体——克隆目标
  变体子树、按实例尺寸重排（约束/auto-layout 生效）、可与 hover/click 回调
  组合实现按钮多态；属性名/值大小写不敏感，组件集需在文档内
- **响应式布局**：约束（LEFT/RIGHT/CENTER/STRETCH/SCALE 双轴）+ 自动布局
  （横/竖 stack、padding、间距、grow、对齐、hug 尺寸、SPACE_BETWEEN、WRAP
  换行、BASELINE 基线近似、min/max 尺寸限制、绝对定位子项），
  `setResizeMode(Reflow)` 后帧随视口重排而非等比缩放：

  ```cpp
  ui->setResizeMode(figmalib::FigmaUI::ResizeMode::Reflow);  // 默认 Scale
  ui->setViewport(w, h);   // 触发 layoutFrame() 重排（demo 中按 R 切换）
  ```

  重排基于解析时快照的原始几何（`Node::base*`），反复 resize 不累积误差；
  布局引擎也可单独使用：`figmalib::layoutFrame(frame, w, h)`。
  矢量 path 几何随节点尺寸缩放，圆角矩形/椭圆按新尺寸重新生成（圆角不变形）。
  注意：.fig 路径需要本仓库配套修改过的 fig2json（上游版本会剥掉
  constraints/stack 字段）；旧缓存会因 fig2json 更新自动重新转换。
  `layout_test.exe` 为布局数学的无窗口自测。

## 一键多端打包（figmapack）

`tools/figmapack.py` 把一个标准 app 工程（app.json）一条命令打成各端包体：

```
python tools/figmapack.py <app-dir> --target win|web|android|all [--out dist]
python tools/figmapack.py examples/apps/sample -t all
```

产出落在 `<out>/<app-slug>/<target>/`：
- **win**：`figmaplay.exe` + `app/` + `run.cmd`（双击即跑）
- **web**：`index.html` + wasm/js/data（`python -m http.server` 起服务打开）
- **android**：签名好的 `<app>.apk`（`adb install -r` 装机）

打包元数据取自 app.json 的 `package` 段：
- `id` → android 包名、`version` → versionName/Code、`name` → 应用名/web 标题。
- `icon`（方形 PNG）→ web favicon + apple-touch-icon、android 各密度
  `mipmap/ic_launcher`、**win 把图标内嵌进 exe**（用 .rc 重链 figmaplay，需要
  `build/` 已配置 + VS 环境；`VCVARS` 环境变量可覆盖路径）。
- `splashColor`（`#rrggbb`，可选）+ `icon` → **启动图**：web 注入品牌化加载遮罩
  （图标 + 应用名 + spinner，load 后淡出）、android 用主题 windowBackground
  （底色 + 居中图标）在 NativeActivity 加载期显示。

原理：figmapack 把 app 目录 staging 后，web/android 运行时**优先读 staging 的
`app.json`**（无则回退 wallet demo），三端共用同一份 design + 逻辑。

约定与坑：
- **.fig 设计**在打包时用 fig2json 转 canvas.json（web/android 不能现场转）；
  canvas.json / REST `.json` 原样打包。
- **web/android 没有系统字体**：app 的 design 用到的字体必须放进 app 的 `fonts/`
  目录，否则文字渲染为空（桌面用系统字体不受影响）。
- **iOS/macOS 不在此列**——需 Mac + Xcode + codesign；app.json 的 `package` 段为其
  预留，待有 Mac/CI 时补 `--target ios`。

下面是各端构建的底层细节（figmapack 即收敛自这些）：

## Web 构建（emscripten）

wallet demo 可编译为 wasm 在浏览器运行（CPU 光栅化 → WebGL 上屏）：

```
tools\build_thorvg_wasm.cmd    # ThorVG wasm 静态库（sw 引擎、无线程、无 lottie）
tools\build_web.cmd            # 产出 build_web\figmaplay.{html,js,wasm,data}
python -m http.server 8123 -d build_web   # 打开 http://localhost:8123/figmaplay.html
```

emsdk 默认在 `D:\devlib\emsdk`（`EMSDK_HOME` 覆盖）。设计以预转换的
canvas.json + images 打包进虚拟文件系统（.fig 转换是原生步骤），字体打包自
`examples/assets/fonts`（浏览器没有系统字体，wallet 用的 Titillium Web/Poppins
已内置）。脚本层 `fetch()` 经 emscripten Fetch（`-sFETCH`，异步 XHR）走真实
网络请求——同源随意，跨域受浏览器 CORS 约束（服务端需带
`Access-Control-Allow-Origin`）。限制：热重载/`--shot` 仅桌面有效。

## Android 构建（无 gradle）

NativeActivity（`hasCode=false`）直接加载 libfigmaplay.so，aapt 手工打包：

```
tools\build_thorvg_android.cmd    # ThorVG arm64-v8a + x86_64 静态库
powershell tools\build_android.ps1  # NDK 双 ABI 编译 → build_android\figmaplay.apk
adb install -r build_android\figmaplay.apk
adb shell am start -n com.figmalib.play/android.app.NativeActivity
```

SDK/NDK 取自 `D:\devlib\android\sdk`（NDK 27.2，API 28+）。APK 内 assets 不是
文件，启动时按打包生成的 manifest.txt 解压到内部存储再走普通文件 IO；设计/脚本/
字体与 Web 版同源。触摸即指针（raylib 映射），滚动/惯性/转场/编辑全部可用。

## GPU 渲染

引擎后端可以让 ThorVG 的 GL 引擎直接渲染进自己的 FBO（零 CPU 像素拷贝）：

```cpp
// CPU（默认）：ui->setViewport(w, h) + ui->pixels() 上传纹理
// GPU：当前 GL 上下文下，把 FBO id 交给 figmalib
if (!ui->setViewportGL(fboId, w, h)) ui->setViewport(w, h);  // 无 GL 引擎时回退
ui->render();  // 直接画进 FBO，无 pixels() 回读
```

raylib 后端封装为 `view.setGpu(true)`（内部用 RenderTexture2D，绘制时翻转 Y，
并在 ThorVG 改动 GL 状态后恢复 rlgl 缓存的状态）。

## 已知限制

- 布局期文本测量（文本重排不改变节点框高度；BASELINE 用字体样式近似基线）
- 原型交互（prototype interactions）；库组件变体（组件集不在文档内时）
- BACKGROUND_BLUR（毛玻璃需要背景采样）；INNER_SHADOW 的 spread 参数；
  描边上的角度/菱形渐变（退化为径向）
