# figo 产品路线图 — "自然语言快速开发 native app"

> 2026-07 定稿。定位讨论结论：figo 运行时是主线（AI 生成设计 + 薄逻辑层的确定性闭环
> 是核心资产）；**不**默认绑定 Godot——figo2godot / figo2cocos 保留为旁路导出通道
> （"逃生舱口" + 游戏开发者获客），后续若做"Godot 的 AI UI 前端"再单独立项。

产品愿景：让人用自然语言分钟级做出能分发的 native app。
一个 app = 设计(.fig) + 逻辑(.js)，AI 通过 figoedit MCP 改结构化节点、
`figoplay --shot` 截图自验，闭环确定、可自我修正——这是对"AI 生成整套
Flutter/RN 代码"路线的差异化。

三个阶段按依赖顺序推进，**依次实现**：

---

## 阶段一：Benchmark 套件 + 能力面补全

目标：让"任意 AI 在干净环境做一个真实 app"一次成功，而不是只有本仓库的
熟练工作流能跑通。Benchmark 同时是质量标准和引擎 roadmap 的来源。

1. **差距盘点**（第一个交付物）：过一遍 `include/figo/script.h` 的 JS API 与
   运行时控件能力，对照 20 个经典 app 清单标出每个 app 的卡点，按出现频率
   排序产出缺口 backlog → `docs/benchmark-gaps.md`。
2. **Benchmark 套件骨架**：`examples/apps/` 下每个 benchmark app 一个标准
   app 工程（app.json），配期望截图 + SELFDRIVE 自验脚本；一条命令全量跑，
   衡量指标是"AI 一次做成率"与自验通过率。
3. **逐个做 app、撞上什么补什么**：优先级由缺口清单的出现频率决定。
   预判的硬骨头（待盘点确认）：
   - 文本输入完整度：IME/中文输入、光标/选区、软键盘避让（Android）
   - 常用控件语义：开关/滑杆/日期与图片选择器/下拉
   - 设备与系统 API：相机、定位、分享、深链、通知
   - 数据层：`fetch`+`localStorage` 之上的 BaaS 一等公民绑定
     （Supabase/Firebase，目标是 `ui.bindList` 直接接远程数据源）
   - 运行时结构化诊断：布局溢出/字体缺失/节点遮挡能吐机器可读报告
     （audit_design 思路向运行时扩展），降低 AI 迭代的"看图猜错"成本

## 阶段二：分发（web 预览 + Android player）

目标：把"生成完 → 别人能用上"的时间缩到分钟级。没有分发，一切是 demo。

现状盘点：`tools/figmapack.py` 已能打 win/web/android 三端单 app 包
（图标/启动屏/字体打包齐全），`build_web`（emscripten）与 `build_android`
（NativeActivity 免 gradle）管线可用。缺的不是打包，是**分发形态**：

1. **Web 预览链接**：生成完自动打 web 包并托管，产出可分享 URL——
   最短路径的"给别人看"。已知约束：web 画布固定 420x900（raylib RESIZABLE
   在 web 的三方尺寸不一致问题，见 CLAUDE.md），预览场景可接受。
2. **Android player（类 Expo Go）**：通用 figoplay app 上架/侧载一次，
   之后扫码/URL 动态加载 .fig+.js bundle 即刻运行——绕过每 app 装一次 apk。
   技术前提：bundle 下载 + 校验 + 沙箱（QuickJS 天然隔离，补资源路径约束）。
3. **独立 apk/ipa 打包**保持现有 figmapack 路线，iOS 后置（签名/上架
   自动化成本高，等 player 验证需求后再投入）。

## 阶段三：一条命令的开发者产品

目标：把 new-app / figo-design / design-critic 这套 skills + figoedit MCP
抽成可安装的包，`figo new "一个记账 app，黑金配色"` → AI 全自动闭环 →
手机扫码/链接看结果。目标用户：独立开发者和会用 Claude Code 的人。

1. 抽包：CLI 入口 + skills + MCP 配置 + design-systems token 一起分发，
   不依赖本仓库的路径约定。
2. **陌生环境实测**：干净的 Claude Code 会话（无项目记忆）只给打包好的
   工具和一句话文档，看 AI 能否独立做出能用的 app——每个卡点回填
   阶段一的缺口清单。
3. 跑通后再谈托管生成服务与计费（云端 headless 闭环 → 分享链接）。

---

## 里程碑与度量

| 阶段 | 完成标准 |
|---|---|
| 一 | 缺口清单产出；≥20 个 benchmark app 全部自验通过；一次做成率有基线数字 |
| 二 | 生成完 1 分钟内拿到可分享 web 链接；Android player 扫码跑任意 benchmark app |
| 三 | 陌生环境的 AI 用打包工具独立做出 ≥3 类 app 且自验通过 |

风险与原则：
- 闭环的确定性（毫秒级截图自验、结构化节点操作）是最值钱的资产，
  任何改动不得使其退化。
- 能力面补全以 benchmark 撞出的真实缺口驱动，不凭空设计 API。
- Godot/Cocos 导出线只做保真度维护，不加新特性投入，除非游戏 UI
  前端立项。
