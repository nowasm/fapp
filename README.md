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
include/figmalib/   公共 API（document / parser / renderer / ui）
src/                解析器、SVG path 解析、ThorVG 场景构建、字体、渲染器、序列化
backends/raylib/    raylib 后端（其他引擎后端的参考模板，~100 行）
apps/editor/        figmaedit — Figma 风格的可视化编辑器（raylib + raygui）
examples/           demo_raylib 演示程序 + 示例 Figma JSON
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
