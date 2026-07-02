# Benchmark 差距盘点 — 20 个经典 app × 运行时能力缺口

> 阶段一第一交付物（见 [ROADMAP.md](ROADMAP.md)）。2026-07 基于对
> `src/script_host.cpp` / `src/ui.cpp` / `backends/raylib/` / `apps/figoplay/`
> 的实测盘点。结论先行：**缺口高度集中，补齐 6 类能力即可解锁绝大多数
> benchmark app；其中 Android 的软键盘与 fetch 两项同时阻塞阶段二的
> player 战略，优先级最高。**

## 运行时现状一句话

强项：矢量渲染、帧转场、内建惯性/嵌套/双轴滚动、组件变体切换、
桌面文本编辑（caret/选区/多行）、HTTPS fetch（Win/Web）、localStorage。

空洞：交互事件只有 onClick/onHover 两种；无内建控件语义；Android 无软键盘
也无 fetch；零系统能力桥（相机/定位/分享/通知/剪贴板/震动全无）；媒体仅
静态位图（无音频/视频/GIF/Lottie）；JS 拿不到几何/样式/滚动位置（visible、
opacity 等还是只写属性）；运行时无结构化诊断（字体缺失静默回退）。

## 缺口清单（按 20-app 命中频率排序）

| # | 缺口 | 命中 | 内容 | 关键事实 |
|---|------|-----|------|---------|
| G1 | 移动端文本输入 | 9 | Android 软键盘唤起/避让（需 JNI，现 `hasCode="false"` 无通道）、IME 预编辑显示、剪贴板（复制/粘贴完全没接）、密码遮罩 | `ui.cpp:287-349,950-1027`；桌面已有 caret/选区/多行，缺的是移动端和周边 |
| G2 | 系统能力桥 | 8 | 相机/相册、定位、分享、本地通知、深链、震动。现状零桥接，Android 无 JNI 通道 | `AndroidManifest.xml` 纯 NativeActivity；全仓 grep 0 命中 |
| G3 | 控件语义/值概念 | 7 | switch/slider/进度条/日期与时间选择器/下拉。现只有 `setVariant` 离散态整树替换，无连续值、无自动状态切换、无过渡 | `ui.cpp:1136-1195` |
| G4 | 跨平台网络 | 6 | **Android fetch 直接返回 not supported**（只实现了 `_WIN32`/`__EMSCRIPTEN__`）；无超时、无二进制 body/ArrayBuffer、无 WebSocket | `script_host.cpp:259-266,865-889` |
| G5 | 手势与事件面 | 6 | 长按、滑动（swipe/滑动删除/下拉刷新）、拖拽、双击、捏合；onScroll 回调与滚动位置读取；事件坐标入参。滚动引擎本身很完整但对 JS 是黑盒 | `figo_raylib.cpp:77-85` 只投单指针；`script_host.cpp:577-603` 仅两种事件 |
| G6 | 动态媒体 | 4 | 音频播放（raylib 有能力未接线）、视频、GIF 动图、Lottie（wasm sjlj 限制已知） | `scene_builder.cpp:250-292`；loaders 仅 svg,ttf,png,jpg,webp |
| G7 | JS 属性/查询面 | 横切 | visible/opacity/primarySizing 只写不可读（getter 落 default 返回 undefined，与 script.h 注释不符）；读不到几何(x/y/w/h)与样式；不能建/删节点 | `script_host.cpp:465-486` |
| G8 | 运行时诊断 | 横切 | 布局溢出/文本截断/字体缺失无警告（font_provider 静默回退，0 输出）；无机器可读诊断通道。直接决定 AI 迭代效率 | `font_provider.cpp:521,541` |
| G9 | 数据层 | 横切 | localStorage 之外无结构化存储；无 BaaS 绑定（目标：`ui.bindList` 接远程数据源） | `script_host.cpp:807-834` |
| G10 | 事件派发中改树 | 横切 | **onClick 处理器里调 `bindList` 是 UB**：`fireUp` 沿 `n->parent` 继续走已被 bindList 销毁的节点（habits 实测同名处理器一次点击多次乱序触发，bench.py 下 0xC0000005 崩溃）。`pointerUp` 里 `hit`/`target` 链同样悬空。修法：派发期间延迟树变更，或 fireUp 先快照名字链。当前约定：点击处理器内只做 setText/setVisible 等就地改动，重绑列表须移出派发（如 setTimeout） | `ui.cpp:114-131,842-902`（habits app 实测，2026-07） |
| G11 | 名字寻址不跨页 | 横切 | **`ui.setText/setVisible` 等按名 mutation 只搜当前 frame 不回落全文档**，与 `script.h` 注释"current frame first, then document"矛盾（`ui.find` 却会回落）——跨页写状态静默返回 false。绕法：`ui.find(name).text/.visible` 走 node 句柄。修法：`findMutable` 与 `findNode` 对齐 | `ui.cpp:136`（shop app 实测，2026-07） |
| G12 | 文档承诺未兑现 | 横切 | `ui.transitionProgress()` 写在 CLAUDE.md 巡演指引里但**未绑定到 JS**（只存在于 C++ `FigmaUI`）；AI 只能按帧数猜转场结束（且真实 vsync dt=1/60，非动画时钟 1/30 假设）。修法：补一个 JS 绑定，G7 一起做 | `ui.cpp:659`；`script_host.cpp` 无注册（settings/shop 实测，2026-07） |

## 20 个 benchmark app × 卡点矩阵

难度递进排序；**加粗**为该 app 的关键阻塞（缺了做不像样），普通字为降级可绕。
"✓ 现在就能做"= 用当前能力可做出可用版本，作为 benchmark 第一批。

| # | app | 主要验证点 | 卡点 | 状态 |
|---|-----|-----------|------|------|
| 1 | 计算器 | 纯点击网格、setText | 无 | **✅ 已做**（linear-app）|
| 2 | 番茄钟 | 计时器、变体切换、环形进度 | 音频提示 G6、通知 G2 | **✅ 已做**（spotify，静音版）|
| 3 | 习惯打卡 | bindList 日历网格、localStorage | 触发 G10 崩溃（已绕） | **✅ 已做**（duolingo）|
| 4 | 设置页 | 变体拼 switch、导航 | G3 用 setVisible 双态拼、G12 | **✅ 已做**（apple）|
| 5 | 电商浏览/购物车 | 列表、详情转场、stepper | G11 跨页写需 node 句柄绕 | **✅ 已做**（airbnb）|
| 6 | 待办清单 | 文本输入、列表增删 | **滑动删除 G5**、移动端输入 G1 | 桌面可做 |
| 7 | 笔记 | 多行编辑、持久化 | **剪贴板 G1**、移动端输入 G1 | 桌面可做 |
| 8 | 记账 | 数字输入、分组列表、简单图表 | 日期选择器 G3、移动端输入 G1 | 桌面可做 |
| 9 | 登录/注册流程 | 表单、校验、fetch POST | **密码遮罩 G1**、Android fetch G4 | 桌面可做 |
| 10 | 天气 | fetch JSON、数据绑定 | **Android fetch G4**、定位 G2 | Win/Web 可做 |
| 11 | 新闻阅读器 | fetch 列表、图片、阅读页 | **Android fetch G4**、分享 G2、下拉刷新 G5 | Win/Web 可做 |
| 12 | 菜谱 | 图片列表、搜索、收藏 | 搜索输入 G1、fetch G4 | Win/Web 可做 |
| 13 | 聊天 UI | 输入条、滚动到底、气泡列表 | **软键盘避让 G1**、**滚动位置 G5/G7**、长按 G5 | 缺口驱动型 |
| 14 | 日历/日程 | 月网格、事件增删、时间选择 | 时间选择器 G3、输入 G1 | 缺口驱动型 |
| 15 | 闹钟/倒计时 | 时间滚轮选择器、后台通知 | **滚轮选择器 G3/G5**、**通知 G2** | 缺口驱动型 |
| 16 | 音乐播放器 | 进度 slider 拖拽、播放控制 | **音频 G6**、**slider 拖拽 G3/G5** | 缺口驱动型 |
| 17 | 播客 | 流媒体音频、倍速 | **音频 G6**、fetch G4 | 缺口驱动型 |
| 18 | 相册/图库 | 网格、大图、手势 | **相册桥 G2**、捏合缩放 G5 | 缺口驱动型 |
| 19 | 扫码 | 相机、解码 | **相机桥 G2** | 缺口驱动型 |
| 20 | 打车/地图类 | 地图、定位 | **地图渲染（超范围，考虑静态瓦片图）**、定位 G2 | 最后 |

## 补齐顺序建议（backlog）

1. **G4 Android fetch**（成本低——按 Win/Web 模式补一个 Android 实现即可，
   解锁 10-12 号且是 player 前提）＋顺手补超时。
2. **G1 Android 软键盘（JNI 通道）**：这是架构性决定——`hasCode="false"`
   要改成带一个极薄 Java 层或用 `ANativeActivity` 的 JNIEnv 直调
   InputMethodManager。同一条 JNI 通道也是 G2 所有系统桥的地基，**先打通道，
   再逐个上桥**。剪贴板/密码遮罩属桌面侧小活，可并行。
3. **G5 事件面**：给 JS 加 onLongPress/onSwipe/onScroll + 事件坐标 +
   滚动位置读取。滚动引擎已完整，主要是暴露而非新造。
4. **G7 属性面补读**：把只写属性补成可读、暴露几何——小活，随手做，
   AI 自验强依赖（读不到状态就只能截图猜）。
5. **G8 运行时诊断**：字体缺失/文本截断/布局溢出输出结构化警告
   （`--shot` 时顺带落一个 diagnostics.json）。投入小、对"AI 一次做成率"
   杠杆大。
6. **G3 控件语义**：不做内建控件，走"变体 + 值绑定"路线：给 setVariant
   补自动状态（pressed/hover）与过渡，加 `ui.bindValue`（slider/switch 的
   连续值/布尔映射到变体或几何）。日期/时间选择器做成官方组件模板
   （design-systems 出设计 + 引擎出滚轮手势）。
7. **G6 音频**（raylib 已有，接线即可）→ GIF → 视频/Lottie 后置。
8. **G2 系统桥**按 benchmark 命中顺序上：分享 → 通知 → 相册 → 相机 →
   定位；深链等 player 立项时一起做。
9. **G9 BaaS 绑定**在 fetch 补齐后做，先出 `examples/` 级的 Supabase
   模式样例，验证后再进 API。

## Benchmark 套件约定（已落地，2026-07）

- 位置：`examples/apps/<name>/`，标准 app 工程；`app.json` 标
  `"benchmark": true` 即被套件收录。
- 设计用 `gen_design.py` 程序化生成 `design.json`（figoedit MCP 可用时
  可改走 MCP + 组件盖章），token 取自 `design-systems/<sys>/design-tokens.json`，
  字体只用系统实有字族（Windows 上 Segoe UI）。
- `app.js` 内 SELFDRIVE 分支：帧 30 截 `<prefix>_home.png`、帧 110 截
  `<prefix>_nav.png`、帧 140 退出；帧 10~100 间用 `ui.tap` 驱动关键路径，
  帧 ~125 前打印 `bench check <名>: got=<实> want=<期>` 明细行和最终一行
  `BENCH: PASS` / `BENCH: FAIL`。localStorage 型 app 序列开头先重置保证幂等。
- 跑法：`python tools/bench.py`（全量）/ `--app <name>`（单个）/
  `--keep-shots`（截图留在 `bench_shots/` 供目检）。退出码 = 失败数。
- 度量：每个 app 记录"AI 一次做成率"（从零到自验通过需要的迭代轮数）。

第一批 5 个已全部落地并 PASS：calculator、pomodoro、habits、settings、shop。
