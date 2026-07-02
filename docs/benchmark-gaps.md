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
| G4 | 跨平台网络 | 6 | ~~Android fetch 直接返回 not supported、无超时~~ **部分修复(2026-07)**：Android 走 JNI→`HttpURLConnection`（后台线程，HTTPS 系统级支持），Windows/Android 均加 15s 连接/读超时；注入口 `figo::setAndroidJNI(vm, activity)` 是通用 JNI 通道（存 JavaVM* + activity 全局引用，G1/G2 复用），figoplay android_main 已注入。编译/打包已验证，**真机运行验证 pending（无 adb 设备）**。仍缺：二进制 body/ArrayBuffer、WebSocket | `script_host.cpp` fetchWorker（Android 分支）、`script.h` setAndroidJNI |
| G5 | 手势与事件面 | 6 | ~~长按、swipe、onScroll、滚动位置读取、事件坐标~~ **部分修复(2026-07)**：`ui.onLongPress`（按住≥0.5s 不动，消费 click）、`ui.onSwipe`（水平 v1，"left"/"right"，位移≥60px 且 \|dx\|>2\|dy\| 且 <0.5s；被水平 drag-scroll 消费的不派发，垂直列表上的横扫仍算——滑动删除可做）、`ui.onScroll`（滚轮/拖拽/惯性/setScroll 全路径，同帧合并、每帧派发）；节点补 `scrollX/scrollY`（可读写）+ `maxScrollX/maxScrollY`；onClick/onHover/onLongPress 回调追加 viewport 坐标 `(node, x, y)`；测试原语 `ui.longPress` / `ui.pointerDown/Move/Up`。派发全走 structureRev/frame 守卫。回归基准 `examples/apps/_events_regress/`。**仍缺**：双击、捏合、多点触控、拖拽(drag&drop)、下拉刷新语义 | `ui.cpp` stepLongPress/maybeFireSwipe/dispatchScrollEvents；`figo_raylib.cpp:77-85` 仍只投单指针 |
| G6 | 动态媒体 | 4 | 音频播放（raylib 有能力未接线）、视频、GIF 动图、Lottie（wasm sjlj 限制已知） | `scene_builder.cpp:250-292`；loaders 仅 svg,ttf,png,jpg,webp |
| G7 | JS 属性/查询面 | 横切 | ~~visible/opacity/primarySizing 只写不可读~~ **部分修复(2026-07)**：visible/opacity/primarySizing/primaryAlign 补了 getter，新增只读 width/height。仍缺：位置(x/y)、颜色/样式读写、建/删节点 | `script_host.cpp` nodeGet |
| G8 | 运行时诊断 | 横切 | 布局溢出/文本截断/字体缺失无警告（font_provider 静默回退，0 输出）；无机器可读诊断通道。直接决定 AI 迭代效率 | `font_provider.cpp:521,541` |
| G9 | 数据层 | 横切 | localStorage 之外无结构化存储；无 BaaS 绑定（目标：`ui.bindList` 接远程数据源） | `script_host.cpp:807-834` |
| G10 | 事件派发中改树 | 横切 | ~~onClick 处理器里调 bindList 是 UB~~ **已修复(2026-07)**：Impl 加 `structureRev`，bindList/setVariant 递增；`fireUp` 在结构变更后立即停止冒泡（同导航消费事件的语义），`pointerUp` 原型链走查与 `pointerMove` 悬空 hit 同步加守卫。注意：处理器**自己的 node 参数**在调 bindList 后仍失效（文档已有约定），需要时重新 find | `ui.cpp` fireUp/bindList/setVariant |
| G11 | 名字寻址不跨页 | 横切 | ~~按名 mutation 只搜当前 frame~~ **已修复(2026-07)**：`findMutable` 与 `findNode` 对齐——当前 frame 优先、回落全文档，跨页 setText/setVisible 生效 | `ui.cpp` findMutable |
| G12 | 文档承诺未兑现 | 横切 | ~~transitionProgress 未绑定 JS~~ **已修复(2026-07)**：`ui.transitionProgress()` 已绑定；语义=转场中 eased [0,1)、**空闲返回 1**，截图门控用 `>= 1` 判定就位（script.h 注释已同步） | `script_host.cpp` ui_transitionProgress |

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

1. ~~**G4 Android fetch**＋顺手补超时~~ **已实现(2026-07)**，见上表 G4 行
   （真机运行验证 pending）。
2. **G1 Android 软键盘（JNI 通道）**：这是架构性决定——`hasCode="false"`
   要改成带一个极薄 Java 层或用 `ANativeActivity` 的 JNIEnv 直调
   InputMethodManager。同一条 JNI 通道也是 G2 所有系统桥的地基，**先打通道，
   再逐个上桥**。剪贴板/密码遮罩属桌面侧小活，可并行。
3. ~~**G5 事件面**：给 JS 加 onLongPress/onSwipe/onScroll + 事件坐标 +
   滚动位置读取~~ **已实现(2026-07)**，见上表 G5 行（v1 范围；双击/捏合/
   多点触控仍缺）。
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
