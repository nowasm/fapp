#pragma once
// SVG document importer: turns SVG markup into a figo node subtree (a FRAME
// holding one VECTOR per drawable element). Transforms are carried on each
// node's relativeTransform and geometry is kept in local coordinates, so the
// renderer's existing path + gradient machinery draws everything 1:1.
//
// Scope: path / rect / circle / ellipse / line / polygon / polyline / g, with
// full transform stacks, presentation-attribute inheritance, solid fills and
// strokes, and best-effort linear/radial gradients. Out of scope (skipped):
// text, <image>, <use>, clipPath/mask/filter, patterns, symbols.

#include <map>
#include <memory>
#include <string>

#include <figo/document.h>

namespace figoedit {

struct SvgImportOptions {
    float x = 0, y = 0;          // where the frame sits in its parent
    float width = 0, height = 0; // target size; <= 0 keeps the SVG's viewBox size
    std::string name;            // frame name; empty → "SVG"
    // Recolor hooks (applied to solid fills/strokes after parsing):
    std::string monochrome;            // "#RRGGBB" → force every solid paint to this
    std::map<std::string, std::string> palette;  // "rrggbb"(lowercase) → "#RRGGBB"
};

// Parse `svg` markup into a FRAME node (caller owns it, assigns ids, reparents).
// Returns nullptr and fills `err` on failure.
std::unique_ptr<figo::Node> importSvg(const std::string& svg,
                                      const SvgImportOptions& opt, std::string& err);

}  // namespace figoedit
