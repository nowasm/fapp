# G3 设计稿：控件语义 = 变体 + 值绑定（评审用，未实现）

> 缺口背景见 [benchmark-gaps.md](benchmark-gaps.md) G3。原则已定：**不做内建
> 控件库**——外观永远归设计（.fig 节点/变体），引擎只补"手势 → 值"的那段
> 语义。本稿定 API 形态，过目后再实现。

## 为什么不是内建控件

内建 switch/slider 意味着引擎带一套自己的外观，和"设计即运行时"的核心主张
冲突（AI/设计师在 Figma 里画的才算数）。benchmark 实测也支持：settings 的
switch、shop 的 stepper、todo 的勾选都用"设计变体/双态节点 + 脚本"拼出来了，
缺的只有两样——**连续值手势**（slider 拖不了）和**状态切换的自动化/动画**
（每个 hover/pressed 都要手写、变体切换是瞬切）。

## API 提案（三件套）

### 1. `ui.bindSlider(track, opts)` — 连续值手势

```js
ui.bindSlider("Volume Track", {
    min: 0, max: 100, step: 1, value: 30,
    knob: "Volume Knob",      // track 子节点名：引擎沿主轴摆放
    fill: "Volume Fill",      // 可选：引擎按比例缩放的已填充段
    axis: "x",                // 默认 "x"
    onChange(v, committed) {}, // 拖动中 committed=false，松手 true
});
```

- 引擎接管 track 上的 pointer 手势（拖拽不再冒泡为滚动/滑动），把位置映射
  到 [min,max]（含 step 吸附），**引擎侧**移动 knob / 缩放 fill——这正是 JS
  没有几何写权限的补位：几何变更留在 C++，JS 只见值。
- knob/fill 的样子完全来自设计；没有 knob 也成立（进度条=只读，传
  `readonly: true` 禁手势、脚本 `ui.setValue(track, v)` 驱动）。
- 同一机制覆盖：音量/进度条/亮度/评分拖条。

### 2. `ui.autoStates(name, map?)` — 交互状态自动切变体

```js
ui.autoStates("Primary Button");   // 约定优先：组件集里有 State=Hover /
                                   // State=Pressed / State=Default 就自动接管
ui.autoStates("Card", { hover: "Elevation=Raised", base: "Elevation=Flat" });
```

- 引擎在 hover/press/release 时自动 `setVariant`，脚本不再为每个按钮写
  onHover 样板（现在 benchmark 里每个 app 都在重复 `setOpacity(0.85)`）。
- 约定命名 `State=Default|Hover|Pressed`，与 Figma 社区习惯一致，AI 生成
  设计时可直接照此起变体名。

### 3. `ui.setVariant(..., {duration})` — 变体过渡

```js
ui.setVariant("Toggle", "State", "On", { duration: 0.15 });  // 溶解过渡
```

- v1 只做 dissolve（新旧子树快照交叉淡化，复用转场的贴图合成通道，
  不逐属性插值）；位置/尺寸补间（smart animate）后置。
- autoStates 的切换默认带 0.12s 过渡。

## 显式非目标（v1）

- 日期/时间滚轮选择器：等 slider 落地后走"滚动吸附"路线
  （`node.snapToChildren` + `onScrollEnd`），单独立项。
- 下拉菜单/popover：用现有 frame 导航或 setVisible 遮罩层可拼，不进引擎。
- 双向数据绑定/响应式：明确不做，保持"事件 + 显式 set"的最小心智。

## 实现顺序与验收

1. `bindSlider`（含 readonly 进度条）→ 解锁 music-player benchmark 的
   进度拖拽（缺口矩阵 #16）；
2. `autoStates` → 回改 2-3 个现有 benchmark app 删掉手写 hover 样板；
3. 变体 dissolve 过渡 → settings 的 switch 不再瞬跳。

每步照例：render/layout test + bench 全绿 + 新断言固化进 `_events_regress`
或新 `_value_regress`。

## 开放问题（请拍板）

- A. `bindSlider` 的命名：`bindSlider` vs 更泛的 `bindValue`？（倾向
  bindSlider——直白；将来真有第二种连续控件再泛化）
- B. `autoStates` 是否默认对**所有**带 State 变体的实例自动生效（零调用）？
  （倾向显式调用——魔法行为会让 AI 难以解释截图差异）
- C. slider 手势与滚动的冲突消解：track 在可滚列表里时，横向 slider +
  纵向滚动共存（按轴分流），同轴时 slider 优先。是否可接受？
