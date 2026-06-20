# agent-memory

Version-controlled mirror of the Claude Code agent memory for this project, so
the accumulated knowledge (design decisions, gotchas, the React→Godot pipeline
fix log) travels with the repo.

- `MEMORY.md` — index (one line per note).
- `react-to-psd-pipeline.md` — the React/HTML → canvas.json → Godot pipeline:
  design decisions, every fidelity fix, and the gotchas behind them.
- `fapp-build-dir-gotcha.md`, `figoplay-shot-basename.md` — build/runtime gotchas.

Live working copy (auto-loaded by Claude Code, may be ahead of this mirror):
`~/.claude/projects/D--work-open-fapp/memory/`. Re-sync with:

```
cp ~/.claude/projects/D--work-open-fapp/memory/*.md docs/agent-memory/
```
