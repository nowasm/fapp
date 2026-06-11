#pragma once
// Builds a ThorVG scene graph from a figmalib node tree.

#include <string>

#include <thorvg.h>

#include "figmalib/document.h"
#include "font_provider.h"

namespace figmalib {

struct BuildContext {
    FontProvider* fonts = nullptr;
    std::string imageDir;
};

// Returns a scene for the node subtree, or nullptr if the node is hidden.
// `isRoot` drops the node's own canvas-position transform so the frame renders
// at origin. Side effect: fills in node.absoluteTransform (in root-frame space)
// for hit-testing.
tvg::Scene* buildNodeScene(Node& node, const Mat23& parentAbs, BuildContext& ctx,
                           bool isRoot = false);

// Measures a TEXT node's content with the same tokenize/wrap pass the
// renderer uses: wrapped at maxWidth (<= 0 → no wrapping). Outputs the widest
// line and the total line-box height. Returns false when fonts are missing.
bool measureTextNode(const Node& n, float maxWidth, BuildContext& ctx,
                     float& outWidth, float& outHeight);

}  // namespace figmalib
