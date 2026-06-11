// fig2json "canvas.json" → figmalib::Document.
//
// Ported from fig2psd's input/load.ts + render/paths.ts. The format differs
// from the Figma REST API in several ways this file normalizes:
//   - nodes/paints/effects carry no `type` field → inferred structurally
//   - transforms are decomposed bags {x, y, rotation, scaleX, scaleY, skewX}
//   - colors are hex strings ("#rrggbb" / "#rrggbbaa")
//   - geometry is a flat commands array ["M",x,y,"L",...] or a vectorNetwork
//   - gradients carry an *inverse* unit-space matrix instead of handles
//   - component INSTANCEs have no children; they hydrate from `components`
//     masters or from baked `derivedSymbolData`
//   - shared styles are string references into the top-level `styles` map

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "figmalib/parser.h"

namespace figmalib {

using json = nlohmann::json;

namespace {

constexpr float kPi = 3.14159265358979323846f;

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

std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// "#rgb" / "#rgba" / "#rrggbb" / "#rrggbbaa" → Color. Opaque black on bad input.
Color parseHexColor(const std::string& input) {
    Color c{0, 0, 0, 1};
    std::string hex = input;
    if (!hex.empty() && hex[0] == '#') hex.erase(0, 1);
    if (hex.size() == 3 || hex.size() == 4) {
        std::string expanded;
        for (char ch : hex) {
            expanded += ch;
            expanded += ch;
        }
        hex = expanded;
    }
    if (hex.size() != 6 && hex.size() != 8) return c;
    auto nibble = [](char ch) -> int {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
        if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
        return 0;
    };
    auto byteAt = [&](size_t i) { return (nibble(hex[i]) << 4 | nibble(hex[i + 1])) / 255.0f; };
    c.r = byteAt(0);
    c.g = byteAt(2);
    c.b = byteAt(4);
    c.a = hex.size() == 8 ? byteAt(6) : 1.0f;
    return c;
}

// Decomposed transform bag → Mat23, CSS order: translate · rotate · scale · skewX.
Mat23 transformOf(const json& node) {
    Mat23 m;
    auto it = node.find("transform");
    if (it == node.end() || !it->is_object()) return m;
    const json& t = *it;
    const float tx = jfloat(t, "x"), ty = jfloat(t, "y");
    const float rot = jfloat(t, "rotation") * kPi / 180.0f;
    const float sx = jfloat(t, "scaleX", 1.0f), sy = jfloat(t, "scaleY", 1.0f);
    const float skx = std::tan(jfloat(t, "skewX") * kPi / 180.0f);
    const float c = std::cos(rot), s = std::sin(rot);
    // rotate · scale · skewX (column-vector convention)
    m.m00 = c * sx;
    m.m01 = (c * skx - s) * sy;
    m.m10 = s * sx;
    m.m11 = (s * skx + c) * sy;
    m.m02 = tx;
    m.m12 = ty;
    return m;
}

// ---- geometry -------------------------------------------------------------

void appendNum(std::string& out, double v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.6g", v);
    out += buf;
    out += ' ';
}

// Flat commands array ["M",x,y,"L",x,y,"Q",...,"C",...,"Z"] → SVG path data.
std::string commandsToSvgPath(const json& cmds) {
    if (!cmds.is_array()) return {};
    std::string out;
    size_t i = 0;
    while (i < cmds.size()) {
        if (!cmds[i].is_string()) break;
        const std::string op = cmds[i].get<std::string>();
        size_t pairs = 0;
        if (op == "M" || op == "L") pairs = 1;
        else if (op == "Q") pairs = 2;
        else if (op == "C") pairs = 3;
        else if (op == "Z") {
            out += "Z ";
            ++i;
            continue;
        } else {
            break;  // unknown command
        }
        if (i + pairs * 2 >= cmds.size()) break;
        out += op;
        out += ' ';
        bool ok = true;
        for (size_t k = 0; k < pairs * 2; ++k) {
            const json& v = cmds[i + 1 + k];
            if (!v.is_number()) {
                ok = false;
                break;
            }
            appendNum(out, v.get<double>());
        }
        if (!ok) break;
        i += 1 + pairs * 2;
    }
    return out;
}

// vectorNetwork {vertices, segments, regions} → SVG path data, scaled from
// normalizedSize into node-local pixels. Port of applyVectorNetworkToPath.
std::string vectorNetworkToSvgPath(const json& network, float scaleX, float scaleY) {
    auto vIt = network.find("vertices");
    auto sIt = network.find("segments");
    if (vIt == network.end() || !vIt->is_array() || sIt == network.end() || !sIt->is_array())
        return {};
    const json& vertices = *vIt;
    const json& segments = *sIt;
    if (vertices.empty() || segments.empty()) return {};

    auto vx = [&](int i) -> float {
        return i >= 0 && i < static_cast<int>(vertices.size())
                   ? jfloat(vertices[i], "x") * scaleX
                   : 0.0f;
    };
    auto vy = [&](int i) -> float {
        return i >= 0 && i < static_cast<int>(vertices.size())
                   ? jfloat(vertices[i], "y") * scaleY
                   : 0.0f;
    };
    auto endpointVertex = [](const json& seg, const char* key) -> int {
        auto it = seg.find(key);
        if (it == seg.end() || !it->is_object()) return -1;
        auto v = it->find("vertex");
        return (v != it->end() && v->is_number()) ? v->get<int>() : -1;
    };

    std::string out;
    auto moveTo = [&](float x, float y) {
        out += "M ";
        appendNum(out, x);
        appendNum(out, y);
    };

    auto drawSeg = [&](const json& seg, bool reversed) {
        const json& s = reversed ? seg["end"] : seg["start"];
        const json& e = reversed ? seg["start"] : seg["end"];
        const int sv = endpointVertex(seg, reversed ? "end" : "start");
        const int ev = endpointVertex(seg, reversed ? "start" : "end");
        const float sx = vx(sv), sy = vy(sv);
        const float ex = vx(ev), ey = vy(ev);
        const float sdx = jfloat(s, "dx") * scaleX, sdy = jfloat(s, "dy") * scaleY;
        const float edx = jfloat(e, "dx") * scaleX, edy = jfloat(e, "dy") * scaleY;
        if (sdx == 0 && sdy == 0 && edx == 0 && edy == 0) {
            out += "L ";
            appendNum(out, ex);
            appendNum(out, ey);
        } else {
            out += "C ";
            appendNum(out, sx + sdx);
            appendNum(out, sy + sdy);
            appendNum(out, ex + edx);
            appendNum(out, ey + edy);
            appendNum(out, ex);
            appendNum(out, ey);
        }
    };

    auto emitLoop = [&](const std::vector<int>& segIdx) {
        if (segIdx.empty()) return;
        const json& first = segments[segIdx[0]];
        bool firstReversed = false;
        if (segIdx.size() >= 2) {
            const json& next = segments[segIdx[1]];
            const int fe = endpointVertex(first, "end");
            if (fe != endpointVertex(next, "start") && fe != endpointVertex(next, "end"))
                firstReversed = true;
        }
        const int startV = endpointVertex(first, firstReversed ? "end" : "start");
        moveTo(vx(startV), vy(startV));
        int cursor = endpointVertex(first, firstReversed ? "start" : "end");
        drawSeg(first, firstReversed);
        for (size_t i = 1; i < segIdx.size(); ++i) {
            const json& seg = segments[segIdx[i]];
            const bool reversed = endpointVertex(seg, "end") == cursor;
            if (!reversed && endpointVertex(seg, "start") != cursor) {
                const int sv = endpointVertex(seg, "start");
                moveTo(vx(sv), vy(sv));  // disconnected — restart
            }
            drawSeg(seg, reversed);
            cursor = endpointVertex(seg, reversed ? "start" : "end");
        }
        out += "Z ";
    };

    auto rIt = network.find("regions");
    if (rIt != network.end() && rIt->is_array() && !rIt->empty()) {
        for (const json& region : *rIt) {
            auto loops = region.find("loops");
            if (loops == region.end() || !loops->is_array()) continue;
            for (const json& loop : *loops) {
                auto segs = loop.find("segments");
                if (segs == loop.end() || !segs->is_array()) continue;
                std::vector<int> idx;
                for (const json& v : *segs)
                    if (v.is_number()) idx.push_back(v.get<int>());
                emitLoop(idx);
            }
        }
        return out;
    }

    // Open stroke-only network: stitch segments into chains by vertex
    // adjacency (stored order is not traversal order).
    std::vector<bool> used(segments.size(), false);
    std::unordered_map<int, std::vector<int>> byVertex;
    for (size_t i = 0; i < segments.size(); ++i) {
        byVertex[endpointVertex(segments[i], "start")].push_back(static_cast<int>(i));
        byVertex[endpointVertex(segments[i], "end")].push_back(static_cast<int>(i));
    }
    std::vector<int> endpointVerts;
    for (const auto& [v, segs] : byVertex)
        if (segs.size() == 1) endpointVerts.push_back(v);

    size_t remaining = segments.size();
    while (remaining > 0) {
        int startVertex = -1, startSeg = -1;
        for (int v : endpointVerts) {
            for (int i : byVertex[v]) {
                if (!used[i]) {
                    startVertex = v;
                    startSeg = i;
                    break;
                }
            }
            if (startSeg >= 0) break;
        }
        if (startSeg < 0) {  // closed cycle: pick any remaining
            for (size_t i = 0; i < used.size(); ++i) {
                if (!used[i]) {
                    startSeg = static_cast<int>(i);
                    startVertex = endpointVertex(segments[i], "start");
                    break;
                }
            }
        }
        if (startSeg < 0) break;
        int cursor = startVertex;
        moveTo(vx(cursor), vy(cursor));
        int segIdx = startSeg;
        while (segIdx >= 0 && !used[segIdx]) {
            const json& seg = segments[segIdx];
            const bool reversed = endpointVertex(seg, "end") == cursor;
            drawSeg(seg, reversed);
            used[segIdx] = true;
            --remaining;
            cursor = endpointVertex(seg, reversed ? "start" : "end");
            segIdx = -1;
            for (int i : byVertex[cursor]) {
                if (!used[i]) {
                    segIdx = i;
                    break;
                }
            }
        }
    }
    return out;
}

// AABB of a commands array — used to synthesize size/transform for
// derivedSymbolData entries that carry neither.
bool commandsAABB(const json& cmds, float& minX, float& minY, float& maxX, float& maxY) {
    minX = minY = 1e30f;
    maxX = maxY = -1e30f;
    bool any = false;
    size_t i = 0;
    while (i < cmds.size()) {
        if (!cmds[i].is_string()) break;
        const std::string op = cmds[i].get<std::string>();
        size_t pairs = op == "M" || op == "L" ? 1 : op == "Q" ? 2 : op == "C" ? 3 : 0;
        if (op == "Z" || pairs == 0) {
            ++i;
            continue;
        }
        for (size_t k = 0; k < pairs; ++k) {
            if (i + 2 + k * 2 >= cmds.size()) break;
            const json& xj = cmds[i + 1 + k * 2];
            const json& yj = cmds[i + 2 + k * 2];
            if (xj.is_number() && yj.is_number()) {
                const float x = xj.get<float>(), y = yj.get<float>();
                minX = std::fmin(minX, x);
                maxX = std::fmax(maxX, x);
                minY = std::fmin(minY, y);
                maxY = std::fmax(maxY, y);
                any = true;
            }
        }
        i += 1 + pairs * 2;
    }
    return any;
}

json shiftCommands(const json& cmds, float dx, float dy) {
    json out = json::array();
    size_t i = 0;
    while (i < cmds.size()) {
        if (!cmds[i].is_string()) break;
        const std::string op = cmds[i].get<std::string>();
        size_t pairs = op == "M" || op == "L" ? 1 : op == "Q" ? 2 : op == "C" ? 3 : 0;
        out.push_back(op);
        if (op == "Z" || pairs == 0) {
            ++i;
            continue;
        }
        for (size_t k = 0; k < pairs; ++k) {
            const float x = cmds[i + 1 + k * 2].is_number() ? cmds[i + 1 + k * 2].get<float>() : 0;
            const float y = cmds[i + 2 + k * 2].is_number() ? cmds[i + 2 + k * 2].get<float>() : 0;
            out.push_back(x + dx);
            out.push_back(y + dy);
        }
        i += 1 + pairs * 2;
    }
    return out;
}

// ---- guid keys (two different orders — see load.ts) ------------------------

std::string guidKey(const json& g) {  // derivedSymbolData paths: "localID:sessionID"
    if (!g.is_object()) return {};
    return std::to_string(static_cast<long long>(jfloat(g, "localID"))) + ":" +
           std::to_string(static_cast<long long>(jfloat(g, "sessionID")));
}

std::string guidPathKey(const json& guids) {
    if (!guids.is_array()) return {};
    std::string out;
    for (size_t i = 0; i < guids.size(); ++i) {
        if (i) out += '/';
        out += guidKey(guids[i]);
    }
    return out;
}

std::string symbolIDKey(const json& g) {  // components/overrides: "sessionID:localID"
    if (!g.is_object()) return {};
    return std::to_string(static_cast<long long>(jfloat(g, "sessionID"))) + ":" +
           std::to_string(static_cast<long long>(jfloat(g, "localID")));
}

// ---- JSON-level hydration (instances, styles) ------------------------------
// Runs on the raw JSON tree before node parsing, mirroring load.ts exactly.

constexpr size_t kDerivedSymbolDataMax = 500;

const json* findStyle(const std::unordered_map<std::string, const json*>& styles,
                      const std::string& key) {
    auto it = styles.find(key);
    return it == styles.end() ? nullptr : it->second;
}

void applyInstanceScaleToClones(json& cloned, const json& instSize, const json& masterSize) {
    const float mw = instSize.is_object() && masterSize.is_object() ? jfloat(masterSize, "x") : 0;
    const float mh = masterSize.is_object() ? jfloat(masterSize, "y") : 0;
    const float iw = instSize.is_object() ? jfloat(instSize, "x") : 0;
    const float ih = instSize.is_object() ? jfloat(instSize, "y") : 0;
    if (mw <= 0 || mh <= 0 || iw <= 0 || ih <= 0) return;
    const float sx = iw / mw, sy = ih / mh;
    if (sx == 1 && sy == 1) return;
    for (json& c : cloned) {
        json t = c.contains("transform") && c["transform"].is_object() ? c["transform"]
                                                                       : json::object();
        t["x"] = jfloat(t, "x") * sx;
        t["y"] = jfloat(t, "y") * sy;
        t["scaleX"] = jfloat(t, "scaleX", 1.0f) * sx;
        t["scaleY"] = jfloat(t, "scaleY", 1.0f) * sy;
        c["transform"] = t;
    }
}

void applySymbolOverridesToClone(json& cloned, const json& overrides,
                                 const std::unordered_map<std::string, const json*>& styles) {
    // Index every cloned descendant by its guid ("sessionID:localID").
    std::unordered_map<std::string, json*> byGuid;
    std::function<void(json&)> index = [&](json& nodes) {
        for (json& n : nodes) {
            if (n.contains("guid")) {
                const std::string key = symbolIDKey(n["guid"]);
                if (!key.empty()) byGuid[key] = &n;
            }
            if (n.contains("children") && n["children"].is_array()) index(n["children"]);
        }
    };
    index(cloned);

    for (const json& ov : overrides) {
        if (!ov.is_object()) continue;
        auto gp = ov.find("guidPath");
        if (gp == ov.end() || !gp->is_object()) continue;
        auto guids = gp->find("guids");
        if (guids == gp->end() || !guids->is_array() || guids->empty()) continue;
        const std::string targetKey = symbolIDKey(guids->back());
        auto found = byGuid.find(targetKey);
        if (found == byGuid.end()) continue;
        json& target = *found->second;

        // fillPaints: inline wins, then styleIdForFill via the styles map.
        if (ov.contains("fillPaints") && ov["fillPaints"].is_array() &&
            !ov["fillPaints"].empty()) {
            target["fillPaints"] = ov["fillPaints"];
        } else if (ov.contains("styleIdForFill") && ov["styleIdForFill"].is_string()) {
            if (const json* st = findStyle(styles, ov["styleIdForFill"].get<std::string>())) {
                if (st->contains("fillPaints") && (*st)["fillPaints"].is_array() &&
                    !(*st)["fillPaints"].empty()) {
                    target["fillPaints"] = (*st)["fillPaints"];
                }
            }
        }
        if (ov.contains("strokePaints") && ov["strokePaints"].is_array() &&
            !ov["strokePaints"].empty()) {
            target["strokePaints"] = ov["strokePaints"];
        } else if (ov.contains("styleIdForStrokeFill") && ov["styleIdForStrokeFill"].is_string()) {
            if (const json* st =
                    findStyle(styles, ov["styleIdForStrokeFill"].get<std::string>())) {
                if (st->contains("strokePaints") && !(*st)["strokePaints"].empty())
                    target["strokePaints"] = (*st)["strokePaints"];
                else if (st->contains("fillPaints") && !(*st)["fillPaints"].empty())
                    target["strokePaints"] = (*st)["fillPaints"];
            }
        }
        if (ov.contains("strokeWeight") && ov["strokeWeight"].is_number())
            target["strokeWeight"] = ov["strokeWeight"];
    }
}

void hydrateFromComponentMaster(json& node,
                                const std::unordered_map<std::string, const json*>& components,
                                const std::unordered_map<std::string, const json*>& styles) {
    if (node.contains("children") && node["children"].is_array() && !node["children"].empty())
        return;
    auto sd = node.find("symbolData");
    if (sd == node.end() || !sd->is_object()) return;
    auto sid = sd->find("symbolID");
    if (sid == sd->end() || !sid->is_object()) return;
    auto master = components.find(symbolIDKey(*sid));
    if (master == components.end()) return;
    const json& m = *master->second;
    if (!m.contains("children") || !m["children"].is_array() || m["children"].empty()) return;

    json cloned = m["children"];  // deep copy
    applyInstanceScaleToClones(cloned, node.value("size", json()), m.value("size", json()));
    auto ovs = sd->find("symbolOverrides");
    if (ovs != sd->end() && ovs->is_array() && !ovs->empty())
        applySymbolOverridesToClone(cloned, *ovs, styles);
    node["children"] = std::move(cloned);
}

// Baked derivedSymbolData → synthetic children. Port of hydrateDerivedSymbolData.
void hydrateDerivedSymbolData(json& node) {
    auto dIt = node.find("derivedSymbolData");
    if (dIt == node.end() || !dIt->is_array() || dIt->empty()) return;
    if (node.contains("children") && node["children"].is_array() && !node["children"].empty())
        return;
    const json& entries = *dIt;

    auto concatCommands = [](const json& list) {
        json out = json::array();
        if (list.is_array()) {
            for (const json& g : list) {
                if (g.is_object() && g.contains("commands") && g["commands"].is_array()) {
                    for (const json& c : g["commands"]) out.push_back(c);
                }
            }
        }
        return out;
    };

    // Bare instance: entry[0] carries the whole baked silhouette.
    const json& e0 = entries[0];
    json e0Fill = concatCommands(e0.value("fillGeometry", json()));
    json e0Stroke = concatCommands(e0.value("strokeGeometry", json()));
    const json& e0Geom = !e0Fill.empty() ? e0Fill : e0Stroke;
    if (!e0Geom.empty()) {
        node["commands"] = e0Geom;
        // First symbolOverrides entry with paint feeds missing fills/strokes.
        auto sd = node.find("symbolData");
        if (sd != node.end() && sd->is_object()) {
            auto ovs = sd->find("symbolOverrides");
            if (ovs != sd->end() && ovs->is_array()) {
                for (const json& ov : *ovs) {
                    if (!ov.is_object()) continue;
                    const bool hasPaint = ov.contains("fillPaints") || ov.contains("strokePaints");
                    if (!hasPaint) continue;
                    if (ov.contains("fillPaints") && !node.contains("fillPaints"))
                        node["fillPaints"] = ov["fillPaints"];
                    if (ov.contains("strokePaints") && !node.contains("strokePaints"))
                        node["strokePaints"] = ov["strokePaints"];
                    if (ov.contains("strokeWeight") && !node.contains("strokeWeight"))
                        node["strokeWeight"] = ov["strokeWeight"];
                    break;
                }
            }
        }
        return;
    }

    if (entries.size() <= 1 || entries.size() > kDerivedSymbolDataMax) return;

    // Synthesize one node per entry [1..N], then rebuild the hierarchy from
    // guidPath prefixes.
    std::unordered_map<std::string, int> pathToIndex;
    for (size_t i = 0; i < entries.size(); ++i) {
        if (entries[i].is_object() && entries[i].contains("guidPath"))
            pathToIndex[guidPathKey(entries[i]["guidPath"].value("guids", json()))] =
                static_cast<int>(i);
    }
    const std::string selfKey =
        e0.is_object() && e0.contains("guidPath") ? guidPathKey(e0["guidPath"].value("guids", json()))
                                                  : std::string();

    std::unordered_map<int, json> synthesized;
    for (size_t i = 1; i < entries.size(); ++i) {
        const json& e = entries[i];
        if (!e.is_object()) continue;
        json fillGeom = e.contains("fillGeometry") && e["fillGeometry"].is_array() &&
                                !e["fillGeometry"].empty()
                            ? e["fillGeometry"][0].value("commands", json())
                            : json();
        json strokeGeom = e.contains("strokeGeometry") && e["strokeGeometry"].is_array() &&
                                  !e["strokeGeometry"].empty()
                              ? e["strokeGeometry"][0].value("commands", json())
                              : json();
        json commands = fillGeom.is_array() && !fillGeom.empty() ? fillGeom
                        : strokeGeom.is_array() && !strokeGeom.empty() ? strokeGeom
                                                                       : json();

        json size = e.value("size", json());
        json transform = e.value("transform", json());
        if (commands.is_array() && !commands.empty() && size.is_null() && transform.is_null()) {
            float minX, minY, maxX, maxY;
            if (commandsAABB(commands, minX, minY, maxX, maxY)) {
                transform = json{{"x", minX}, {"y", minY}};
                size = json{{"x", std::fmax(1.0f, maxX - minX)},
                            {"y", std::fmax(1.0f, maxY - minY)}};
                commands = shiftCommands(commands, -minX, -minY);
            }
        }

        json n = json::object();
        n["name"] = jstr(node, "name", "Instance") + " - part " + std::to_string(i);
        if (!size.is_null()) n["size"] = size;
        if (!transform.is_null()) n["transform"] = transform;
        if (e.contains("effects")) n["effects"] = e["effects"];
        if (commands.is_array() && !commands.empty()) {
            n["commands"] = commands;
            n["fillPaints"] = e.contains("fillPaints") ? e["fillPaints"]
                                                       : node.value("fillPaints", json());
            n["strokePaints"] = e.contains("strokePaints") ? e["strokePaints"]
                                                           : node.value("strokePaints", json());
            if (e.contains("strokeWeight")) n["strokeWeight"] = e["strokeWeight"];
            else if (node.contains("strokeWeight")) n["strokeWeight"] = node["strokeWeight"];
        } else {
            if (e.contains("fillPaints")) n["fillPaints"] = e["fillPaints"];
            if (e.contains("strokePaints")) n["strokePaints"] = e["strokePaints"];
            if (e.contains("strokeWeight")) n["strokeWeight"] = e["strokeWeight"];
        }
        synthesized[static_cast<int>(i)] = std::move(n);
    }

    // Attach children to parents by guid prefix.
    std::vector<int> topLevel;
    std::unordered_map<int, std::vector<int>> childrenOf;
    for (size_t i = 1; i < entries.size(); ++i) {
        const json& e = entries[i];
        if (!e.is_object() || !synthesized.count(static_cast<int>(i))) continue;
        json guids = e.contains("guidPath") ? e["guidPath"].value("guids", json()) : json();
        json prefix = json::array();
        if (guids.is_array() && guids.size() > 1)
            prefix = json(guids.begin(), guids.end() - 1);
        const std::string prefixKey = guidPathKey(prefix);
        int parentIdx = -1;
        if (!prefixKey.empty() && prefixKey != selfKey) {
            auto it = pathToIndex.find(prefixKey);
            if (it != pathToIndex.end() && synthesized.count(it->second))
                parentIdx = it->second;
        }
        if (parentIdx >= 0) childrenOf[parentIdx].push_back(static_cast<int>(i));
        else topLevel.push_back(static_cast<int>(i));
    }

    // Assemble bottom-up and prune geometry-less leaves.
    std::function<json(int)> build = [&](int idx) -> json {
        json n = synthesized[idx];
        json kids = json::array();
        for (int c : childrenOf[idx]) {
            json k = build(c);
            if (!k.is_null()) kids.push_back(std::move(k));
        }
        if (!kids.empty()) {
            n["children"] = std::move(kids);
            return n;
        }
        const bool hasGeom = n.contains("commands") && n["commands"].is_array() &&
                             !n["commands"].empty();
        return hasGeom ? n : json();
    };
    json kept = json::array();
    for (int i : topLevel) {
        json k = build(i);
        if (!k.is_null()) kept.push_back(std::move(k));
    }
    if (!kept.empty()) node["children"] = std::move(kept);
}

void hydrateRecursive(json& node,
                      const std::unordered_map<std::string, const json*>& components,
                      const std::unordered_map<std::string, const json*>& styles) {
    if (!components.empty()) hydrateFromComponentMaster(node, components, styles);
    hydrateDerivedSymbolData(node);
    if (node.contains("children") && node["children"].is_array()) {
        for (json& c : node["children"]) hydrateRecursive(c, components, styles);
    }
}

// styleIdForFill / styleIdForStrokeFill / styleIdForText resolution.
void resolveSharedStyles(json& node,
                         const std::unordered_map<std::string, const json*>& styles) {
    if (node.contains("styleIdForFill") && node["styleIdForFill"].is_string()) {
        if (const json* st = findStyle(styles, node["styleIdForFill"].get<std::string>())) {
            if (st->contains("fillPaints") && !(*st)["fillPaints"].empty())
                node["fillPaints"] = (*st)["fillPaints"];
        }
    }
    if (node.contains("styleIdForStrokeFill") && node["styleIdForStrokeFill"].is_string()) {
        if (const json* st =
                findStyle(styles, node["styleIdForStrokeFill"].get<std::string>())) {
            if (st->contains("strokePaints") && !(*st)["strokePaints"].empty())
                node["strokePaints"] = (*st)["strokePaints"];
            else if (st->contains("fillPaints") && !(*st)["fillPaints"].empty())
                node["strokePaints"] = (*st)["fillPaints"];
        }
    }
    if (node.contains("styleIdForText") && node["styleIdForText"].is_string()) {
        if (const json* st = findStyle(styles, node["styleIdForText"].get<std::string>())) {
            // Text styles win over the node's stale snapshot.
            for (const char* field : {"fontName", "fontSize", "fontWeight", "letterSpacing",
                                      "lineHeight", "textCase", "textDecoration"}) {
                if (st->contains(field)) node[field] = (*st)[field];
            }
        }
    }
    // Rich-text run overrides reference shared styles the same way; inline
    // them so node parsing only has to read resolved fields. Inline values
    // already on the entry win (a per-run override of the referenced style).
    if (node.contains("textData") && node["textData"].is_object()) {
        json& td = node["textData"];
        if (td.contains("styleOverrideTable") && td["styleOverrideTable"].is_array()) {
            for (json& ov : td["styleOverrideTable"]) {
                if (!ov.is_object()) continue;
                if (ov.contains("styleIdForFill") && ov["styleIdForFill"].is_string() &&
                    (!ov.contains("fillPaints") || ov["fillPaints"].empty())) {
                    if (const json* st =
                            findStyle(styles, ov["styleIdForFill"].get<std::string>())) {
                        if (st->contains("fillPaints") && !(*st)["fillPaints"].empty())
                            ov["fillPaints"] = (*st)["fillPaints"];
                    }
                }
                if (ov.contains("styleIdForText") && ov["styleIdForText"].is_string()) {
                    if (const json* st =
                            findStyle(styles, ov["styleIdForText"].get<std::string>())) {
                        for (const char* field : {"fontName", "fontSize", "fontWeight",
                                                  "letterSpacing", "lineHeight"}) {
                            if (st->contains(field) && !ov.contains(field))
                                ov[field] = (*st)[field];
                        }
                    }
                }
            }
        }
    }
    if (node.contains("children") && node["children"].is_array()) {
        for (json& c : node["children"]) resolveSharedStyles(c, styles);
    }
}

// ---- node parsing ----------------------------------------------------------

Paint parseCanvasPaint(const json& j) {
    Paint p;
    std::string type = jstr(j, "type");
    if (type.empty()) {  // infer (load.ts inferPaintType)
        if (j.contains("image")) type = "IMAGE";
        else if (j.contains("stops")) type = "GRADIENT_LINEAR";
        else type = "SOLID";
    }
    if (type == "SOLID") p.type = PaintType::Solid;
    else if (type == "GRADIENT_LINEAR") p.type = PaintType::GradientLinear;
    else if (type == "GRADIENT_RADIAL") p.type = PaintType::GradientRadial;
    else if (type == "GRADIENT_ANGULAR") p.type = PaintType::GradientAngular;
    else if (type == "GRADIENT_DIAMOND") p.type = PaintType::GradientDiamond;
    else if (type == "IMAGE") p.type = PaintType::Image;

    p.visible = jbool(j, "visible", true);
    p.opacity = jfloat(j, "opacity", 1.0f);
    if (j.contains("color") && j["color"].is_string())
        p.color = parseHexColor(j["color"].get<std::string>());

    if (j.contains("stops") && j["stops"].is_array()) {
        for (const json& s : j["stops"]) {
            GradientStop stop;
            stop.position = jfloat(s, "position");
            if (s.contains("color") && s["color"].is_string())
                stop.color = parseHexColor(s["color"].get<std::string>());
            p.stops.push_back(stop);
        }
    }

    // Gradient geometry: the paint `transform` bag is the matrix M mapping a
    // frame-normalized point INTO gradient-local space (canonical line
    // (0,0.5)→(1,0.5); canonical radial center (0.5,0.5) r=0.5). Apply M⁻¹ to
    // the canonical anchors to get REST-style normalized handles, which is
    // what the scene builder consumes. (Port of fig2psd linearEndpoints.)
    if (p.type != PaintType::Solid && p.type != PaintType::Image) {
        float tx = 0, ty = 0, m00 = 1, m01 = 0, m10 = 0, m11 = 1;
        if (j.contains("transform") && j["transform"].is_object()) {
            const json& t = j["transform"];
            tx = jfloat(t, "x");
            ty = jfloat(t, "y");
            const float rad = jfloat(t, "rotation") * kPi / 180.0f;
            const float sx = jfloat(t, "scaleX", 1.0f), sy = jfloat(t, "scaleY", 1.0f);
            m00 = std::cos(rad) * sx;
            m01 = -std::sin(rad) * sy;
            m10 = std::sin(rad) * sx;
            m11 = std::cos(rad) * sy;
        }
        const float det = m00 * m11 - m01 * m10;
        auto setHandles = [&](float h0x, float h0y, float h1x, float h1y) {
            p.handle0 = {h0x, h0y};
            p.handle1 = {h1x, h1y};
        };
        if (std::fabs(det) < 1e-9f) {
            if (p.type == PaintType::GradientLinear) setHandles(0, 0.5f, 1, 0.5f);
            else setHandles(0.5f, 0.5f, 1.0f, 0.5f);
        } else {
            const float i00 = m11 / det, i01 = -m01 / det;
            const float i10 = -m10 / det, i11 = m00 / det;
            const float i02 = -(i00 * tx + i01 * ty);
            const float i12 = -(i10 * tx + i11 * ty);
            auto inv = [&](float x, float y, float& ox, float& oy) {
                ox = i00 * x + i01 * y + i02;
                oy = i10 * x + i11 * y + i12;
            };
            float ax, ay, bx, by;
            if (p.type == PaintType::GradientLinear) {
                inv(0, 0.5f, ax, ay);
                inv(1, 0.5f, bx, by);
            } else {
                inv(0.5f, 0.5f, ax, ay);  // center
                inv(1.0f, 0.5f, bx, by);  // radius tip
            }
            setHandles(ax, ay, bx, by);
        }
    }

    if (j.contains("image") && j["image"].is_object()) {
        p.imageRef = jstr(j["image"], "filename", jstr(j["image"], "name"));
        // fig2json prefixes refs with "images/"; the renderer joins them with
        // the images dir itself.
        const std::string prefix = "images/";
        if (p.imageRef.rfind(prefix, 0) == 0) p.imageRef = p.imageRef.substr(prefix.size());
        const std::string prefixWin = "images\\";
        if (p.imageRef.rfind(prefixWin, 0) == 0) p.imageRef = p.imageRef.substr(prefixWin.size());
    }
    p.imageScaleMode = jstr(j, "scaleMode", "FILL");
    return p;
}

void parseCanvasPaints(const json& j, const char* key, std::vector<Paint>& out) {
    auto it = j.find(key);
    if (it == j.end() || !it->is_array()) return;
    for (const json& p : *it)
        if (p.is_object()) out.push_back(parseCanvasPaint(p));
}

void parseCanvasEffects(const json& j, std::vector<Effect>& out) {
    auto it = j.find("effects");
    if (it == j.end() || !it->is_array()) return;
    for (const json& e : *it) {
        if (!e.is_object()) continue;
        Effect fx;
        std::string type = jstr(e, "type");
        if (type == "FOREGROUND_BLUR") type = "LAYER_BLUR";
        if (type.empty()) {  // structural inference for old fixtures
            const bool hasOffset = e.contains("offset");
            const bool hasColor = e.contains("color") && e["color"].is_string();
            type = hasOffset && hasColor ? "DROP_SHADOW" : hasColor ? "INNER_SHADOW" : "LAYER_BLUR";
        }
        if (type == "DROP_SHADOW") fx.type = Effect::Type::DropShadow;
        else if (type == "INNER_SHADOW") fx.type = Effect::Type::InnerShadow;
        else if (type == "LAYER_BLUR") fx.type = Effect::Type::LayerBlur;
        else if (type == "BACKGROUND_BLUR") fx.type = Effect::Type::BackgroundBlur;
        else continue;
        fx.visible = jbool(e, "visible", true);
        if (e.contains("color") && e["color"].is_string())
            fx.color = parseHexColor(e["color"].get<std::string>());
        if (e.contains("offset") && e["offset"].is_object()) {
            fx.offsetX = jfloat(e["offset"], "x");
            fx.offsetY = jfloat(e["offset"], "y");
        }
        fx.radius = jfloat(e, "radius");
        fx.spread = jfloat(e, "spread");
        out.push_back(fx);
    }
}

// lineHeight / letterSpacing values arrive as number, "36px", "150%", or
// {value, units}. Returns pixels (relative values resolved against fontSize).
float resolveTextMetric(const json& v, float fontSize, float fallback) {
    if (v.is_number()) return v.get<float>();
    if (v.is_string()) {
        const std::string s = v.get<std::string>();
        char* end = nullptr;
        const float n = std::strtof(s.c_str(), &end);
        if (end == s.c_str()) return fallback;
        while (end && *end == ' ') ++end;
        if (end && *end == '%') return fontSize * n / 100.0f;
        return n;  // "36px" or bare number string
    }
    if (v.is_object() && v.contains("value") && v["value"].is_number()) {
        const float val = v["value"].get<float>();
        const std::string units = jstr(v, "units");
        if (units == "PERCENT") return fontSize * val / 100.0f;
        if (units == "RAW" || units == "FONT_SIZE") return fontSize * val;
        return val;
    }
    return fallback;
}

// fig2json rarely sets fontWeight — derive it from style/postscript names
// ("Semi Bold", "TitilliumWeb-Bold"). Port of weightFromName.
int weightFromName(const std::string& style, const std::string& postscript) {
    const std::string hay = toLower(style + " " + postscript);
    struct Entry { const char* needle; int weight; };
    // Order matters: "semibold"/"extrabold" must match before "bold".
    static const Entry table[] = {
        {"thin", 100},      {"hairline", 100},
        {"extralight", 200}, {"extra light", 200}, {"ultralight", 200},
        {"semibold", 600},  {"semi bold", 600},  {"demibold", 600}, {"demi bold", 600},
        {"extrabold", 800}, {"extra bold", 800}, {"ultrabold", 800},
        {"light", 300},     {"medium", 500},
        {"bold", 700},      {"black", 900},      {"heavy", 900},
    };
    for (const auto& e : table) {
        if (hay.find(e.needle) != std::string::npos) return e.weight;
    }
    return 0;
}

NodeType inferCanvasNodeType(const json& j, int depth) {
    const std::string explicitType = jstr(j, "type");
    // .fig stores Figma GROUPs as FRAME + resizeToFit (the frame hugs its
    // children). Groups never clip their content, so the distinction matters:
    // stroked curves wider than the hug box would otherwise be shaved off
    // (frameMaskDisabled defaults to clip-on for real frames).
    if (explicitType == "FRAME" && jbool(j, "resizeToFit", false))
        return NodeType::Group;
    if (!explicitType.empty()) {
        static const std::unordered_map<std::string, NodeType> map = {
            {"DOCUMENT", NodeType::Document},   {"CANVAS", NodeType::Canvas},
            {"FRAME", NodeType::Frame},         {"GROUP", NodeType::Group},
            {"SECTION", NodeType::Section},     {"RECTANGLE", NodeType::Rectangle},
            {"ELLIPSE", NodeType::Ellipse},     {"LINE", NodeType::Line},
            {"VECTOR", NodeType::Vector},       {"BOOLEAN_OPERATION", NodeType::BooleanOperation},
            {"STAR", NodeType::Star},           {"REGULAR_POLYGON", NodeType::RegularPolygon},
            {"TEXT", NodeType::Text},           {"COMPONENT", NodeType::Component},
            {"COMPONENT_SET", NodeType::ComponentSet}, {"INSTANCE", NodeType::Instance},
            {"SLICE", NodeType::Slice},
            // kiwi (.fig) names
            {"SYMBOL", NodeType::Component},    {"STATE_GROUP", NodeType::ComponentSet},
            {"ROUNDED_RECTANGLE", NodeType::Rectangle},
        };
        auto it = map.find(explicitType);
        if (it != map.end()) return it->second;
    }
    // Structural inference — port of inferNodeType in load.ts.
    if (depth == 0) return NodeType::Document;
    if (depth == 1 && j.contains("backgroundColor")) return NodeType::Canvas;
    if (j.contains("textData") || j.contains("fontSize")) return NodeType::Text;
    if (j.contains("arcData")) return NodeType::Ellipse;
    if (j.contains("vectorData") ||
        (j.contains("commands") && j["commands"].is_array() && !j["commands"].empty()))
        return NodeType::Vector;
    if (j.contains("booleanOperation") && j["booleanOperation"].is_string())
        return NodeType::BooleanOperation;
    if (j.contains("children") && j["children"].is_array() && !j["children"].empty())
        return NodeType::Frame;
    const std::string name = toLower(jstr(j, "name"));
    if (name.rfind("ellipse", 0) == 0) return NodeType::Ellipse;
    return NodeType::Rectangle;
}

float firstPositive(std::initializer_list<float> values, float fallback) {
    for (float v : values)
        if (v > 0) return v;
    return fallback;
}

Constraint parseKiwiConstraint(const std::string& v) {
    if (v == "CENTER") return Constraint::Center;
    if (v == "MAX") return Constraint::Max;
    if (v == "STRETCH") return Constraint::Stretch;
    if (v == "SCALE") return Constraint::Scale;
    return Constraint::Min;
}

AutoLayout::Align parseKiwiAlignItems(const std::string& v) {
    if (v == "CENTER") return AutoLayout::Align::Center;
    if (v == "MAX") return AutoLayout::Align::Max;
    // Figma's UI exposes only SPACE_BETWEEN; kiwi also has SPACE_EVENLY.
    if (v == "SPACE_BETWEEN" || v == "SPACE_EVENLY") return AutoLayout::Align::SpaceBetween;
    if (v == "BASELINE") return AutoLayout::Align::Baseline;
    return AutoLayout::Align::Min;
}

// Layout fields from kiwi names (requires a fig2json build that keeps the
// constraint/stack properties instead of stripping them).
void parseKiwiLayout(const json& j, Node& node) {
    node.constraintH = parseKiwiConstraint(jstr(j, "horizontalConstraint", "MIN"));
    node.constraintV = parseKiwiConstraint(jstr(j, "verticalConstraint", "MIN"));

    const std::string mode = jstr(j, "stackMode", "NONE");
    if (mode == "HORIZONTAL" || mode == "VERTICAL") {
        AutoLayout& al = node.autoLayout;
        al.mode = mode == "HORIZONTAL" ? AutoLayout::Mode::Horizontal
                                       : AutoLayout::Mode::Vertical;
        // RESIZE_TO_FIT / RESIZE_TO_FIT_WITH_IMPLICIT_SIZE = hug content.
        al.primarySizing = jstr(j, "stackPrimarySizing", "FIXED") == "FIXED"
                               ? AutoLayout::Sizing::Fixed
                               : AutoLayout::Sizing::Hug;
        al.counterSizing = jstr(j, "stackCounterSizing", "FIXED") == "FIXED"
                               ? AutoLayout::Sizing::Fixed
                               : AutoLayout::Sizing::Hug;
        al.primaryAlign = parseKiwiAlignItems(jstr(j, "stackPrimaryAlignItems", "MIN"));
        al.counterAlign = parseKiwiAlignItems(jstr(j, "stackCounterAlignItems", "MIN"));
        // stackHorizontalPadding/stackVerticalPadding are the LEFT/TOP values;
        // right/bottom fall back to them when absent (symmetric padding).
        al.paddingLeft = jfloat(j, "stackHorizontalPadding");
        al.paddingTop = jfloat(j, "stackVerticalPadding");
        al.paddingRight = jfloat(j, "stackPaddingRight", al.paddingLeft);
        al.paddingBottom = jfloat(j, "stackPaddingBottom", al.paddingTop);
        al.itemSpacing = jfloat(j, "stackSpacing");
        al.counterSpacing = jfloat(j, "stackCounterSpacing");
        al.wrap = jstr(j, "stackWrap", "NO_WRAP") == "WRAP";
    }

    node.layoutGrow = jfloat(j, "stackChildPrimaryGrow", 0);
    node.layoutAlignStretch = jstr(j, "stackChildAlignSelf") == "STRETCH";
    node.layoutAbsolute = jstr(j, "stackPositioning") == "ABSOLUTE";

    // Min/max sizes arrive either as plain numbers or {"value": n} wrappers.
    auto sizeLimit = [&](const char* key) -> float {
        auto it = j.find(key);
        if (it == j.end()) return 0;
        if (it->is_number()) return it->get<float>();
        if (it->is_object()) return jfloat(*it, "value");
        return 0;
    };
    node.minWidth = sizeLimit("minWidth");
    node.maxWidth = sizeLimit("maxWidth");
    node.minHeight = sizeLimit("minHeight");
    node.maxHeight = sizeLimit("maxHeight");
}

std::unique_ptr<Node> parseCanvasNode(const json& j, Node* parent, int depth) {
    auto node = std::make_unique<Node>();
    node->parent = parent;
    node->id = j.contains("guid") ? symbolIDKey(j["guid"]) : std::string();
    node->name = jstr(j, "name");
    node->type = inferCanvasNodeType(j, depth);
    node->visible = jbool(j, "visible", true);
    node->opacity = jfloat(j, "opacity", 1.0f);
    node->relativeTransform = transformOf(j);

    if (j.contains("size") && j["size"].is_object()) {
        node->width = jfloat(j["size"], "x");
        node->height = jfloat(j["size"], "y");
    }

    // .fig clipping flag: `frameMaskDisabled == false` means clipping ON;
    // missing means "don't clip" (wrapper canvas/page nodes shouldn't clip).
    // Groups (FRAME + resizeToFit) never clip regardless of the flag.
    if (node->type != NodeType::Group && j.contains("frameMaskDisabled") &&
        j["frameMaskDisabled"].is_boolean()) {
        node->clipsContent = !j["frameMaskDisabled"].get<bool>();
    }

    parseKiwiLayout(j, *node);

    // Instance → master component link ("sessionID:localID", same key format
    // as node ids and the components map).
    if (auto sd = j.find("symbolData"); sd != j.end() && sd->is_object()) {
        if (auto sid = sd->find("symbolID"); sid != sd->end() && sid->is_object()) {
            node->componentId = symbolIDKey(*sid);
            if (node->type == NodeType::Frame) node->type = NodeType::Instance;
        }
    }

    parseCanvasPaints(j, "fillPaints", node->fills);
    parseCanvasPaints(j, "strokePaints", node->strokes);
    node->strokeWeight = jfloat(j, "strokeWeight", 1.0f);
    const std::string sa = jstr(j, "strokeAlign", "INSIDE");
    node->strokeAlign = sa == "OUTSIDE" ? StrokeAlign::Outside
                      : sa == "CENTER"  ? StrokeAlign::Center
                                        : StrokeAlign::Inside;
    if (j.contains("dashPattern") && j["dashPattern"].is_array()) {
        for (const json& d : j["dashPattern"])
            if (d.is_number()) node->strokeDashes.push_back(d.get<float>());
    }
    node->strokeCap = jstr(j, "strokeCap", "NONE");
    node->strokeJoin = jstr(j, "strokeJoin", "MITER");

    // Corner radii: several spellings across fig2json schema versions.
    node->cornerRadius = std::fmax(0.0f, jfloat(j, "cornerRadius"));
    const float tl = firstPositive({jfloat(j, "rectangleTopLeftCornerRadius"),
                                    jfloat(j, "topLeftRadius")}, node->cornerRadius);
    const float tr = firstPositive({jfloat(j, "rectangleTopRightCornerRadius"),
                                    jfloat(j, "topRightRadius")}, node->cornerRadius);
    const float br = firstPositive({jfloat(j, "rectangleBottomRightCornerRadius"),
                                    jfloat(j, "bottomRightRadius")}, node->cornerRadius);
    const float bl = firstPositive({jfloat(j, "rectangleBottomLeftCornerRadius"),
                                    jfloat(j, "bottomLeftRadius")}, node->cornerRadius);
    if (tl > 0 || tr > 0 || br > 0 || bl > 0)
        node->rectangleCornerRadii = {{tl, tr, br, bl}};

    // Geometry: explicit commands win; then vectorNetwork scaled from
    // normalizedSize into node-local pixels.
    bool evenOdd = false;
    if (j.contains("fillGeometry") && j["fillGeometry"].is_array() &&
        !j["fillGeometry"].empty()) {
        // Some nodes carry REST-style geometry arrays of {commands, windingRule}.
        for (const json& g : j["fillGeometry"]) {
            if (!g.is_object()) continue;
            std::string path = commandsToSvgPath(g.value("commands", json()));
            if (path.empty()) continue;
            PathGeometry pg;
            pg.path = std::move(path);
            const std::string wr = jstr(g, "windingRule", "NONZERO");
            pg.evenOdd = wr == "ODD" || wr == "EVENODD";
            node->fillGeometry.push_back(std::move(pg));
        }
    } else if (j.contains("commands") && j["commands"].is_array() && !j["commands"].empty()) {
        std::string path = commandsToSvgPath(j["commands"]);
        if (!path.empty()) node->fillGeometry.push_back({std::move(path), evenOdd});
    } else if (j.contains("vectorData") && j["vectorData"].is_object()) {
        const json& vd = j["vectorData"];
        if (vd.contains("vectorNetwork") && vd["vectorNetwork"].is_object()) {
            const float nx = vd.contains("normalizedSize") ? jfloat(vd["normalizedSize"], "x")
                                                           : node->width;
            const float ny = vd.contains("normalizedSize") ? jfloat(vd["normalizedSize"], "y")
                                                           : node->height;
            const float sx = nx > 0 ? node->width / nx : 1.0f;
            const float sy = ny > 0 ? node->height / ny : 1.0f;
            std::string path = vectorNetworkToSvgPath(vd["vectorNetwork"], sx, sy);
            if (!path.empty()) node->fillGeometry.push_back({std::move(path), false});
        }
    }
    if (j.contains("strokeGeometry") && j["strokeGeometry"].is_array()) {
        for (const json& g : j["strokeGeometry"]) {
            if (!g.is_object()) continue;
            std::string path = commandsToSvgPath(g.value("commands", json()));
            if (path.empty()) continue;
            PathGeometry pg;
            pg.path = std::move(path);
            const std::string wr = jstr(g, "windingRule", "NONZERO");
            pg.evenOdd = wr == "ODD" || wr == "EVENODD";
            node->strokeGeometry.push_back(std::move(pg));
        }
    }

    parseCanvasEffects(j, node->effects);

    if (node->type == NodeType::Text) {
        if (j.contains("textData") && j["textData"].is_object()) {
            const json& td = j["textData"];
            node->characters = jstr(td, "characters", jstr(td, "content"));
        }
        if (node->characters.empty()) node->characters = jstr(j, "content");

        TextStyle& ts = node->textStyle;
        std::string fontStyle, postscript;
        if (j.contains("fontName") && j["fontName"].is_object()) {
            ts.fontFamily = jstr(j["fontName"], "family", ts.fontFamily);
            fontStyle = jstr(j["fontName"], "style");
            postscript = jstr(j["fontName"], "postscript");
            ts.fontPostScriptName = postscript;
        }
        ts.fontSize = jfloat(j, "fontSize", ts.fontSize);
        if (j.contains("fontWeight") && j["fontWeight"].is_number()) {
            ts.fontWeight = static_cast<int>(j["fontWeight"].get<float>());
        } else {
            const int w = weightFromName(fontStyle, postscript);
            ts.fontWeight = w > 0 ? w : 400;
        }
        ts.italic = toLower(fontStyle).find("italic") != std::string::npos ||
                    toLower(postscript).find("italic") != std::string::npos;
        if (j.contains("lineHeight"))
            ts.lineHeightPx = resolveTextMetric(j["lineHeight"], ts.fontSize, 0);
        if (j.contains("letterSpacing"))
            ts.letterSpacing = resolveTextMetric(j["letterSpacing"], ts.fontSize, 0);
        const std::string ah = jstr(j, "textAlignHorizontal", "LEFT");
        if (ah == "CENTER") ts.alignH = TextStyle::AlignH::Center;
        else if (ah == "RIGHT") ts.alignH = TextStyle::AlignH::Right;
        else if (ah == "JUSTIFIED") ts.alignH = TextStyle::AlignH::Justified;
        const std::string av = jstr(j, "textAlignVertical", "TOP");
        if (av == "CENTER") ts.alignV = TextStyle::AlignV::Center;
        else if (av == "BOTTOM") ts.alignV = TextStyle::AlignV::Bottom;
        ts.autoResize = jstr(j, "textAutoResize", "NONE");
        ts.truncateEnding = jstr(j, "textTruncation") == "ENDING";
        ts.maxLines = static_cast<int>(jfloat(j, "maxLines", 0));

        // Rich-text runs: characterStyleIDs index styleOverrideTable entries
        // (ID 0 = base style). Indices are per UTF-16 code unit; walk the
        // UTF-8 string codepoint by codepoint to map them onto byte ranges.
        if (j.contains("textData") && j["textData"].is_object()) {
            const json& td = j["textData"];
            if (td.contains("characterStyleIDs") &&
                td["characterStyleIDs"].is_array() &&
                td.contains("styleOverrideTable") && td["styleOverrideTable"].is_array()) {
                std::unordered_map<int, const json*> table;
                for (const json& ov : td["styleOverrideTable"]) {
                    if (ov.is_object() && ov.contains("styleID") && ov["styleID"].is_number())
                        table[ov["styleID"].get<int>()] = &ov;
                }
                const json& ids = td["characterStyleIDs"];
                auto idAt = [&](size_t i) -> int {
                    return i < ids.size() && ids[i].is_number() ? ids[i].get<int>() : 0;
                };
                // Byte offset of each codepoint plus its style id (taken from
                // the UTF-16 position of the codepoint's first code unit).
                const std::string& s = node->characters;
                std::vector<std::pair<size_t, int>> cps;  // (byte offset, style id)
                size_t byte = 0, u16 = 0;
                while (byte < s.size()) {
                    const unsigned char c = static_cast<unsigned char>(s[byte]);
                    const size_t n = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : 4;
                    cps.emplace_back(byte, idAt(u16));
                    byte += n;
                    u16 += n == 4 ? 2 : 1;  // astral codepoints take a surrogate pair
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
                    run.style = ts;
                    const json* ov = id != 0 && table.count(id) ? table[id] : nullptr;
                    if (ov) {
                        const json& o = *ov;
                        std::string fStyle, fPost;
                        if (o.contains("fontName") && o["fontName"].is_object()) {
                            run.style.fontFamily =
                                jstr(o["fontName"], "family", run.style.fontFamily);
                            fStyle = jstr(o["fontName"], "style");
                            fPost = jstr(o["fontName"], "postscript");
                        }
                        run.style.fontSize = jfloat(o, "fontSize", run.style.fontSize);
                        if (o.contains("fontWeight") && o["fontWeight"].is_number())
                            run.style.fontWeight = static_cast<int>(o["fontWeight"].get<float>());
                        else if (int w = weightFromName(fStyle, fPost); w > 0)
                            run.style.fontWeight = w;
                        if (!fStyle.empty() || !fPost.empty()) {
                            run.style.italic =
                                toLower(fStyle).find("italic") != std::string::npos ||
                                toLower(fPost).find("italic") != std::string::npos;
                        }
                        if (o.contains("fillPaints") && o["fillPaints"].is_array()) {
                            for (const json& p : o["fillPaints"]) {
                                if (!p.is_object()) continue;
                                Paint paint = parseCanvasPaint(p);
                                if (paint.visible && paint.type == PaintType::Solid) {
                                    Color c = paint.color;
                                    c.a *= paint.opacity;
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
                if (anyOverride && runs.size() > 1) node->textRuns = std::move(runs);
            }
        }
    }

    // CANVAS pages: backgroundColor is a hex string here.
    if (node->type == NodeType::Canvas && node->fills.empty() &&
        j.contains("backgroundColor") && j["backgroundColor"].is_string()) {
        Paint bg;
        bg.type = PaintType::Solid;
        bg.color = parseHexColor(j["backgroundColor"].get<std::string>());
        node->fills.push_back(bg);
    }

    // Boolean ops with a precomputed outline (vectorData/commands): the
    // outline IS the final visual — parsing the source children too would
    // paint them again on top of the combined shape.
    const bool booleanWithOutline =
        j.contains("booleanOperation") && !node->fillGeometry.empty();
    if (!booleanWithOutline && j.contains("children") && j["children"].is_array()) {
        for (const json& c : j["children"]) {
            if (c.is_object()) node->children.push_back(parseCanvasNode(c, node.get(), depth + 1));
        }
    }
    return node;
}

}  // namespace

std::unique_ptr<Document> parseCanvasDocument(const std::string& jsonText) {
    json root = json::parse(jsonText);
    auto docIt = root.find("document");
    if (docIt == root.end() || !docIt->is_object())
        throw std::runtime_error("figmalib: canvas.json has no \"document\" field");

    // Lookups for hydration.
    std::unordered_map<std::string, const json*> components;
    if (root.contains("components") && root["components"].is_array()) {
        for (const json& c : root["components"]) {
            const std::string key = jstr(c, "componentKey");
            if (!key.empty()) components[key] = &c;
        }
    }
    std::unordered_map<std::string, const json*> styles;
    if (root.contains("styles") && root["styles"].is_object()) {
        for (auto it = root["styles"].begin(); it != root["styles"].end(); ++it)
            styles[it.key()] = &it.value();
    }

    json& doc = *docIt;
    hydrateRecursive(doc, components, styles);
    resolveSharedStyles(doc, styles);

    auto out = std::make_unique<Document>();
    out->name = jstr(root, "name", jstr(doc, "name", "Untitled"));
    out->root = parseCanvasNode(doc, nullptr, 0);
    out->captureBaseLayout();
    return out;
}

}  // namespace figmalib
