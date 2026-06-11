#pragma once
// figmalib — responsive layout engine.
//
// Reflows a frame subtree to a target size using Figma's constraints
// (LEFT/CENTER/RIGHT/STRETCH/SCALE per axis) and auto-layout stacks
// (direction, padding, spacing, grow, align, hug sizing). Geometry is always
// recomputed from the authored snapshot (Node::base*), so repeated resizes
// are stable and never accumulate error.

#include <functional>

#include "document.h"

namespace figmalib {

// Restore the authored geometry of the whole subtree.
void resetLayout(Node& root);

// Resize `frame` to width × height and reflow its subtree.
void layoutFrame(Node& frame, float width, float height);

// Re-run layout inside `node` at its current size, re-deriving hug axes from
// the (possibly changed) children — for runtime child mutations like
// FigmaUI::bindList. Ancestors are not updated; callers in Reflow mode
// should reflow the frame instead.
void relayoutNode(Node& node);

// Text measurement hook: wraps the node's text at maxWidth (<= 0 → no wrap)
// and reports the content extent. Installed by the Renderer (process-global;
// the most recently constructed renderer owns it) so layoutFrame can size
// auto-height text from real font metrics. Without a measurer, text keeps
// its authored box (the pre-measurement behavior).
using TextMeasurer =
    std::function<bool(const Node&, float maxWidth, float& outW, float& outH)>;
void setTextMeasurer(TextMeasurer fn);

}  // namespace figmalib
