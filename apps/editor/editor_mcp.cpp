// figmaedit MCP server — JSON-RPC dispatch + design tools.
//
// Threading model: the net thread (editor_mcp_net) answers protocol-only
// requests (initialize / ping / tools/list) inline; every tools/call is
// queued and executed by mcpPump() on the main thread between frames, so
// tool handlers may touch EditorState freely. All mutations go through the
// editor's undo machinery (NodeProps snapshots / TreeChange entries) — the
// user can Ctrl+Z anything the AI did.

#include "editor_mcp.h"

#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include "editor_mcp_net.h"

namespace figmaedit {

namespace {

using json = nlohmann::json;
using figmalib::AutoLayout;
using figmalib::Constraint;
using figmalib::Effect;
using figmalib::NodeType;
using figmalib::Paint;
using figmalib::PaintType;
using figmalib::StrokeAlign;
using figmalib::TextStyle;

constexpr const char* kServerVersion = "0.1.0";

// Tool-level failure: reported inside the tool result (isError), not as a
// JSON-RPC protocol error.
struct ToolError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// ---- small helpers -------------------------------------------------------

std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

std::string base64(const unsigned char* data, size_t len) {
    static const char* tbl =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        const unsigned b0 = data[i];
        const unsigned b1 = i + 1 < len ? data[i + 1] : 0;
        const unsigned b2 = i + 2 < len ? data[i + 2] : 0;
        out.push_back(tbl[b0 >> 2]);
        out.push_back(tbl[((b0 & 3) << 4) | (b1 >> 4)]);
        out.push_back(i + 1 < len ? tbl[((b1 & 15) << 2) | (b2 >> 6)] : '=');
        out.push_back(i + 2 < len ? tbl[b2 & 63] : '=');
    }
    return out;
}

std::string colorToHex(const figmalib::Color& c) {
    auto b = [](float v) {
        return static_cast<int>(std::lround(std::max(0.0f, std::min(1.0f, v)) * 255));
    };
    char buf[12];
    if (c.a < 0.999f)
        std::snprintf(buf, sizeof(buf), "#%02X%02X%02X%02X", b(c.r), b(c.g), b(c.b), b(c.a));
    else
        std::snprintf(buf, sizeof(buf), "#%02X%02X%02X", b(c.r), b(c.g), b(c.b));
    return buf;
}

figmalib::Color parseColor(const std::string& in) {
    const char* s = in.c_str();
    if (*s == '#') ++s;
    const size_t n = std::strlen(s);
    if (n != 6 && n != 8) throw ToolError("bad color '" + in + "' (want #RRGGBB or #RRGGBBAA)");
    auto nib = [&](char ch) -> int {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
        if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
        throw ToolError("bad color '" + in + "'");
    };
    auto byte = [&](size_t i) { return (nib(s[i]) * 16 + nib(s[i + 1])) / 255.0f; };
    figmalib::Color c;
    c.r = byte(0);
    c.g = byte(2);
    c.b = byte(4);
    c.a = n == 8 ? byte(6) : 1.0f;
    return c;
}

// ---- enum <-> string -----------------------------------------------------

const char* typeName(NodeType t) {
    switch (t) {
    case NodeType::Document: return "DOCUMENT";
    case NodeType::Canvas: return "CANVAS";
    case NodeType::Frame: return "FRAME";
    case NodeType::Group: return "GROUP";
    case NodeType::Section: return "SECTION";
    case NodeType::Rectangle: return "RECTANGLE";
    case NodeType::Ellipse: return "ELLIPSE";
    case NodeType::Line: return "LINE";
    case NodeType::Vector: return "VECTOR";
    case NodeType::BooleanOperation: return "BOOLEAN_OPERATION";
    case NodeType::Star: return "STAR";
    case NodeType::RegularPolygon: return "REGULAR_POLYGON";
    case NodeType::Text: return "TEXT";
    case NodeType::Component: return "COMPONENT";
    case NodeType::ComponentSet: return "COMPONENT_SET";
    case NodeType::Instance: return "INSTANCE";
    case NodeType::Slice: return "SLICE";
    default: return "UNKNOWN";
    }
}

NodeType creatableTypeFromString(const std::string& s) {
    const std::string t = upper(s);
    if (t == "FRAME") return NodeType::Frame;
    if (t == "GROUP") return NodeType::Group;
    if (t == "RECTANGLE" || t == "RECT") return NodeType::Rectangle;
    if (t == "ELLIPSE") return NodeType::Ellipse;
    if (t == "LINE") return NodeType::Line;
    if (t == "VECTOR") return NodeType::Vector;
    if (t == "TEXT") return NodeType::Text;
    if (t == "STAR") return NodeType::Star;
    if (t == "POLYGON" || t == "REGULAR_POLYGON") return NodeType::RegularPolygon;
    if (t == "SECTION") return NodeType::Section;
    throw ToolError("unsupported node type '" + s + "'");
}

const char* strokeAlignName(StrokeAlign a) {
    switch (a) {
    case StrokeAlign::Outside: return "OUTSIDE";
    case StrokeAlign::Center: return "CENTER";
    default: return "INSIDE";
    }
}

StrokeAlign strokeAlignFromString(const std::string& s) {
    const std::string t = upper(s);
    if (t == "OUTSIDE") return StrokeAlign::Outside;
    if (t == "CENTER") return StrokeAlign::Center;
    if (t == "INSIDE") return StrokeAlign::Inside;
    throw ToolError("bad strokeAlign '" + s + "'");
}

const char* constraintName(Constraint c) {
    switch (c) {
    case Constraint::Center: return "CENTER";
    case Constraint::Max: return "MAX";
    case Constraint::Stretch: return "STRETCH";
    case Constraint::Scale: return "SCALE";
    default: return "MIN";
    }
}

Constraint constraintFromString(const std::string& s) {
    const std::string t = upper(s);
    if (t == "MIN" || t == "LEFT" || t == "TOP") return Constraint::Min;
    if (t == "CENTER") return Constraint::Center;
    if (t == "MAX" || t == "RIGHT" || t == "BOTTOM") return Constraint::Max;
    if (t == "STRETCH" || t == "LEFT_RIGHT" || t == "TOP_BOTTOM") return Constraint::Stretch;
    if (t == "SCALE") return Constraint::Scale;
    throw ToolError("bad constraint '" + s + "'");
}

const char* alAlignName(AutoLayout::Align a) {
    switch (a) {
    case AutoLayout::Align::Center: return "CENTER";
    case AutoLayout::Align::Max: return "MAX";
    case AutoLayout::Align::SpaceBetween: return "SPACE_BETWEEN";
    case AutoLayout::Align::Baseline: return "BASELINE";
    default: return "MIN";
    }
}

AutoLayout::Align alAlignFromString(const std::string& s) {
    const std::string t = upper(s);
    if (t == "MIN") return AutoLayout::Align::Min;
    if (t == "CENTER") return AutoLayout::Align::Center;
    if (t == "MAX") return AutoLayout::Align::Max;
    if (t == "SPACE_BETWEEN") return AutoLayout::Align::SpaceBetween;
    if (t == "BASELINE") return AutoLayout::Align::Baseline;
    throw ToolError("bad auto-layout align '" + s + "'");
}

// ---- Paint / Effect / TextStyle <-> JSON ----------------------------------

json paintToJson(const Paint& p) {
    json j;
    j["visible"] = p.visible;
    j["opacity"] = p.opacity;
    switch (p.type) {
    case PaintType::Solid:
        j["type"] = "SOLID";
        j["color"] = colorToHex(p.color);
        break;
    case PaintType::Image:
        j["type"] = "IMAGE";
        j["imageRef"] = p.imageRef;
        j["scaleMode"] = p.imageScaleMode;
        break;
    default: {
        j["type"] = p.type == PaintType::GradientLinear    ? "GRADIENT_LINEAR"
                    : p.type == PaintType::GradientRadial  ? "GRADIENT_RADIAL"
                    : p.type == PaintType::GradientAngular ? "GRADIENT_ANGULAR"
                                                           : "GRADIENT_DIAMOND";
        json stops = json::array();
        for (const auto& s : p.stops)
            stops.push_back({{"position", s.position}, {"color", colorToHex(s.color)}});
        j["stops"] = std::move(stops);
        j["handles"] = {{p.handle0[0], p.handle0[1]},
                        {p.handle1[0], p.handle1[1]},
                        {p.handle2[0], p.handle2[1]}};
        break;
    }
    }
    return j;
}

Paint paintFromJson(const json& v) {
    Paint p;
    if (v.is_string()) {  // shorthand: just a color string
        p.type = PaintType::Solid;
        p.color = parseColor(v.get<std::string>());
        return p;
    }
    if (!v.is_object()) throw ToolError("paint must be a color string or object");
    const std::string t = upper(v.value("type", v.contains("stops") ? "GRADIENT_LINEAR"
                                                                    : "SOLID"));
    p.visible = v.value("visible", true);
    p.opacity = v.value("opacity", 1.0f);
    if (t == "SOLID") {
        p.type = PaintType::Solid;
        if (!v.contains("color")) throw ToolError("solid paint needs \"color\"");
        p.color = parseColor(v["color"].get<std::string>());
        return p;
    }
    if (t == "GRADIENT_LINEAR") p.type = PaintType::GradientLinear;
    else if (t == "GRADIENT_RADIAL") p.type = PaintType::GradientRadial;
    else if (t == "GRADIENT_ANGULAR") p.type = PaintType::GradientAngular;
    else if (t == "GRADIENT_DIAMOND") p.type = PaintType::GradientDiamond;
    else throw ToolError("unsupported paint type '" + t + "'");
    if (!v.contains("stops") || !v["stops"].is_array() || v["stops"].size() < 2)
        throw ToolError("gradient paint needs >=2 \"stops\"");
    for (const auto& s : v["stops"]) {
        figmalib::GradientStop stop;
        stop.position = s.value("position", 0.0f);
        stop.color = parseColor(s.at("color").get<std::string>());
        p.stops.push_back(stop);
    }
    if (v.contains("handles") && v["handles"].is_array() && v["handles"].size() >= 2) {
        auto h = [&](size_t i, std::array<float, 2>& out) {
            if (i < v["handles"].size() && v["handles"][i].is_array() &&
                v["handles"][i].size() >= 2) {
                out[0] = v["handles"][i][0].get<float>();
                out[1] = v["handles"][i][1].get<float>();
            }
        };
        h(0, p.handle0);
        h(1, p.handle1);
        h(2, p.handle2);
    } else if (p.type == PaintType::GradientLinear) {
        // default: top -> bottom
        p.handle0 = {0.5f, 0.0f};
        p.handle1 = {0.5f, 1.0f};
        p.handle2 = {1.0f, 0.0f};
    } else {
        // default: centered radial
        p.handle0 = {0.5f, 0.5f};
        p.handle1 = {1.0f, 0.5f};
        p.handle2 = {0.5f, 1.0f};
    }
    return p;
}

std::vector<Paint> paintListFromJson(const json& v) {
    std::vector<Paint> out;
    if (v.is_array()) {
        for (const auto& e : v) out.push_back(paintFromJson(e));
    } else if (!v.is_null()) {
        out.push_back(paintFromJson(v));
    }
    return out;
}

json effectToJson(const Effect& e) {
    json j;
    switch (e.type) {
    case Effect::Type::DropShadow: j["type"] = "DROP_SHADOW"; break;
    case Effect::Type::InnerShadow: j["type"] = "INNER_SHADOW"; break;
    case Effect::Type::LayerBlur: j["type"] = "LAYER_BLUR"; break;
    case Effect::Type::BackgroundBlur: j["type"] = "BACKGROUND_BLUR"; break;
    }
    j["visible"] = e.visible;
    j["color"] = colorToHex(e.color);
    j["offsetX"] = e.offsetX;
    j["offsetY"] = e.offsetY;
    j["radius"] = e.radius;
    j["spread"] = e.spread;
    return j;
}

Effect effectFromJson(const json& v) {
    Effect e;
    const std::string t = upper(v.value("type", "DROP_SHADOW"));
    if (t == "DROP_SHADOW") e.type = Effect::Type::DropShadow;
    else if (t == "INNER_SHADOW") e.type = Effect::Type::InnerShadow;
    else if (t == "LAYER_BLUR") e.type = Effect::Type::LayerBlur;
    else if (t == "BACKGROUND_BLUR") e.type = Effect::Type::BackgroundBlur;
    else throw ToolError("bad effect type '" + t + "'");
    e.visible = v.value("visible", true);
    if (v.contains("color")) e.color = parseColor(v["color"].get<std::string>());
    e.offsetX = v.value("offsetX", v.value("x", 0.0f));
    e.offsetY = v.value("offsetY", v.value("y", 0.0f));
    e.radius = v.value("radius", 4.0f);
    e.spread = v.value("spread", 0.0f);
    return e;
}

json textStyleToJson(const TextStyle& s) {
    json j;
    j["fontFamily"] = s.fontFamily;
    j["fontSize"] = s.fontSize;
    j["fontWeight"] = s.fontWeight;
    j["italic"] = s.italic;
    j["lineHeightPx"] = s.lineHeightPx;
    j["letterSpacing"] = s.letterSpacing;
    j["alignH"] = s.alignH == TextStyle::AlignH::Center      ? "CENTER"
                  : s.alignH == TextStyle::AlignH::Right     ? "RIGHT"
                  : s.alignH == TextStyle::AlignH::Justified ? "JUSTIFIED"
                                                             : "LEFT";
    j["alignV"] = s.alignV == TextStyle::AlignV::Center   ? "CENTER"
                  : s.alignV == TextStyle::AlignV::Bottom ? "BOTTOM"
                                                          : "TOP";
    j["autoResize"] = s.autoResize;
    return j;
}

// ---- node JSON -------------------------------------------------------------

bool underPage(const EditorState& ed, const Node* n) {
    for (const Node* p = n; p; p = p->parent)
        if (p == ed.page) return true;
    return false;
}

json nodeBrief(const EditorState& ed, Node& n) {
    json j;
    j["id"] = n.id;
    j["name"] = n.name;
    j["type"] = typeName(n.type);
    j["visible"] = n.visible;
    j["x"] = n.relativeTransform.m02;
    j["y"] = n.relativeTransform.m12;
    j["width"] = n.width;
    j["height"] = n.height;
    if (!n.children.empty()) j["childCount"] = n.children.size();
    // absoluteTransform is only maintained for the current page.
    if (n.parent && underPage(ed, &n)) {
        const WorldRect b = worldBounds(n);
        j["absBounds"] = {b.x0, b.y0, b.x1, b.y1};
    }
    return j;
}

json nodeDetail(const EditorState& ed, Node& n) {
    json j = nodeBrief(ed, n);
    j["opacity"] = n.opacity;
    if (n.parent) j["parentId"] = n.parent->id;
    if (n.cornerRadius > 0) j["cornerRadius"] = n.cornerRadius;
    if (n.rectangleCornerRadii) {
        const auto& r = *n.rectangleCornerRadii;
        j["cornerRadii"] = {r[0], r[1], r[2], r[3]};
    }
    j["fills"] = json::array();
    for (const auto& p : n.fills) j["fills"].push_back(paintToJson(p));
    j["strokes"] = json::array();
    for (const auto& p : n.strokes) j["strokes"].push_back(paintToJson(p));
    if (!n.strokes.empty()) {
        j["strokeWeight"] = n.strokeWeight;
        j["strokeAlign"] = strokeAlignName(n.strokeAlign);
        if (!n.strokeDashes.empty()) j["strokeDashes"] = n.strokeDashes;
    }
    if (!n.effects.empty()) {
        j["effects"] = json::array();
        for (const auto& e : n.effects) j["effects"].push_back(effectToJson(e));
    }
    if (n.clipsContent) j["clipsContent"] = true;
    if (n.type == NodeType::Text) {
        j["text"] = n.characters;
        j["textStyle"] = textStyleToJson(n.textStyle);
    }
    if (!n.componentId.empty()) j["componentId"] = n.componentId;
    j["constraints"] = {{"h", constraintName(n.constraintH)},
                        {"v", constraintName(n.constraintV)}};
    if (n.autoLayout.enabled()) {
        const auto& al = n.autoLayout;
        j["autoLayout"] = {
            {"mode", al.mode == AutoLayout::Mode::Horizontal ? "HORIZONTAL" : "VERTICAL"},
            {"itemSpacing", al.itemSpacing},
            {"paddingLeft", al.paddingLeft},
            {"paddingRight", al.paddingRight},
            {"paddingTop", al.paddingTop},
            {"paddingBottom", al.paddingBottom},
            {"primaryAlign", alAlignName(al.primaryAlign)},
            {"counterAlign", alAlignName(al.counterAlign)},
            {"primarySizing", al.primarySizing == AutoLayout::Sizing::Hug ? "HUG" : "FIXED"},
            {"counterSizing", al.counterSizing == AutoLayout::Sizing::Hug ? "HUG" : "FIXED"},
            {"wrap", al.wrap},
        };
    }
    if (!n.fillGeometry.empty()) j["hasVectorGeometry"] = true;
    if (!n.children.empty()) {
        j["children"] = json::array();
        for (const auto& c : n.children)
            j["children"].push_back({{"id", c->id}, {"name", c->name},
                                     {"type", typeName(c->type)}});
    }
    return j;
}

json nodeTree(const EditorState& ed, Node& n, int depth) {
    json j = nodeBrief(ed, n);
    if (depth > 0 && !n.children.empty()) {
        j["children"] = json::array();
        for (const auto& c : n.children) j["children"].push_back(nodeTree(ed, *c, depth - 1));
    }
    return j;
}

// ---- lookups ---------------------------------------------------------------

void requireDoc(const EditorState& ed) {
    if (!ed.file.document || !ed.page)
        throw ToolError("no document is open — call open_document first");
}

Node* findNode(EditorState& ed, const std::string& key) {
    if (!ed.file.document || key.empty()) return nullptr;
    if (Node* n = ed.file.document->findById(key)) return n;
    return ed.file.document->findByName(key);
}

// .fig-derived documents can contain synthesized wrapper nodes without a
// guid. Ids are the AI's only stable handle, so backfill the empty ones once
// per document before running tools.
void ensureNodeIds(EditorState& ed) {
    if (!ed.file.document || !ed.file.document->root) return;
    Node* root = ed.file.document->root.get();
    std::unordered_set<std::string> used;
    root->visit([&](Node& n) {
        if (!n.id.empty()) used.insert(n.id);
        return true;
    });
    int counter = 0;
    root->visit([&](Node& n) {
        if (n.id.empty()) {
            std::string id;
            do {
                id = "n:" + std::to_string(++counter);
            } while (used.count(id));
            n.id = id;
            used.insert(std::move(id));
        }
        return true;
    });
}

Node* findNodeArg(EditorState& ed, const json& args, const char* field) {
    const std::string key = args.at(field).get<std::string>();
    Node* n = findNode(ed, key);
    if (!n) throw ToolError("node not found: '" + key + "' (id or name)");
    return n;
}

std::string newId(EditorState& ed) {
    static int counter = 0;
    std::string id;
    do {
        id = "mcp:" + std::to_string(++counter);
    } while (ed.file.document->findById(id));
    return id;
}

// Same rule as the inspector / canvas resize: nodes with baked vector
// geometry scale their transform, primitives just change width/height.
void setNodeSize(Node& n, float w, float h) {
    w = std::max(1.0f, w);
    h = std::max(1.0f, h);
    const bool hasGeometry = !n.fillGeometry.empty() || !n.strokeGeometry.empty();
    if (hasGeometry && n.width > 0 && n.height > 0) {
        n.relativeTransform.m00 *= w / n.width;
        n.relativeTransform.m11 *= h / n.height;
    } else {
        n.width = w;
        n.height = h;
    }
}

// ---- property patch (shared by create_node and update_nodes) ---------------

void applyFields(Node* n, const json& u) {
    if (u.contains("name")) n->name = u["name"].get<std::string>();
    if (u.contains("x")) n->relativeTransform.m02 = u["x"].get<float>();
    if (u.contains("y")) n->relativeTransform.m12 = u["y"].get<float>();
    if (u.contains("width") || u.contains("height")) {
        setNodeSize(*n, u.value("width", n->width), u.value("height", n->height));
    }
    if (u.contains("visible")) n->visible = u["visible"].get<bool>();
    if (u.contains("opacity"))
        n->opacity = std::max(0.0f, std::min(1.0f, u["opacity"].get<float>()));
    if (u.contains("cornerRadius")) n->cornerRadius = std::max(0.0f, u["cornerRadius"].get<float>());
    if (u.contains("cornerRadii")) {
        const auto& r = u["cornerRadii"];
        if (!r.is_array() || r.size() != 4) throw ToolError("cornerRadii wants [TL,TR,BR,BL]");
        n->rectangleCornerRadii = {{r[0].get<float>(), r[1].get<float>(), r[2].get<float>(),
                                    r[3].get<float>()}};
    }
    if (u.contains("fill")) n->fills = paintListFromJson(u["fill"]);
    if (u.contains("fills")) n->fills = paintListFromJson(u["fills"]);
    if (u.contains("stroke")) n->strokes = paintListFromJson(u["stroke"]);
    if (u.contains("strokes")) n->strokes = paintListFromJson(u["strokes"]);
    if (u.contains("strokeWeight")) n->strokeWeight = std::max(0.0f, u["strokeWeight"].get<float>());
    if (u.contains("strokeAlign"))
        n->strokeAlign = strokeAlignFromString(u["strokeAlign"].get<std::string>());
    if (u.contains("strokeDashes")) {
        n->strokeDashes.clear();
        for (const auto& d : u["strokeDashes"]) n->strokeDashes.push_back(d.get<float>());
    }
    if (u.contains("effects")) {
        n->effects.clear();
        for (const auto& e : u["effects"]) n->effects.push_back(effectFromJson(e));
    }
    if (u.contains("clipsContent")) n->clipsContent = u["clipsContent"].get<bool>();
    if (u.contains("text")) {
        n->characters = u["text"].get<std::string>();
        n->textRuns.clear();  // ranges of the old string are meaningless now
    }
    if (u.contains("fontFamily")) n->textStyle.fontFamily = u["fontFamily"].get<std::string>();
    if (u.contains("fontSize")) n->textStyle.fontSize = u["fontSize"].get<float>();
    if (u.contains("fontWeight")) n->textStyle.fontWeight = u["fontWeight"].get<int>();
    if (u.contains("italic")) n->textStyle.italic = u["italic"].get<bool>();
    if (u.contains("lineHeight")) n->textStyle.lineHeightPx = u["lineHeight"].get<float>();
    if (u.contains("letterSpacing")) n->textStyle.letterSpacing = u["letterSpacing"].get<float>();
    if (u.contains("textAlignH")) {
        const std::string a = upper(u["textAlignH"].get<std::string>());
        n->textStyle.alignH = a == "CENTER"      ? TextStyle::AlignH::Center
                              : a == "RIGHT"     ? TextStyle::AlignH::Right
                              : a == "JUSTIFIED" ? TextStyle::AlignH::Justified
                                                 : TextStyle::AlignH::Left;
    }
    if (u.contains("textAlignV")) {
        const std::string a = upper(u["textAlignV"].get<std::string>());
        n->textStyle.alignV = a == "CENTER"   ? TextStyle::AlignV::Center
                              : a == "BOTTOM" ? TextStyle::AlignV::Bottom
                                              : TextStyle::AlignV::Top;
    }
    if (u.contains("textAutoResize")) n->textStyle.autoResize = upper(u["textAutoResize"].get<std::string>());
    if (u.contains("constraints")) {
        const auto& c = u["constraints"];
        if (c.contains("h")) n->constraintH = constraintFromString(c["h"].get<std::string>());
        if (c.contains("v")) n->constraintV = constraintFromString(c["v"].get<std::string>());
    }
    if (u.contains("autoLayout")) {
        const auto& a = u["autoLayout"];
        AutoLayout& al = n->autoLayout;
        if (a.contains("mode")) {
            const std::string m = upper(a["mode"].get<std::string>());
            al.mode = m == "HORIZONTAL" ? AutoLayout::Mode::Horizontal
                      : m == "VERTICAL" ? AutoLayout::Mode::Vertical
                                        : AutoLayout::Mode::None;
        }
        if (a.contains("itemSpacing")) al.itemSpacing = a["itemSpacing"].get<float>();
        if (a.contains("counterSpacing")) al.counterSpacing = a["counterSpacing"].get<float>();
        if (a.contains("paddingLeft")) al.paddingLeft = a["paddingLeft"].get<float>();
        if (a.contains("paddingRight")) al.paddingRight = a["paddingRight"].get<float>();
        if (a.contains("paddingTop")) al.paddingTop = a["paddingTop"].get<float>();
        if (a.contains("paddingBottom")) al.paddingBottom = a["paddingBottom"].get<float>();
        if (a.contains("padding")) {
            const float p = a["padding"].get<float>();
            al.paddingLeft = al.paddingRight = al.paddingTop = al.paddingBottom = p;
        }
        if (a.contains("primaryAlign"))
            al.primaryAlign = alAlignFromString(a["primaryAlign"].get<std::string>());
        if (a.contains("counterAlign"))
            al.counterAlign = alAlignFromString(a["counterAlign"].get<std::string>());
        if (a.contains("primarySizing"))
            al.primarySizing = upper(a["primarySizing"].get<std::string>()) == "HUG"
                                   ? AutoLayout::Sizing::Hug
                                   : AutoLayout::Sizing::Fixed;
        if (a.contains("counterSizing"))
            al.counterSizing = upper(a["counterSizing"].get<std::string>()) == "HUG"
                                   ? AutoLayout::Sizing::Hug
                                   : AutoLayout::Sizing::Fixed;
        if (a.contains("wrap")) al.wrap = a["wrap"].get<bool>();
    }
    if (u.contains("layoutGrow")) n->layoutGrow = u["layoutGrow"].get<float>();
    if (u.contains("layoutAlignStretch")) n->layoutAlignStretch = u["layoutAlignStretch"].get<bool>();
    if (u.contains("layoutAbsolute")) n->layoutAbsolute = u["layoutAbsolute"].get<bool>();
    if (u.contains("svgPath")) {
        figmalib::PathGeometry g;
        g.path = u["svgPath"].get<std::string>();
        g.evenOdd = u.value("evenOdd", false);
        n->fillGeometry.clear();
        n->fillGeometry.push_back(std::move(g));
        n->strokeGeometry.clear();
    }
}

// ---- tool result wrappers ---------------------------------------------------

json toolText(const std::string& s) {
    return json{{"content", json::array({json{{"type", "text"}, {"text", s}}})}};
}

json toolErrorResult(const std::string& s) {
    json j = toolText(s);
    j["isError"] = true;
    return j;
}

// ---- tools ------------------------------------------------------------------

json editorStateJson(EditorState& ed) {
    json j;
    if (!ed.file.document) {
        j["document"] = nullptr;
        j["hint"] = "no document open; use open_document";
        return j;
    }
    j["filePath"] = ed.filePath;
    j["savePath"] = ed.savePath;
    j["unsaved"] = ed.unsaved;
    j["documentName"] = ed.file.document->name;
    json pages = json::array();
    int idx = 0;
    for (const auto& c : ed.file.document->root->children) {
        if (c->type != NodeType::Canvas) continue;
        pages.push_back({{"index", idx},
                         {"id", c->id},
                         {"name", c->name},
                         {"current", c.get() == ed.page},
                         {"topLevelChildren", c->children.size()}});
        ++idx;
    }
    j["pages"] = std::move(pages);
    if (ed.page) j["currentPageId"] = ed.page->id;
    json sel = json::array();
    for (Node* n : ed.selection) sel.push_back({{"id", n->id}, {"name", n->name}});
    j["selection"] = std::move(sel);
    j["undoDepth"] = ed.undoStack.size();
    j["redoDepth"] = ed.redoStack.size();
    j["viewport"] = {{"width", ed.viewportW()},
                     {"height", ed.viewportH()},
                     {"zoom", ed.cam.zoom}};
    j["hint"] = "use get_node_tree for layers, get_screenshot to see the canvas";
    return j;
}

json toolGetNodeTree(EditorState& ed, const json& a) {
    requireDoc(ed);
    Node* root = a.contains("nodeId") ? findNodeArg(ed, a, "nodeId") : ed.page;
    const int depth = std::max(0, std::min(100, a.value("depth", 100)));
    return toolText(nodeTree(ed, *root, depth).dump(2));
}

json toolGetNode(EditorState& ed, const json& a) {
    requireDoc(ed);
    json out = json::array();
    for (const auto& idv : a.at("ids")) {
        Node* n = findNode(ed, idv.get<std::string>());
        if (!n) out.push_back({{"error", "not found: " + idv.get<std::string>()}});
        else out.push_back(nodeDetail(ed, *n));
    }
    return toolText(out.dump(2));
}

json toolCreateNode(EditorState& ed, const json& a) {
    requireDoc(ed);
    const NodeType type = creatableTypeFromString(a.value("type", "rectangle"));
    Node* parent = a.contains("parentId") ? findNodeArg(ed, a, "parentId") : ed.page;
    if (parent->type == NodeType::Text) throw ToolError("cannot nest nodes inside a TEXT node");

    auto holder = std::make_unique<Node>();
    Node* n = holder.get();
    n->type = type;
    n->id = newId(ed);
    // Figma-flavored defaults; everything below can be overridden by args.
    switch (type) {
    case NodeType::Frame:
        n->name = "Frame";
        n->width = n->height = 100;
        n->fills.push_back({});
        n->fills.back().color = {1, 1, 1, 1};
        n->clipsContent = true;
        break;
    case NodeType::Group: n->name = "Group"; n->width = n->height = 100; break;
    case NodeType::Section: n->name = "Section"; n->width = n->height = 400; break;
    case NodeType::Text: {
        n->name = a.value("text", std::string("Text"));
        n->width = 200;
        n->height = 24;
        n->fills.push_back({});
        n->fills.back().color = {0, 0, 0, 1};
        break;
    }
    case NodeType::Line: {
        n->name = "Line";
        n->width = 100;
        n->height = 0;
        n->strokes.push_back({});
        n->strokes.back().color = {0, 0, 0, 1};
        n->strokeWeight = 1;
        break;
    }
    default: {  // rectangle / ellipse / vector / star / polygon
        if (type == NodeType::Rectangle) n->name = "Rectangle";
        else if (type == NodeType::Ellipse) n->name = "Ellipse";
        else if (type == NodeType::Vector) n->name = "Vector";
        else if (type == NodeType::Star) n->name = "Star";
        else n->name = "Polygon";
        n->width = n->height = 100;
        n->fills.push_back({});
        n->fills.back().color = {0.851f, 0.851f, 0.851f, 1};  // #D9D9D9
        break;
    }
    }
    applyFields(n, a);  // x/y/width/height/fill/text/etc. from the args
    n->baseTransform = n->relativeTransform;
    n->baseWidth = n->width;
    n->baseHeight = n->height;
    n->parent = parent;

    size_t at = parent->children.size();
    if (a.contains("index"))
        at = std::min(static_cast<size_t>(std::max(0, a["index"].get<int>())),
                      parent->children.size());
    parent->children.insert(parent->children.begin() + static_cast<long long>(at),
                            std::move(holder));

    UndoEntry e;
    TreeChange ch;
    ch.isInsert = true;
    ch.parent = parent;
    ch.index = at;
    ch.node = n;
    e.tree.push_back(std::move(ch));
    ed.undoStack.push_back(std::move(e));
    ed.redoStack.clear();
    ed.unsaved = true;
    ed.markDocChanged();
    ed.setStatus("MCP: created " + n->name);
    return toolText(nodeDetail(ed, *n).dump(2));
}

json toolUpdateNodes(EditorState& ed, const json& a) {
    requireDoc(ed);
    const auto& updates = a.at("updates");
    if (!updates.is_array() || updates.empty()) throw ToolError("\"updates\" must be a non-empty array");
    std::vector<NodeProps> before;
    std::vector<Node*> touched;
    json out = json::array();
    for (const auto& u : updates) {
        Node* n = findNodeArg(ed, u, "id");
        before.push_back(NodeProps::capture(n));
        try {
            applyFields(n, u);
        } catch (...) {
            before.back().apply();  // roll back this node, keep earlier ones
            throw;
        }
        touched.push_back(n);
    }
    ed.pushPropsUndo(std::move(before));
    for (Node* n : touched) ed.bumpNode(n);
    ed.markDocChanged();
    for (Node* n : touched) out.push_back(nodeBrief(ed, *n));
    ed.setStatus("MCP: updated " + std::to_string(touched.size()) + " node(s)");
    return toolText(out.dump(2));
}

json toolDeleteNodes(EditorState& ed, const json& a) {
    requireDoc(ed);
    UndoEntry e;
    std::vector<std::string> deleted;
    for (const auto& idv : a.at("ids")) {
        Node* n = findNode(ed, idv.get<std::string>());
        if (!n) throw ToolError("node not found: " + idv.get<std::string>());
        if (!n->parent || n->type == NodeType::Canvas || n->type == NodeType::Document)
            throw ToolError("cannot delete page/root node: " + n->id);
        auto& siblings = n->parent->children;
        for (size_t i = 0; i < siblings.size(); ++i) {
            if (siblings[i].get() != n) continue;
            // Drop dangling editor references into the doomed subtree.
            ed.selection.erase(std::remove_if(ed.selection.begin(), ed.selection.end(),
                                              [&](Node* s) {
                                                  for (Node* p = s; p; p = p->parent)
                                                      if (p == n) return true;
                                                  return false;
                                              }),
                               ed.selection.end());
            for (Node* p = ed.scope; p; p = p->parent)
                if (p == n) { ed.scope = ed.page; break; }
            ed.hovered = nullptr;
            TreeChange ch;
            ch.isInsert = false;
            ch.parent = n->parent;
            ch.index = i;
            ch.node = n;
            ch.detached = std::move(siblings[i]);
            siblings.erase(siblings.begin() + static_cast<long long>(i));
            e.tree.push_back(std::move(ch));
            deleted.push_back(idv.get<std::string>());
            break;
        }
    }
    if (e.tree.empty()) throw ToolError("nothing deleted");
    ed.undoStack.push_back(std::move(e));
    ed.redoStack.clear();
    ed.unsaved = true;
    ed.markDocChanged();
    ed.setStatus("MCP: deleted " + std::to_string(deleted.size()) + " node(s)");
    return toolText(json{{"deleted", deleted}}.dump());
}

void assignFreshIds(EditorState& ed, Node& n) {
    n.id = newId(ed);
    for (auto& c : n.children) assignFreshIds(ed, *c);
}

json toolDuplicateNode(EditorState& ed, const json& a) {
    requireDoc(ed);
    Node* src = findNodeArg(ed, a, "id");
    if (!src->parent) throw ToolError("cannot duplicate the page/root");
    auto copy = figmalib::cloneNode(*src, src->parent);
    assignFreshIds(ed, *copy);
    copy->relativeTransform.m02 += a.value("dx", 10.0f);
    copy->relativeTransform.m12 += a.value("dy", 10.0f);
    Node* raw = copy.get();
    src->parent->children.push_back(std::move(copy));
    UndoEntry e;
    TreeChange ch;
    ch.isInsert = true;
    ch.parent = src->parent;
    ch.index = src->parent->children.size() - 1;
    ch.node = raw;
    e.tree.push_back(std::move(ch));
    ed.undoStack.push_back(std::move(e));
    ed.redoStack.clear();
    ed.unsaved = true;
    ed.markDocChanged();
    ed.setStatus("MCP: duplicated " + src->name);
    return toolText(nodeDetail(ed, *raw).dump(2));
}

json toolMoveNode(EditorState& ed, const json& a) {
    requireDoc(ed);
    Node* n = findNodeArg(ed, a, "id");
    if (!n->parent) throw ToolError("cannot move the page/root");
    Node* newParent = a.contains("parentId") ? findNodeArg(ed, a, "parentId") : n->parent;
    if (newParent->type == NodeType::Text) throw ToolError("cannot nest nodes inside a TEXT node");
    for (Node* p = newParent; p; p = p->parent)
        if (p == n) throw ToolError("cannot move a node into its own subtree");

    Node* oldParent = n->parent;
    auto& siblings = oldParent->children;
    size_t oldIndex = 0;
    std::unique_ptr<Node> holder;
    for (size_t i = 0; i < siblings.size(); ++i) {
        if (siblings[i].get() == n) {
            oldIndex = i;
            holder = std::move(siblings[i]);
            siblings.erase(siblings.begin() + static_cast<long long>(i));
            break;
        }
    }
    if (!holder) throw ToolError("internal: node not under its parent");

    size_t at = newParent->children.size();
    if (a.contains("index"))
        at = std::min(static_cast<size_t>(std::max(0, a["index"].get<int>())),
                      newParent->children.size());
    newParent->children.insert(newParent->children.begin() + static_cast<long long>(at),
                               std::move(holder));
    n->parent = newParent;

    UndoEntry e;
    TreeChange rem;
    rem.isInsert = false;
    rem.parent = oldParent;
    rem.index = oldIndex;
    rem.node = n;
    e.tree.push_back(std::move(rem));
    TreeChange ins;
    ins.isInsert = true;
    ins.parent = newParent;
    ins.index = at;
    ins.node = n;
    e.tree.push_back(std::move(ins));
    ed.undoStack.push_back(std::move(e));
    ed.redoStack.clear();
    ed.unsaved = true;
    ed.markDocChanged();
    ed.setStatus("MCP: moved " + n->name);
    return toolText(nodeBrief(ed, *n).dump(2));
}

void zoomToWorldRect(EditorState& ed, const WorldRect& b) {
    const float vw = static_cast<float>(ed.viewportW());
    const float vh = static_cast<float>(ed.viewportH());
    if (vw <= 0 || vh <= 0 || b.w() <= 0 || b.h() <= 0) return;  // headless / degenerate
    const float pad = 48;
    const float z = std::min((vw - pad * 2) / b.w(), (vh - pad * 2) / b.h());
    ed.cam.zoom = std::max(kZoomMin, std::min(kZoomMax, z));
    ed.cam.panX = (vw - b.w() * ed.cam.zoom) * 0.5f - b.x0 * ed.cam.zoom;
    ed.cam.panY = (vh - b.h() * ed.cam.zoom) * 0.5f - b.y0 * ed.cam.zoom;
    ed.lastViewChange = GetTime();
    ed.viewSettled = false;
}

json toolSetSelection(EditorState& ed, const json& a) {
    requireDoc(ed);
    std::vector<Node*> sel;
    for (const auto& idv : a.at("ids")) {
        Node* n = findNode(ed, idv.get<std::string>());
        if (!n) throw ToolError("node not found: " + idv.get<std::string>());
        if (!underPage(ed, n)) throw ToolError("node is not on the current page: " + n->id);
        sel.push_back(n);
    }
    ed.setSelection(sel);
    if (a.value("zoomTo", false) && !sel.empty()) {
        WorldRect b = worldBounds(*sel[0]);
        for (size_t i = 1; i < sel.size(); ++i) {
            const WorldRect o = worldBounds(*sel[i]);
            b.x0 = std::min(b.x0, o.x0);
            b.y0 = std::min(b.y0, o.y0);
            b.x1 = std::max(b.x1, o.x1);
            b.y1 = std::max(b.y1, o.y1);
        }
        zoomToWorldRect(ed, b);
    }
    return toolText("selected " + std::to_string(sel.size()) + " node(s)");
}

json toolSetPage(EditorState& ed, const json& a) {
    requireDoc(ed);
    ed.selectPage(a.value("index", 0));
    return toolText(editorStateJson(ed).dump(2));
}

json toolGetScreenshot(EditorState& ed, const json& a) {
    requireDoc(ed);
    ed.updateAbsoluteTransforms();
    Node* target = a.contains("nodeId") ? findNodeArg(ed, a, "nodeId") : ed.page;
    if (target != ed.page && !underPage(ed, target))
        throw ToolError("node is not on the current page (use set_page first)");

    WorldRect b;
    if (target == ed.page) {
        b.x0 = b.y0 = 1e30f;
        b.x1 = b.y1 = -1e30f;
        for (const auto& c : ed.page->children) {
            if (!c->visible) continue;
            const WorldRect o = worldBounds(*c);
            b.x0 = std::min(b.x0, o.x0);
            b.y0 = std::min(b.y0, o.y0);
            b.x1 = std::max(b.x1, o.x1);
            b.y1 = std::max(b.y1, o.y1);
        }
        if (b.x1 <= b.x0) throw ToolError("page is empty — nothing to screenshot");
    } else {
        b = worldBounds(*target);
    }
    const float pad = std::max(0.0f, a.value("padding", 0.0f));
    b.x0 -= pad;
    b.y0 -= pad;
    b.x1 += pad;
    b.y1 += pad;
    const float maxSize =
        std::max(64.0f, std::min(2048.0f, a.value("maxSize", 1024.0f)));
    const float s = std::min(maxSize / std::max(b.w(), b.h()), 2.0f);
    const int tw = std::max(1, static_cast<int>(std::ceil(b.w() * s)));
    const int th = std::max(1, static_cast<int>(std::ceil(b.h() * s)));

    figmalib::Renderer r;
    for (const auto& dir : ed.fontDirs) r.registerFontsFromDirectory(dir);
    if (!ed.imageDir.empty()) r.setImageDirectory(ed.imageDir);
    r.setFrame(ed.page);
    Mat23 view;
    view.m00 = view.m11 = s;
    view.m02 = -b.x0 * s;
    view.m12 = -b.y0 * s;
    if (!r.setTarget(static_cast<uint32_t>(tw), static_cast<uint32_t>(th)))
        throw ToolError("renderer setTarget failed");
    r.setViewTransform(view);
    r.markDirty();
    if (!r.render() || !r.pixels()) throw ToolError("render failed");

    Image img{};
    img.data = const_cast<uint32_t*>(r.pixels());
    img.width = tw;
    img.height = th;
    img.mipmaps = 1;
    img.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
    int pngSize = 0;
    unsigned char* png = ExportImageToMemory(img, ".png", &pngSize);
    if (!png || pngSize <= 0) throw ToolError("PNG encode failed");
    const std::string b64 = base64(png, static_cast<size_t>(pngSize));
    MemFree(png);

    json content = json::array();
    content.push_back({{"type", "image"}, {"data", b64}, {"mimeType", "image/png"}});
    content.push_back(
        {{"type", "text"},
         {"text", json{{"world", {b.x0, b.y0, b.x1, b.y1}}, {"pixelsPerUnit", s},
                       {"width", tw}, {"height", th}}
                      .dump()}});
    return json{{"content", std::move(content)}};
}

json toolSaveDocument(EditorState& ed, const json& a) {
    requireDoc(ed);
    if (a.contains("path")) ed.savePath = a["path"].get<std::string>();
    if (!figmalib::saveDocumentFile(*ed.file.document, ed.savePath))
        throw ToolError("save failed: " + ed.savePath);
    ed.unsaved = false;
    ed.setStatus("MCP: saved " + ed.savePath);
    return toolText("saved " + ed.savePath);
}

json toolOpenDocument(EditorState& ed, const json& a) {
    const std::string path = a.at("path").get<std::string>();
    if (!openFile(ed, path)) throw ToolError("failed to open " + path);
    return toolText(editorStateJson(ed).dump(2));
}

json toolUndoRedo(EditorState& ed, const json& a, bool redo) {
    requireDoc(ed);
    const int steps = std::max(1, std::min(100, a.value("steps", 1)));
    int done = 0;
    for (int i = 0; i < steps; ++i) {
        if (redo ? ed.redoStack.empty() : ed.undoStack.empty()) break;
        redo ? ed.redo() : ed.undo();
        ++done;
    }
    return toolText((redo ? "redid " : "undid ") + std::to_string(done) + " step(s)");
}

// ---- tool registry ----------------------------------------------------------

const json& toolsJson() {
    static const json tools = json::parse(R"JSON([
{
  "name": "get_editor_state",
  "description": "Current file, pages, selection and viewport of the running figmaedit editor. Call this first to orient yourself.",
  "inputSchema": {"type": "object", "properties": {}}
},
{
  "name": "get_node_tree",
  "description": "Layer tree outline (id/name/type/x/y/size per node). Defaults to the current page; pass nodeId (id or unique name) to scope, depth to limit.",
  "inputSchema": {"type": "object", "properties": {
    "nodeId": {"type": "string"},
    "depth": {"type": "integer", "minimum": 0}
  }}
},
{
  "name": "get_node",
  "description": "Full properties (fills, strokes, effects, text, layout) of one or more nodes by id or name.",
  "inputSchema": {"type": "object", "required": ["ids"], "properties": {
    "ids": {"type": "array", "items": {"type": "string"}}
  }}
},
{
  "name": "create_node",
  "description": "Create a node. Types: frame, group, section, rectangle, ellipse, line, vector, text, star, polygon. Parent defaults to the current page. Accepts every styling field of update_nodes (fill, stroke, cornerRadius, text, fontSize, svgPath for vectors, ...). Colors are '#RRGGBB' or '#RRGGBBAA'. Coordinates are relative to the parent's top-left.",
  "inputSchema": {"type": "object", "required": ["type"], "properties": {
    "type": {"type": "string"},
    "parentId": {"type": "string"},
    "index": {"type": "integer"},
    "name": {"type": "string"},
    "x": {"type": "number"}, "y": {"type": "number"},
    "width": {"type": "number"}, "height": {"type": "number"},
    "fill": {"description": "color string or paint object {type:SOLID|GRADIENT_LINEAR|...,color,stops,handles}"},
    "stroke": {"description": "color string or paint object"},
    "strokeWeight": {"type": "number"},
    "cornerRadius": {"type": "number"},
    "opacity": {"type": "number"},
    "text": {"type": "string", "description": "TEXT nodes: the characters"},
    "fontFamily": {"type": "string"}, "fontSize": {"type": "number"},
    "fontWeight": {"type": "integer"},
    "textAlignH": {"type": "string", "enum": ["LEFT","CENTER","RIGHT","JUSTIFIED"]},
    "textAlignV": {"type": "string", "enum": ["TOP","CENTER","BOTTOM"]},
    "svgPath": {"type": "string", "description": "VECTOR nodes: SVG path data in local coordinates (0,0..width,height)"},
    "evenOdd": {"type": "boolean"},
    "clipsContent": {"type": "boolean"}
  }}
},
{
  "name": "update_nodes",
  "description": "Batch-update node properties. Each update needs 'id' plus any of: name, x, y, width, height, visible, opacity, cornerRadius, cornerRadii[TL,TR,BR,BL], fills/fill, strokes/stroke, strokeWeight, strokeAlign(INSIDE|OUTSIDE|CENTER), strokeDashes, effects[{type:DROP_SHADOW|INNER_SHADOW|LAYER_BLUR,color,offsetX,offsetY,radius}], clipsContent, text, fontFamily, fontSize, fontWeight, italic, lineHeight, letterSpacing, textAlignH, textAlignV, constraints{h,v}, autoLayout{mode:NONE|HORIZONTAL|VERTICAL,itemSpacing,padding,...} (metadata for runtime reflow; the editor canvas does not auto-rearrange children), layoutGrow, svgPath. Undoable as one step.",
  "inputSchema": {"type": "object", "required": ["updates"], "properties": {
    "updates": {"type": "array", "items": {"type": "object", "required": ["id"],
      "properties": {"id": {"type": "string"}}}}
  }}
},
{
  "name": "delete_nodes",
  "description": "Delete nodes by id or name (undoable).",
  "inputSchema": {"type": "object", "required": ["ids"], "properties": {
    "ids": {"type": "array", "items": {"type": "string"}}
  }}
},
{
  "name": "duplicate_node",
  "description": "Deep-copy a node next to the original, offset by dx/dy (default 10,10). Returns the copy with fresh ids.",
  "inputSchema": {"type": "object", "required": ["id"], "properties": {
    "id": {"type": "string"}, "dx": {"type": "number"}, "dy": {"type": "number"}
  }}
},
{
  "name": "move_node",
  "description": "Reparent and/or reorder a node. index is the position among the new siblings (default: append). Note: x/y stay relative to the new parent — adjust them via update_nodes if needed.",
  "inputSchema": {"type": "object", "required": ["id"], "properties": {
    "id": {"type": "string"}, "parentId": {"type": "string"}, "index": {"type": "integer"}
  }}
},
{
  "name": "set_selection",
  "description": "Select nodes in the editor UI (and optionally zoom the viewport to them) — useful to show the user what you are working on.",
  "inputSchema": {"type": "object", "required": ["ids"], "properties": {
    "ids": {"type": "array", "items": {"type": "string"}},
    "zoomTo": {"type": "boolean"}
  }}
},
{
  "name": "set_page",
  "description": "Switch the current page by index (see get_editor_state).",
  "inputSchema": {"type": "object", "required": ["index"], "properties": {
    "index": {"type": "integer"}
  }}
},
{
  "name": "get_screenshot",
  "description": "Render the current page (or one node) offscreen and return a PNG — your eyes on the design. Verify your edits visually after making them. maxSize caps the long edge in pixels (default 1024).",
  "inputSchema": {"type": "object", "properties": {
    "nodeId": {"type": "string"},
    "maxSize": {"type": "number"},
    "padding": {"type": "number"}
  }}
},
{
  "name": "save_document",
  "description": "Save to the editor's save path (<original>.figmalib.json) or an explicit path. The saved JSON reloads losslessly in figmalib/figmaedit.",
  "inputSchema": {"type": "object", "properties": {"path": {"type": "string"}}}
},
{
  "name": "open_document",
  "description": "Open a .fig / canvas.json / Figma REST JSON file in the editor.",
  "inputSchema": {"type": "object", "required": ["path"], "properties": {"path": {"type": "string"}}}
},
{
  "name": "undo",
  "description": "Undo the last edit step(s) — shared history with the user's Ctrl+Z.",
  "inputSchema": {"type": "object", "properties": {"steps": {"type": "integer"}}}
},
{
  "name": "redo",
  "description": "Redo previously undone step(s).",
  "inputSchema": {"type": "object", "properties": {"steps": {"type": "integer"}}}
}
])JSON");
    return tools;
}

json runTool(EditorState& ed, const std::string& name, const json& args) {
    try {
        ensureNodeIds(ed);
        if (name == "get_editor_state") return toolText(editorStateJson(ed).dump(2));
        if (name == "get_node_tree") return toolGetNodeTree(ed, args);
        if (name == "get_node") return toolGetNode(ed, args);
        if (name == "create_node") return toolCreateNode(ed, args);
        if (name == "update_nodes") return toolUpdateNodes(ed, args);
        if (name == "delete_nodes") return toolDeleteNodes(ed, args);
        if (name == "duplicate_node") return toolDuplicateNode(ed, args);
        if (name == "move_node") return toolMoveNode(ed, args);
        if (name == "set_selection") return toolSetSelection(ed, args);
        if (name == "set_page") return toolSetPage(ed, args);
        if (name == "get_screenshot") return toolGetScreenshot(ed, args);
        if (name == "save_document") return toolSaveDocument(ed, args);
        if (name == "open_document") return toolOpenDocument(ed, args);
        if (name == "undo") return toolUndoRedo(ed, args, false);
        if (name == "redo") return toolUndoRedo(ed, args, true);
        return toolErrorResult("unknown tool: " + name);
    } catch (const json::exception& e) {
        return toolErrorResult(std::string("bad arguments: ") + e.what());
    } catch (const std::exception& e) {
        return toolErrorResult(e.what());
    }
}

// ---- JSON-RPC dispatch --------------------------------------------------------

json rpcResult(const json& id, json result) {
    return json{{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}};
}

json rpcErrorMsg(const json& id, int code, const std::string& msg) {
    return json{{"jsonrpc", "2.0"}, {"id", id},
                {"error", {{"code", code}, {"message", msg}}}};
}

// Returns the response, or null json for notifications. `ed` is only touched
// for tools/call — the net thread dispatches the other methods inline.
json handleRpc(EditorState& ed, const json& msg) {
    if (!msg.is_object() || msg.value("jsonrpc", "") != "2.0" || !msg.contains("method"))
        return rpcErrorMsg(msg.is_object() ? msg.value("id", json()) : json(), -32600,
                           "invalid request");
    const std::string method = msg["method"].get<std::string>();
    const bool isNotification = !msg.contains("id") || msg["id"].is_null();
    if (isNotification) return json();  // notifications need no reply
    const json id = msg["id"];
    const json params = msg.value("params", json::object());

    if (method == "initialize") {
        std::string ver = params.value("protocolVersion", "2025-03-26");
        if (ver != "2024-11-05" && ver != "2025-03-26" && ver != "2025-06-18")
            ver = "2025-03-26";
        return rpcResult(
            id, json{{"protocolVersion", ver},
                     {"capabilities", {{"tools", json::object()}}},
                     {"serverInfo", {{"name", "figmaedit"}, {"version", kServerVersion}}},
                     {"instructions",
                      "figmaedit is a live Figma-style design editor. Workflow: "
                      "get_editor_state -> get_node_tree / get_screenshot to see the "
                      "design -> create_node / update_nodes to edit -> get_screenshot "
                      "to verify visually. Every edit is undoable by the user (Ctrl+Z). "
                      "Node references accept ids or unique layer names. Colors are "
                      "'#RRGGBB' / '#RRGGBBAA'; coordinates are parent-relative pixels."}});
    }
    if (method == "ping") return rpcResult(id, json::object());
    if (method == "tools/list") return rpcResult(id, json{{"tools", toolsJson()}});
    if (method == "tools/call") {
        const std::string name = params.value("name", "");
        const json args = params.value("arguments", json::object());
        return rpcResult(id, runTool(ed, name, args));
    }
    return rpcErrorMsg(id, -32601, "method not found: " + method);
}

// ---- queue between net thread and main loop -----------------------------------

struct PendingCall {
    json request;
    json response;
    bool done = false;
    std::mutex m;
    std::condition_variable cv;
};

std::mutex gQueueMutex;
std::deque<std::shared_ptr<PendingCall>> gQueue;
std::atomic<bool> gAccepting{false};
EditorState* gEd = nullptr;
int gPort = 0;

void finishCall(const std::shared_ptr<PendingCall>& call, json response) {
    std::lock_guard<std::mutex> lk(call->m);
    call->response = std::move(response);
    call->done = true;
    call->cv.notify_all();
}

net::HttpResponse onHttpRequest(const std::string& method, const std::string& path,
                                const std::string& body) {
    if (path != "/mcp" && path != "/") return {404, "text/plain", "use /mcp"};
    if (method == "GET")
        return {405, "text/plain", "SSE streams are not supported; POST JSON-RPC here"};
    if (method == "DELETE") return {200, "text/plain", ""};
    if (method != "POST") return {405, "text/plain", "POST only"};

    json msg = json::parse(body, nullptr, false);
    if (msg.is_discarded())
        return {400, "application/json",
                rpcErrorMsg(json(), -32700, "parse error").dump()};
    if (msg.is_array())
        return {400, "application/json",
                rpcErrorMsg(json(), -32600, "batch requests are not supported").dump()};
    if (!msg.contains("id") || msg["id"].is_null())
        return {202, "application/json", ""};  // notification

    const std::string rpcMethod = msg.value("method", "");
    if (rpcMethod != "tools/call") {
        // Protocol-only methods never touch EditorState — answer inline so the
        // handshake works even while the main thread is busy or in a dialog.
        return {200, "application/json", handleRpc(*gEd, msg).dump()};
    }

    auto call = std::make_shared<PendingCall>();
    call->request = std::move(msg);
    {
        std::lock_guard<std::mutex> lk(gQueueMutex);
        if (!gAccepting)
            return {503, "application/json",
                    rpcErrorMsg(call->request["id"], -32000, "editor shutting down").dump()};
        gQueue.push_back(call);
    }
    std::unique_lock<std::mutex> lk(call->m);
    if (!call->cv.wait_for(lk, std::chrono::seconds(60), [&] { return call->done; })) {
        return {200, "application/json",
                rpcErrorMsg(call->request["id"], -32000,
                            "editor main loop is busy (modal dialog open?)")
                    .dump()};
    }
    return {200, "application/json", call->response.dump()};
}

}  // namespace

// ---- public API -----------------------------------------------------------------

bool mcpStart(EditorState& ed, int port) {
    gEd = &ed;
    {
        std::lock_guard<std::mutex> lk(gQueueMutex);
        gAccepting = true;
    }
    if (!net::serverStart(port, onHttpRequest)) {
        gAccepting = false;
        return false;
    }
    gPort = port;
    return true;
}

void mcpStop() {
    {
        // Refuse new calls, then unblock any handler waiting on the pump
        // (the net thread can't be joined while it waits on a queued call).
        std::lock_guard<std::mutex> lk(gQueueMutex);
        gAccepting = false;
        for (auto& call : gQueue)
            finishCall(call, rpcErrorMsg(call->request.value("id", json()), -32000,
                                         "editor shutting down"));
        gQueue.clear();
    }
    net::serverStop();
    gPort = 0;
    gEd = nullptr;
}

void mcpPump(EditorState& ed) {
    std::deque<std::shared_ptr<PendingCall>> batch;
    {
        std::lock_guard<std::mutex> lk(gQueueMutex);
        batch.swap(gQueue);
    }
    for (auto& call : batch) {
        json resp;
        try {
            resp = handleRpc(ed, call->request);
        } catch (const std::exception& e) {
            resp = rpcErrorMsg(call->request.value("id", json()), -32603,
                               std::string("internal error: ") + e.what());
        }
        finishCall(call, std::move(resp));
    }
}

int mcpPort() { return gPort; }

std::string mcpHandleMessageForTest(EditorState& ed, const std::string& body) {
    json msg = json::parse(body, nullptr, false);
    if (msg.is_discarded()) return rpcErrorMsg(json(), -32700, "parse error").dump();
    const json resp = handleRpc(ed, msg);
    return resp.is_null() ? std::string() : resp.dump();
}

}  // namespace figmaedit
