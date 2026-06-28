// Figma REST API JSON → figo::Document.
//
// Schema reference: https://www.figma.com/developers/api#node-types
// Geometry path data ("fillGeometry"/"strokeGeometry") is present when the
// file was fetched with ?geometry=paths.

#include "figo/parser.h"

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

#include <nlohmann/json.hpp>

namespace figo {

using json = nlohmann::json;

namespace {

float jfloat(const json& j, const char* key, float def = 0.0f) {
    auto it = j.find(key);
    if (it == j.end() || !it->is_number()) return def;
    return it->get<float>();
}

bool jbool(const json& j, const char* key, bool def) {
    auto it = j.find(key);
    if (it == j.end() || !it->is_boolean()) return def;
    return it->get<bool>();
}

std::string jstr(const json& j, const char* key, const std::string& def = {}) {
    auto it = j.find(key);
    if (it == j.end() || !it->is_string()) return def;
    return it->get<std::string>();
}

NodeType parseNodeType(const std::string& t) {
    static const std::unordered_map<std::string, NodeType> map = {
        {"DOCUMENT", NodeType::Document},
        {"CANVAS", NodeType::Canvas},
        {"FRAME", NodeType::Frame},
        {"GROUP", NodeType::Group},
        {"SECTION", NodeType::Section},
        {"RECTANGLE", NodeType::Rectangle},
        {"ELLIPSE", NodeType::Ellipse},
        {"LINE", NodeType::Line},
        {"VECTOR", NodeType::Vector},
        {"BOOLEAN_OPERATION", NodeType::BooleanOperation},
        {"STAR", NodeType::Star},
        {"REGULAR_POLYGON", NodeType::RegularPolygon},
        {"TEXT", NodeType::Text},
        {"COMPONENT", NodeType::Component},
        {"COMPONENT_SET", NodeType::ComponentSet},
        {"INSTANCE", NodeType::Instance},
        {"SLICE", NodeType::Slice},
    };
    auto it = map.find(t);
    return it == map.end() ? NodeType::Unknown : it->second;
}

Color parseColor(const json& j, float opacity = 1.0f) {
    Color c;
    c.r = jfloat(j, "r");
    c.g = jfloat(j, "g");
    c.b = jfloat(j, "b");
    c.a = jfloat(j, "a", 1.0f) * opacity;
    return c;
}

Paint parsePaint(const json& j) {
    Paint p;
    const std::string type = jstr(j, "type", "SOLID");
    if (type == "SOLID") p.type = PaintType::Solid;
    else if (type == "GRADIENT_LINEAR") p.type = PaintType::GradientLinear;
    else if (type == "GRADIENT_RADIAL") p.type = PaintType::GradientRadial;
    else if (type == "GRADIENT_ANGULAR") p.type = PaintType::GradientAngular;
    else if (type == "GRADIENT_DIAMOND") p.type = PaintType::GradientDiamond;
    else if (type == "IMAGE") p.type = PaintType::Image;

    p.visible = jbool(j, "visible", true);
    p.opacity = jfloat(j, "opacity", 1.0f);

    if (auto it = j.find("color"); it != j.end() && it->is_object()) {
        p.color = parseColor(*it);
    }
    if (auto it = j.find("gradientStops"); it != j.end() && it->is_array()) {
        for (const auto& s : *it) {
            GradientStop stop;
            stop.position = jfloat(s, "position");
            if (auto c = s.find("color"); c != s.end()) stop.color = parseColor(*c);
            p.stops.push_back(stop);
        }
    }
    if (auto it = j.find("gradientHandlePositions"); it != j.end() && it->is_array()) {
        const auto& h = *it;
        auto get = [&](size_t i, std::array<float, 2>& out) {
            if (i < h.size() && h[i].is_object()) {
                out[0] = jfloat(h[i], "x");
                out[1] = jfloat(h[i], "y");
            }
        };
        get(0, p.handle0);
        get(1, p.handle1);
        get(2, p.handle2);
    }
    p.imageRef = jstr(j, "imageRef");
    p.imageScaleMode = jstr(j, "scaleMode", "FILL");
    return p;
}

void parsePaints(const json& j, const char* key, std::vector<Paint>& out) {
    auto it = j.find(key);
    if (it == j.end() || !it->is_array()) return;
    for (const auto& p : *it) out.push_back(parsePaint(p));
}

void parseGeometry(const json& j, const char* key, std::vector<PathGeometry>& out) {
    auto it = j.find(key);
    if (it == j.end() || !it->is_array()) return;
    for (const auto& g : *it) {
        PathGeometry pg;
        pg.path = jstr(g, "path");
        pg.evenOdd = jstr(g, "windingRule", "NONZERO") == "EVENODD";
        if (!pg.path.empty()) out.push_back(pg);
    }
}

void parseEffects(const json& j, std::vector<Effect>& out) {
    auto it = j.find("effects");
    if (it == j.end() || !it->is_array()) return;
    for (const auto& e : *it) {
        Effect fx;
        const std::string type = jstr(e, "type");
        if (type == "DROP_SHADOW") fx.type = Effect::Type::DropShadow;
        else if (type == "INNER_SHADOW") fx.type = Effect::Type::InnerShadow;
        else if (type == "LAYER_BLUR") fx.type = Effect::Type::LayerBlur;
        else if (type == "BACKGROUND_BLUR") fx.type = Effect::Type::BackgroundBlur;
        else continue;
        fx.visible = jbool(e, "visible", true);
        if (auto c = e.find("color"); c != e.end()) fx.color = parseColor(*c);
        if (auto o = e.find("offset"); o != e.end()) {
            fx.offsetX = jfloat(*o, "x");
            fx.offsetY = jfloat(*o, "y");
        }
        fx.radius = jfloat(e, "radius");
        fx.spread = jfloat(e, "spread");
        out.push_back(fx);
    }
}

void parseTextStyle(const json& j, TextStyle& ts) {
    auto it = j.find("style");
    if (it == j.end() || !it->is_object()) return;
    const json& s = *it;
    ts.fontFamily = jstr(s, "fontFamily", ts.fontFamily);
    ts.fontPostScriptName = jstr(s, "fontPostScriptName");
    ts.fontSize = jfloat(s, "fontSize", ts.fontSize);
    ts.fontWeight = static_cast<int>(jfloat(s, "fontWeight", 400));
    ts.italic = jbool(s, "italic", false);
    ts.lineHeightPx = jfloat(s, "lineHeightPx", 0);
    ts.letterSpacing = jfloat(s, "letterSpacing", 0);
    const std::string ah = jstr(s, "textAlignHorizontal", "LEFT");
    if (ah == "CENTER") ts.alignH = TextStyle::AlignH::Center;
    else if (ah == "RIGHT") ts.alignH = TextStyle::AlignH::Right;
    else if (ah == "JUSTIFIED") ts.alignH = TextStyle::AlignH::Justified;
    const std::string av = jstr(s, "textAlignVertical", "TOP");
    if (av == "CENTER") ts.alignV = TextStyle::AlignV::Center;
    else if (av == "BOTTOM") ts.alignV = TextStyle::AlignV::Bottom;
    ts.autoResize = jstr(s, "textAutoResize", "NONE");
    ts.truncateEnding = jstr(s, "textTruncation") == "ENDING";
    ts.maxLines = static_cast<int>(jfloat(s, "maxLines", 0));
}

Constraint parseConstraint(const std::string& v) {
    // REST uses LEFT/TOP, RIGHT/BOTTOM, LEFT_RIGHT/TOP_BOTTOM.
    if (v == "CENTER") return Constraint::Center;
    if (v == "RIGHT" || v == "BOTTOM" || v == "MAX") return Constraint::Max;
    if (v == "LEFT_RIGHT" || v == "TOP_BOTTOM" || v == "STRETCH") return Constraint::Stretch;
    if (v == "SCALE") return Constraint::Scale;
    return Constraint::Min;
}

AutoLayout::Align parseAlignItems(const std::string& v) {
    if (v == "CENTER") return AutoLayout::Align::Center;
    if (v == "MAX") return AutoLayout::Align::Max;
    if (v == "SPACE_BETWEEN") return AutoLayout::Align::SpaceBetween;
    if (v == "BASELINE") return AutoLayout::Align::Baseline;
    return AutoLayout::Align::Min;
}

void parseLayout(const json& j, Node& node) {
    if (auto it = j.find("constraints"); it != j.end() && it->is_object()) {
        node.constraintH = parseConstraint(jstr(*it, "horizontal", "LEFT"));
        node.constraintV = parseConstraint(jstr(*it, "vertical", "TOP"));
    }

    const std::string mode = jstr(j, "layoutMode", "NONE");
    if (mode == "HORIZONTAL" || mode == "VERTICAL") {
        AutoLayout& al = node.autoLayout;
        al.mode = mode == "HORIZONTAL" ? AutoLayout::Mode::Horizontal
                                       : AutoLayout::Mode::Vertical;
        al.primarySizing = jstr(j, "primaryAxisSizingMode", "AUTO") == "FIXED"
                               ? AutoLayout::Sizing::Fixed
                               : AutoLayout::Sizing::Hug;
        al.counterSizing = jstr(j, "counterAxisSizingMode", "AUTO") == "FIXED"
                               ? AutoLayout::Sizing::Fixed
                               : AutoLayout::Sizing::Hug;
        al.primaryAlign = parseAlignItems(jstr(j, "primaryAxisAlignItems", "MIN"));
        al.counterAlign = parseAlignItems(jstr(j, "counterAxisAlignItems", "MIN"));
        al.paddingLeft = jfloat(j, "paddingLeft");
        al.paddingRight = jfloat(j, "paddingRight");
        al.paddingTop = jfloat(j, "paddingTop");
        al.paddingBottom = jfloat(j, "paddingBottom");
        al.itemSpacing = jfloat(j, "itemSpacing");
        al.counterSpacing = jfloat(j, "counterAxisSpacing");
        al.wrap = jstr(j, "layoutWrap", "NO_WRAP") == "WRAP";
    }

    node.layoutGrow = jfloat(j, "layoutGrow", 0);
    node.layoutAlignStretch = jstr(j, "layoutAlign") == "STRETCH";
    node.layoutAbsolute = jstr(j, "layoutPositioning") == "ABSOLUTE";

    const std::string overflow = jstr(j, "overflowDirection", "NONE");
    if (overflow == "HORIZONTAL_SCROLLING") {
        node.scrollDirection = ScrollDirection::Horizontal;
    } else if (overflow == "VERTICAL_SCROLLING") {
        node.scrollDirection = ScrollDirection::Vertical;
    } else if (overflow == "HORIZONTAL_AND_VERTICAL_SCROLLING") {
        node.scrollDirection = ScrollDirection::Both;
    }
    node.scrollFixed = jbool(j, "isFixed", false) || jstr(j, "scrollBehavior") == "FIXED";

    node.transitionNodeId = jstr(j, "transitionNodeID");
    node.transitionType = jstr(j, "transitionType");  // absent in REST: keep ""
    node.transitionDuration = jfloat(j, "transitionDuration", 0);

    node.minWidth = jfloat(j, "minWidth");
    node.maxWidth = jfloat(j, "maxWidth");
    node.minHeight = jfloat(j, "minHeight");
    node.maxHeight = jfloat(j, "maxHeight");

    // CSS animation (web2canvas): opacity + 2D scale keyframes. Replayed by
    // figo2godot; ignored everywhere else.
    if (auto it = j.find("anim"); it != j.end() && it->is_object()) {
        NodeAnim a;
        a.dur = jfloat(*it, "dur", 0);
        a.delay = jfloat(*it, "delay", 0);
        a.iter = static_cast<int>(jfloat(*it, "iter", 1));
        a.ease = jstr(*it, "ease", "linear");
        if (auto p = it->find("pivot"); p != it->end() && p->is_array() && p->size() >= 2) {
            a.pivotX = (*p)[0].get<float>();
            a.pivotY = (*p)[1].get<float>();
        }
        if (auto ks = it->find("keys"); ks != it->end() && ks->is_array()) {
            for (const auto& kj : *ks) {
                if (!kj.is_object()) continue;
                AnimKey k;
                k.t = jfloat(kj, "t", 0);
                if (kj.contains("opacity") && kj["opacity"].is_number()) {
                    k.hasOpacity = true;
                    k.opacity = kj["opacity"].get<float>();
                }
                if (auto sc = kj.find("scale"); sc != kj.end() && sc->is_array() && sc->size() >= 2) {
                    k.hasScale = true;
                    k.sx = (*sc)[0].get<float>();
                    k.sy = (*sc)[1].get<float>();
                }
                a.keys.push_back(k);
            }
        }
        if (a.dur > 0 && a.keys.size() >= 2) node.anim = std::move(a);
    }
}

// REST rich text: characterStyleOverrides lists a style id per UTF-16 code
// unit; styleOverrideTable maps ids to partial style objects. Mapped onto
// UTF-8 byte ranges by walking codepoints.
void parseTextRuns(const json& j, Node& node) {
    auto so = j.find("characterStyleOverrides");
    auto tbl = j.find("styleOverrideTable");
    if (so == j.end() || !so->is_array() || so->empty() || tbl == j.end() || !tbl->is_object())
        return;
    auto idAt = [&](size_t i) -> int {
        return i < so->size() && (*so)[i].is_number() ? (*so)[i].get<int>() : 0;
    };

    const std::string& s = node.characters;
    std::vector<std::pair<size_t, int>> cps;  // (byte offset, style id)
    size_t byte = 0, u16 = 0;
    while (byte < s.size()) {
        const unsigned char c = static_cast<unsigned char>(s[byte]);
        const size_t len = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : 4;
        cps.emplace_back(byte, idAt(u16));
        byte += len;
        u16 += len == 4 ? 2 : 1;
    }

    bool anyOverride = false;
    std::vector<TextRun> runs;
    for (size_t i = 0; i < cps.size();) {
        const int id = cps[i].second;
        size_t e = i + 1;
        while (e < cps.size() && cps[e].second == id) ++e;
        TextRun run;
        run.start = static_cast<int>(cps[i].first);
        run.end = static_cast<int>(e < cps.size() ? cps[e].first : s.size());
        run.style = node.textStyle;
        const json* ov = nullptr;
        if (id != 0) {
            if (auto it = tbl->find(std::to_string(id)); it != tbl->end() && it->is_object())
                ov = &*it;
        }
        if (ov) {
            const json& o = *ov;
            run.style.fontFamily = jstr(o, "fontFamily", run.style.fontFamily);
            run.style.fontSize = jfloat(o, "fontSize", run.style.fontSize);
            if (o.contains("fontWeight") && o["fontWeight"].is_number())
                run.style.fontWeight = static_cast<int>(o["fontWeight"].get<float>());
            if (o.contains("italic") && o["italic"].is_boolean())
                run.style.italic = o["italic"].get<bool>();
            if (o.contains("letterSpacing") && o["letterSpacing"].is_number())
                run.style.letterSpacing = o["letterSpacing"].get<float>();
            if (auto f = o.find("fills"); f != o.end() && f->is_array()) {
                for (const auto& pj : *f) {
                    Paint p = parsePaint(pj);
                    if (p.visible && p.type == PaintType::Solid) {
                        Color c = p.color;
                        c.a *= p.opacity;
                        run.color = c;
                        break;
                    }
                }
            }
            anyOverride = true;
        }
        runs.push_back(std::move(run));
        i = e;
    }
    if (anyOverride && runs.size() > 1) node.textRuns = std::move(runs);
}

std::unique_ptr<Node> parseNode(const json& j, Node* parent) {
    auto node = std::make_unique<Node>();
    node->parent = parent;
    node->id = jstr(j, "id");
    node->name = jstr(j, "name");
    node->compType = jstr(j, "comp", "");  // hand-authored prefab tag (figo2godot --prefabs)
    node->type = parseNodeType(jstr(j, "type"));
    node->visible = jbool(j, "visible", true);
    node->opacity = jfloat(j, "opacity", 1.0f);

    // Size: prefer "size" (present with geometry=paths), fall back to
    // absoluteBoundingBox (correct only for unrotated nodes).
    if (auto it = j.find("size"); it != j.end() && it->is_object()) {
        node->width = jfloat(*it, "x");
        node->height = jfloat(*it, "y");
    } else if (auto bb = j.find("absoluteBoundingBox"); bb != j.end() && bb->is_object()) {
        node->width = jfloat(*bb, "width");
        node->height = jfloat(*bb, "height");
    }

    // relativeTransform: [[m00, m01, m02], [m10, m11, m12]]
    if (auto it = j.find("relativeTransform");
        it != j.end() && it->is_array() && it->size() >= 2) {
        const auto& r0 = (*it)[0];
        const auto& r1 = (*it)[1];
        if (r0.is_array() && r0.size() >= 3 && r1.is_array() && r1.size() >= 3) {
            Mat23& m = node->relativeTransform;
            m.m00 = r0[0].get<float>(); m.m01 = r0[1].get<float>(); m.m02 = r0[2].get<float>();
            m.m10 = r1[0].get<float>(); m.m11 = r1[1].get<float>(); m.m12 = r1[2].get<float>();
        }
    } else if (auto bb = j.find("absoluteBoundingBox"); bb != j.end() && bb->is_object()) {
        // Derive translation from absolute boxes (parent must be resolved the same way).
        node->relativeTransform.m02 = jfloat(*bb, "x");
        node->relativeTransform.m12 = jfloat(*bb, "y");
        if (parent) {
            node->relativeTransform.m02 -= parent->absoluteTransform.m02;
            node->relativeTransform.m12 -= parent->absoluteTransform.m12;
        }
        // Stash our absolute origin for children (runtime field reused during parse).
        node->absoluteTransform.m02 = jfloat(*bb, "x");
        node->absoluteTransform.m12 = jfloat(*bb, "y");
    }

    node->clipsContent = jbool(j, "clipsContent", false);
    parseLayout(j, *node);

    parsePaints(j, "fills", node->fills);
    parsePaints(j, "strokes", node->strokes);
    node->strokeWeight = jfloat(j, "strokeWeight", 1.0f);
    const std::string sa = jstr(j, "strokeAlign", "INSIDE");
    node->strokeAlign = sa == "OUTSIDE" ? StrokeAlign::Outside
                      : sa == "CENTER"  ? StrokeAlign::Center
                                        : StrokeAlign::Inside;
    if (auto it = j.find("strokeDashes"); it != j.end() && it->is_array()) {
        for (const auto& d : *it)
            if (d.is_number()) node->strokeDashes.push_back(d.get<float>());
    }
    node->strokeCap = jstr(j, "strokeCap", "NONE");
    node->strokeJoin = jstr(j, "strokeJoin", "MITER");

    node->cornerRadius = jfloat(j, "cornerRadius", 0);
    if (auto it = j.find("rectangleCornerRadii");
        it != j.end() && it->is_array() && it->size() == 4) {
        node->rectangleCornerRadii = {{(*it)[0].get<float>(), (*it)[1].get<float>(),
                                       (*it)[2].get<float>(), (*it)[3].get<float>()}};
    }

    parseGeometry(j, "fillGeometry", node->fillGeometry);
    parseGeometry(j, "strokeGeometry", node->strokeGeometry);
    parseEffects(j, node->effects);

    if (node->type == NodeType::Text) {
        node->characters = jstr(j, "characters");
        parseTextStyle(j, node->textStyle);
        parseTextRuns(j, *node);
    }

    if (node->type == NodeType::Instance) node->componentId = jstr(j, "componentId");

    // CANVAS pages have a background color exposed via "backgroundColor".
    if (node->type == NodeType::Canvas && node->fills.empty()) {
        if (auto it = j.find("backgroundColor"); it != j.end() && it->is_object()) {
            Paint bg;
            bg.type = PaintType::Solid;
            bg.color = parseColor(*it);
            node->fills.push_back(bg);
        }
    }

    // Boolean ops with combined geometry: the outline is the final visual;
    // children would repaint the source shapes on top of it.
    const bool booleanWithOutline =
        node->type == NodeType::BooleanOperation && !node->fillGeometry.empty();
    if (!booleanWithOutline) {
        if (auto it = j.find("children"); it != j.end() && it->is_array()) {
            for (const auto& c : *it) node->children.push_back(parseNode(c, node.get()));
        }
    }
    return node;
}

}  // namespace

std::unique_ptr<Document> parseDocument(const std::string& jsonText) {
    json j = json::parse(jsonText);  // throws nlohmann::json::parse_error (runtime_error)

    auto doc = std::make_unique<Document>();
    doc->name = jstr(j, "name", "Untitled");

    const json* rootJson = nullptr;
    if (auto it = j.find("document"); it != j.end() && it->is_object()) {
        rootJson = &*it;
    } else if (j.contains("id") && j.contains("type")) {
        rootJson = &j;  // bare node tree
    } else {
        throw std::runtime_error("figo: JSON has no \"document\" node");
    }
    doc->root = parseNode(*rootJson, nullptr);
    doc->captureBaseLayout();
    return doc;
}

std::unique_ptr<Document> loadDocumentFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("figo: cannot open file: " + path);
    std::ostringstream ss;
    ss << f.rdbuf();
    return parseDocument(ss.str());
}

}  // namespace figo
