# design-systems — 审美知识库

把成熟产品的设计系统（配色 / 字阶 / 间距 / 圆角 / 阴影）作为**纯文本 token**
喂给 AI，让它通过 figoedit MCP 建出来的设计有审美下限，而不是每次从零拍脑袋。

来源是 [Open Design](https://github.com/nexu-io/open-design)（Apache-2.0），我们只
vendor 了**可移植的知识**——12 套设计系统的 token + 4 个设计流程 skill，丢掉了上游
那套 HTML/CSS/GSAP 产物（我们渲染的是 Figma 节点→ThorVG，不是浏览器）。署名见
[`NOTICE`](NOTICE)。

## 目录

```
<system>/                     # x12，每套：
  DESIGN.md                   #   人读的设计指南 + "Agent Prompt Guide"
  design-tokens.json          #   机器读的 token（W3C Design Tokens schema）★主力
  tokens.css                  #   token 的 CSS 变量原始声明
  USAGE.md                    #   这套系统怎么用
skills/                       # 设计流程 skill（SKILL.md）
  design-review/              #   视觉审计→修复（套进我们的截图自检闭环）
  apple-hig/  color-expert/  brand-guidelines/
  taste/                      #   反模板审美纪律（vendor 自 taste-skill, MIT）
                              #   被 .claude/skills/figo-design 默认加载
TOKEN_MAPPING.md              # design-tokens.json -> figoedit 调用的映射规则 ★必读
NOTICE / LICENSE-APACHE-2.0.txt
```

精选的 12 套覆盖风格谱系（偏重金融，因为旗舰 demo 是 wallet）：

| 系统 | 风格 | 适用 |
|---|---|---|
| `linear-app` | 极简深色、开发者工具 | 工具类、dashboard |
| `stripe` | 干净浅色、克制 | fintech、SaaS |
| `apple` | 精致、HIG | 通用、iOS 风 |
| `notion` | 清爽文档 | 内容、编辑器 |
| `spotify` | 深色媒体 | 音乐、视频 |
| `coinbase` / `revolut` | 金融蓝、机构感 / 大胆渐变 | **钱包、交易** |
| `duolingo` | 活泼、圆润 | 教育、游戏化 |
| `airbnb` | 亲和消费级 | 电商、出行 |
| `material` | Google 基线 | Android 标准 |
| `neobrutalism` | 硬边粗描边 | 个性、潮流 |
| `framer` | 现代营销页 | landing |

## AI 怎么用（接入 Phase 1 创作闭环）

1. **选系统**：app 工程的 `app.json` 里写 `"designSystem": "coinbase"`。
2. **读 token**：建设计前先读 `coinbase/design-tokens.json` + `DESIGN.md` 的
   "Agent Prompt Guide" 段。
3. **建节点**：按 [`TOKEN_MAPPING.md`](TOKEN_MAPPING.md) 把 token 折算成 figoedit
   的 `create_node` / `update_nodes` 属性（填充色、圆角、文本样式、阴影）。
4. **审计**：`figoplay <app> --shot` 出截图后，按 `skills/design-review` 对着这套
   系统的 token 自评（配色/间距/层级对不对），再迭代。

> 上游持续在加新系统/skill。要扩充时重新 sparse-clone open-design 拷对应目录即可，
> 记得同步更新 `NOTICE` 的清单。
