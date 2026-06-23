---
name: figo2cocos
description: 把一个 figo 设计（.fig / canvas.json / 设计 JSON）导成 Cocos Creator 3.x 可直接打开的预制体（.prefab + 贴图 + .meta）。当用户要"把这个设计/页面导成 Cocos"、"figo 转 Cocos Creator 预制体/prefab"、"生成 .prefab"，或提到 figo2cocos / psd2prefab 时使用。覆盖：构建 figo2cocos → 一条命令转 → 拖进 Cocos assets/ 打开自验 → 看图迭代。
---

# 把 figo 设计变成 Cocos Creator 3.x 预制体

一条链，复用 figo2godot 的渲染/烘焙机制，换上从 psd2prefab 移植的 Cocos 序列化：

```
.fig / canvas.json / 设计 JSON ──figo2cocos──► <name>.prefab + textures/*.png(+.meta)
```

节点树用 figo 自己的 **ThorVG 光栅化**烘焙复杂图形，所以贴图和 figo 运行时**逐像素一致**。
`.prefab` 是一串 `__id__` 互引的引擎对象数组（`cc.Prefab` → `cc.Node` →
`cc.UITransform`/`cc.Sprite`/`cc.Label`…），格式对齐 **Cocos Creator 3.4.2+**。

## 节点映射

| figo 节点 | Cocos 产物 |
|---|---|
| TEXT | `cc.Node` + `cc.Label`（字符串/字号/颜色/对齐/换行，系统字体） |
| 纯色矩形 / 容器底色 | `cc.Node` + `cc.Sprite`（共享 `white.png`，用 `_color` 染色——免分辨率） |
| 椭圆/矢量/渐变/图片/描边/外发光/圆角面板 | `cc.Node` + `cc.Sprite`（烘焙 PNG；够大且可拉伸的走 9 宫格 `_type=1`） |
| 容器 | `cc.Node`，挂底色 sprite + 子节点 |
| 空容器 | `cc.Node`（透传） |

**坐标翻转**：每个节点 anchor 取 `(0,1)`（左上角），`_lpos = (relX, -relY)`，
`cc.UITransform._contentSize` 用 figo 的盒子尺寸——Figma(Y 向下) → Cocos(Y 向上)
逐层正确复合。

## 0. 先确认（一次性）

- **figo2cocos 在吗**：在 `build/` 下编出来（macOS 见 CLAUDE.md，秒级增量）：
  ```bash
  cd build && cmake . && cmake --build . --target figo2cocos -j
  ```
  Windows 必须在 vcvars64 下用 CLAUDE.md 的 PowerShell cmd 包装跑。
- **输入是什么**：`figo2cocos` 吃 `FigmaUI::fromFile` 认的所有格式——`.fig`（自动
  转 canvas.json 缓存）、fig2json 的 `canvas.json`、Figma REST JSON、或 figo app
  工程的 `design.json`。

## 1. 一条命令转

```bash
build/figo2cocos <input> [outDir] [--frame NAME] [--scale N]
```

- `outDir` 默认 `cocos_out`。每个顶层 frame 出一个 `<Frame>.prefab`，贴图共享
  `<outDir>/textures/`。
- `--frame NAME` 只导一个 frame；不给则全导。
- `--scale N` sprite 超采样倍率（默认 2，更清晰；9 宫格内部按 1x 烘焙保证切角锐利）。

例：
```bash
build/figo2cocos examples/assets/sample_ui.json cocos_out
```

## 2. 拖进 Cocos 工程

把 `outDir` 整个目录（`*.prefab` + `*.prefab.meta` + `textures/`）拷进目标
Cocos Creator 3.x 工程的 `assets/` 下某个子目录。**`.meta` 必须一起拷**——里面是
贴图/sprite-frame 的 UUID，prefab 靠 `uuid@f9941` 引用它们；少了 .meta，Cocos 会
重新分配 UUID，引用就断了。

打开 Cocos Creator，等它导入资源，把 `<Frame>.prefab` 拖进一个有 Canvas 的场景里，
即可看到还原的界面。

## 3. 自验

没有无头 Cocos，先做**结构自检**（再人工开 Cocos 目检）：

```bash
python3 - <<'PY'
import json,glob,os
os.chdir("cocos_out")
tex={json.load(open(m))["uuid"] for m in glob.glob("textures/*.png.meta")}
for pf in glob.glob("*.prefab"):
    a=json.load(open(pf)); n=len(a); bad=[]
    def walk(o,w):
        if isinstance(o,dict):
            if "__id__" in o and not(0<=o["__id__"]<n): bad.append(w)
            for k,v in o.items(): walk(v,f"{w}.{k}")
        elif isinstance(o,list):
            [walk(v,f"{w}[{i}]") for i,v in enumerate(o)]
    [walk(o,f"[{i}]") for i,o in enumerate(a)]
    assert a[0]["__type__"]=="cc.Prefab" and a[0]["data"]["__id__"]==1
    for o in a:  # 每个 sprite frame 的贴图 uuid 都得有 meta
        if o.get("__type__")=="cc.Sprite":
            assert o["_spriteFrame"]["__uuid__"].split("@")[0] in tex
    print(pf,"OK" if not bad else f"DANGLING {bad}")
print("done")
PY
```

全 `OK` 表示对象图自洽、所有 sprite-frame 引用都能落到一张有 .meta 的贴图上。
人工目检重点：层级顺序、文字位置（Label 锚点左上 + 顶对齐时短文本可能偏上）、
9 宫格面板拉伸是否走样。

### 真机自验：cocos-cli MCP（已端到端验证）

`cocos-cli`（官方 CLI，`/Users/t5/cocosProjs/cocos/cocos-cli`）带一个 `start-mcp-server`
命令，能用**真实引擎**加载工程并通过 MCP 程序化校验——比结构自检强得多，能确认 prefab
真的反序列化成活节点。已用 sample_ui 跑通：prefab 导入为 `cc.Prefab`、贴图导入为
sprite-frame、引用全部解析、层级/组件/属性（Label 文案字号色、UITransform 锚点 (0,1)、
Y 翻转坐标）全对、可实例化进场景并保存。

```bash
# 一次性：构建 cocos-cli（首次 ~10min，详见其 README：npm run init && npm install && npm run build）
# 起服务（Streamable HTTP，无状态，协议版本必须 2025-06-18）
node <cocos-cli>/dist/cli.js start-mcp-server --project <cocosProj> --port 9527
# 等日志出现 "Server is running on: http://localhost:9527/mcp"
```

调用约定（curl JSON-RPC）：`POST /mcp`，头
`Content-Type: application/json` + `Accept: application/json, text/event-stream`，
先 `initialize`（`protocolVersion:"2025-06-18"`，否则 400），无状态不需 session-id。
关键工具：`assets-refresh {dir}` 导入 → `assets-query-asset-info {urlOrUUIDOrPath}`
确认 `type:"cc.Prefab", invalid:false` → `scene-open {options:{dbURLOrUUID, includeChildren,
includeComponents}}` 直接打开 prefab 看活节点树 → `scene-query-component {component:{path}}`
读 Label/UITransform/Sprite 的真实属性 → `scene-create` + `scene-create-node-by-asset
{options:{dbURL, canvasRequired:true}}` + `scene-save` 验证实例化落地。共 51 个工具
（scene/prefab/asset/builder/file）。注意 cocos-cli 引擎是 4.0-alpha，能读我们的 3.8 格式。

> 文本位置/对齐（已修）：早期 Label 一律 `overflow=NONE`，Cocos 会把节点缩成文字大小、
> 丢掉作者设的文本框，导致居中标题塌到框左上角（anchor 0,1）——"文字位置不对、没对齐
> 设计稿"。现按 figo `autoResize` 映射 `_overflow`：`WIDTH_AND_HEIGHT`→NONE(贴合文字)、
> `HEIGHT`→RESIZE_HEIGHT(定宽换行增高)、`NONE`/`TRUNCATE`→**CLAMP**(锁住文本框、框内对齐、
> 超出裁剪)，文字即按设计稿在框内对齐定位。
>
> 排查时注意 cocos MCP 的两个怪癖：(1) 同一节点 `scene-query-node` 用场景实例路径
> `Canvas/<prefab>/...`，`scene-query-component` 在 prefab 实例里常 404/500——位置用
> query-node 验，属性值优先直接读导出的 .prefab JSON；(2) 工具返回是**双层包裹**
> （`content[0].text` 里又是一层 `{"result":{"data":...}}`），解析要再剥一层。

## 关键约定与坑

- **只产 v3.4.2+ 格式**（单一代码路径）。要兼容 Cocos 2.4.x 需另加 `_trs`/直挂
  `_contentSize` 那套字段（psd2prefab 里有，按需移植）。
- **每帧一个自包含 prefab**，节点全部内联——v1 **不做嵌套 prefab 提取**（Cocos
  PrefabInstance 的 override/mountedChildren 序列化重得多）。重复卡片/按钮想复用，
  目前在 Cocos 里手动抽 prefab。
- **系统字体**：Label 走 `_isSystemFontUsed`，不打包 TTF。要自定义字体得在 Cocos
  里给 Label 指 Font 资源。
- **轴对齐**：v1 不导旋转（和 psd2prefab 一致）。绝大多数 UI 不旋转；个别旋转元素
  会按未旋转的盒子摆放（烘焙类 sprite 的旋转其实已烤进像素）。
- **UUID 确定性**：贴图 UUID 由内容哈希派生、prefab/节点 fileId 由帧名+计数派生，
  所以**重跑输出稳定**、同图自动去重。但若你已在 Cocos 里改过这些 .meta 的 UUID，
  重跑会覆盖回派生值。
- 烘焙/去重/9 宫格逻辑直接抄自 `apps/figo2godot/main.cpp`；性能或像素问题先对照那边。
