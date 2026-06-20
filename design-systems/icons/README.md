# icons — 开箱即用的矢量图标库

35 个最常用的 UI 图标，取自 [Lucide](https://lucide.dev)（lucide-static **v1.21.0**，ISC
许可，见 `LICENSE`）。每个都是 24×24、`stroke="currentColor"` 的描边图标——干净、可缩放、
**可改色**，正好喂给 figoedit MCP 的 `import_svg`。

## 怎么用

按名导入，并用 `monochrome` 把描边色统一成当前设计系统的前景/强调 token：

```jsonc
// figoedit MCP: import_svg
{ "path": "design-systems/icons/search.svg",
  "name": "icon-search", "parentId": "<容器>", "x": 12, "y": 12,
  "width": 20, "height": 20,
  "monochrome": "#e2e8f0" }   // = 该系统的 --fg / --muted / --accent
```

- 这些图标只有描边、没有填充，所以 `monochrome` 改的是描边色（图标的可见颜色）。
- 尺寸随便给（矢量无损）。常用：导航/按钮内 16–24，强调图标 28–40。
- 颜色对接 token：见 `../TOKEN_MAPPING.md`，把 `--fg`/`--muted`/`--accent` 的值填进
  `monochrome`。
- 想批量改色或对照已有设计审查，配合 `audit_design` / `/design-critic`。

## 清单（35）

`arrow-left` `arrow-right` `bell` `calendar` `check` `chevron-down` `chevron-left`
`chevron-right` `chevron-up` `circle-alert` `circle-check` `clock` `download` `eye`
`eye-off` `heart` `house` `info` `lock` `mail` `menu` `minus` `pause` `pencil` `play`
`plus` `search` `settings` `share-2` `star` `trash-2` `upload` `user` `users` `x`

## 加更多图标

Lucide 有 1500+ 图标，命名一致。要哪个直接拉（保持版本一致）：

```bash
curl -sSL "https://unpkg.com/lucide-static@1.21.0/icons/<name>.svg" \
  -o design-systems/icons/<name>.svg
```

名字查 https://lucide.dev/icons 。`import_svg` 能吃 Lucide 的全部元素
（path/circle/rect/line/polyline + 顶部的 license 注释）。
