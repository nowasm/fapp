#include "figmalib/document.h"

#include <algorithm>

namespace figmalib {

void Node::contentExtent(float& w, float& h) const {
    w = 0;
    h = 0;
    for (const auto& c : children) {
        if (!c->effectivelyVisible() || c->scrollFixed) continue;
        // Children can be rotated: take the max over all four corners.
        const float xs[2] = {0, c->width}, ys[2] = {0, c->height};
        for (float cx : xs) {
            for (float cy : ys) {
                float px, py;
                c->relativeTransform.apply(cx, cy, px, py);
                w = std::max(w, px);
                h = std::max(h, py);
            }
        }
    }
}

float Node::maxScrollX() const {
    if (scrollDirection != ScrollDirection::Horizontal &&
        scrollDirection != ScrollDirection::Both) {
        return 0;
    }
    float w, h;
    contentExtent(w, h);
    return std::max(0.0f, w - width);
}

float Node::maxScrollY() const {
    if (scrollDirection != ScrollDirection::Vertical &&
        scrollDirection != ScrollDirection::Both) {
        return 0;
    }
    float w, h;
    contentExtent(w, h);
    return std::max(0.0f, h - height);
}

Node* Node::findById(const std::string& nodeId) {
    if (id == nodeId) return this;
    for (auto& c : children) {
        if (Node* n = c->findById(nodeId)) return n;
    }
    return nullptr;
}

Node* Node::findByName(const std::string& nodeName) {
    if (name == nodeName) return this;
    for (auto& c : children) {
        if (Node* n = c->findByName(nodeName)) return n;
    }
    return nullptr;
}

void Node::visit(const std::function<bool(Node&)>& fn) {
    if (!fn(*this)) return;
    for (auto& c : children) c->visit(fn);
}

std::vector<Node*> Document::topLevelFrames() const {
    std::vector<Node*> frames;
    if (!root) return frames;
    auto frameLike = [](NodeType t) {
        return t == NodeType::Frame || t == NodeType::Component || t == NodeType::Instance ||
               t == NodeType::Section;
    };
    for (auto& page : root->children) {
        if (page->type == NodeType::Canvas) {
            for (auto& child : page->children) {
                if (frameLike(child->type)) frames.push_back(child.get());
            }
        } else if (frameLike(page->type) && page->width > 0 && page->height > 0) {
            // canvas.json pages can miss the CANVAS inference; accept direct frames.
            frames.push_back(page.get());
        }
    }
    // A bare node tree (no DOCUMENT/CANVAS wrapper): treat the root as a frame.
    if (frames.empty() && root->type != NodeType::Document) frames.push_back(root.get());
    return frames;
}

std::unique_ptr<Node> cloneNode(const Node& src, Node* parent) {
    auto n = std::make_unique<Node>();
    static_cast<NodeData&>(*n) = src;  // member-wise copy of all value state
    n->parent = parent;
    n->children.reserve(src.children.size());
    for (const auto& c : src.children) n->children.push_back(cloneNode(*c, n.get()));
    return n;
}

void setNodeText(Node& n, const std::string& text) {
    n.characters = text;
    // Replace stale rich-text runs (they index the old string) with a single
    // base-style run over the new text. Keeping a run (rather than none)
    // routes rendering through the per-token path, whose font fallback works
    // glyph-by-glyph — mixed Latin/CJK runtime strings need that.
    n.textRuns.clear();
    if (!text.empty()) {
        TextRun run;
        run.start = 0;
        run.end = static_cast<int>(text.size());
        run.style = n.textStyle;
        n.textRuns.push_back(run);
    }
}

void Document::captureBaseLayout() {
    if (!root) return;
    root->visit([](Node& n) {
        n.baseTransform = n.relativeTransform;
        n.baseWidth = n.width;
        n.baseHeight = n.height;
        return true;
    });
}

}  // namespace figmalib
