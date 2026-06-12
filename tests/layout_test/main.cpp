// Layout engine self-test: hand-written REST JSON documents are reflowed by
// layoutFrame() and the resulting positions/sizes are checked numerically.
// Returns non-zero on the first failure (CI-friendly).

#include <cmath>
#include <cstdio>
#include <string>

#include <figmalib/figmalib.h>

static int g_failures = 0;

#define CHECK_NEAR(label, actual, expected)                                       \
    do {                                                                          \
        const float a_ = (actual), e_ = (expected);                               \
        if (std::fabs(a_ - e_) > 0.01f) {                                         \
            std::printf("FAIL %s: %s = %.2f, expected %.2f\n", __func__, label,   \
                        a_, e_);                                                  \
            ++g_failures;                                                         \
        }                                                                         \
    } while (0)

static figmalib::Node* child(figmalib::Node& parent, const char* name) {
    figmalib::Node* n = parent.findByName(name);
    if (!n) {
        std::printf("FAIL: node \"%s\" not found\n", name);
        ++g_failures;
    }
    return n;
}

// One 400×300 frame; every constraint mode on one child each. The frame
// doubles to 800×600.
static void testConstraints() {
    const char* json = R"({
      "name": "t",
      "document": {
        "id": "0:0", "type": "DOCUMENT", "children": [{
          "id": "0:1", "type": "CANVAS", "children": [{
            "id": "1:1", "name": "root", "type": "FRAME",
            "size": {"x": 400, "y": 300},
            "relativeTransform": [[1,0,0],[0,1,0]],
            "children": [
              {"id": "2:1", "name": "min", "type": "RECTANGLE",
               "size": {"x": 50, "y": 40},
               "relativeTransform": [[1,0,10],[0,1,20]],
               "constraints": {"horizontal": "LEFT", "vertical": "TOP"}},
              {"id": "2:2", "name": "max", "type": "RECTANGLE",
               "size": {"x": 50, "y": 40},
               "relativeTransform": [[1,0,340],[0,1,240]],
               "constraints": {"horizontal": "RIGHT", "vertical": "BOTTOM"}},
              {"id": "2:3", "name": "center", "type": "RECTANGLE",
               "size": {"x": 50, "y": 40},
               "relativeTransform": [[1,0,175],[0,1,130]],
               "constraints": {"horizontal": "CENTER", "vertical": "CENTER"}},
              {"id": "2:4", "name": "stretch", "type": "RECTANGLE",
               "size": {"x": 380, "y": 40},
               "relativeTransform": [[1,0,10],[0,1,130]],
               "constraints": {"horizontal": "LEFT_RIGHT", "vertical": "TOP_BOTTOM"}},
              {"id": "2:5", "name": "scale", "type": "RECTANGLE",
               "size": {"x": 100, "y": 60},
               "relativeTransform": [[1,0,40],[0,1,30]],
               "constraints": {"horizontal": "SCALE", "vertical": "SCALE"}}
            ]
          }]
        }]
      }
    })";
    auto doc = figmalib::parseDocument(json);
    figmalib::Node* root = doc->findByName("root");
    figmalib::layoutFrame(*root, 800, 600);

    figmalib::Node* n = child(*root, "min");
    CHECK_NEAR("x", n->relativeTransform.m02, 10);
    CHECK_NEAR("w", n->width, 50);

    n = child(*root, "max");  // right/bottom margins stay 10/20
    CHECK_NEAR("x", n->relativeTransform.m02, 740);
    CHECK_NEAR("y", n->relativeTransform.m12, 540);

    n = child(*root, "center");  // center offset preserved (was exact center)
    CHECK_NEAR("x", n->relativeTransform.m02, 375);
    CHECK_NEAR("y", n->relativeTransform.m12, 280);

    n = child(*root, "stretch");  // both margins pinned at 10
    CHECK_NEAR("x", n->relativeTransform.m02, 10);
    CHECK_NEAR("w", n->width, 780);
    CHECK_NEAR("h", n->height, 340);

    n = child(*root, "scale");  // ×2 in both axes
    CHECK_NEAR("x", n->relativeTransform.m02, 80);
    CHECK_NEAR("y", n->relativeTransform.m12, 60);
    CHECK_NEAR("w", n->width, 200);
    CHECK_NEAR("h", n->height, 120);

    // Reset restores authored geometry exactly.
    figmalib::resetLayout(*root);
    CHECK_NEAR("reset w", root->width, 400);
    CHECK_NEAR("reset child x", child(*root, "max")->relativeTransform.m02, 340);
}

// Vertical auto-layout: padding 10, spacing 8, fixed header, growing body,
// stretch-width children. Frame grows 200×300 → 400×500.
static void testAutoLayoutColumn() {
    const char* json = R"({
      "name": "t",
      "document": {
        "id": "0:0", "type": "DOCUMENT", "children": [{
          "id": "0:1", "type": "CANVAS", "children": [{
            "id": "1:1", "name": "root", "type": "FRAME",
            "size": {"x": 200, "y": 300},
            "relativeTransform": [[1,0,0],[0,1,0]],
            "layoutMode": "VERTICAL",
            "primaryAxisSizingMode": "FIXED", "counterAxisSizingMode": "FIXED",
            "paddingLeft": 10, "paddingRight": 10, "paddingTop": 10, "paddingBottom": 10,
            "itemSpacing": 8,
            "children": [
              {"id": "2:1", "name": "header", "type": "RECTANGLE",
               "size": {"x": 180, "y": 40},
               "relativeTransform": [[1,0,10],[0,1,10]],
               "layoutAlign": "STRETCH"},
              {"id": "2:2", "name": "body", "type": "RECTANGLE",
               "size": {"x": 180, "y": 184},
               "relativeTransform": [[1,0,10],[0,1,58]],
               "layoutAlign": "STRETCH", "layoutGrow": 1},
              {"id": "2:3", "name": "footer", "type": "RECTANGLE",
               "size": {"x": 100, "y": 30},
               "relativeTransform": [[1,0,10],[0,1,250]]}
            ]
          }]
        }]
      }
    })";
    auto doc = figmalib::parseDocument(json);
    figmalib::Node* root = doc->findByName("root");
    figmalib::layoutFrame(*root, 400, 500);

    figmalib::Node* n = child(*root, "header");
    CHECK_NEAR("x", n->relativeTransform.m02, 10);
    CHECK_NEAR("y", n->relativeTransform.m12, 10);
    CHECK_NEAR("w", n->width, 380);  // stretched to inner width
    CHECK_NEAR("h", n->height, 40);

    n = child(*root, "body");  // absorbs all extra height: 480-40-30-2*8 = 394
    CHECK_NEAR("y", n->relativeTransform.m12, 58);
    CHECK_NEAR("h", n->height, 394);
    CHECK_NEAR("w", n->width, 380);

    n = child(*root, "footer");  // packed after body, keeps own width
    CHECK_NEAR("y", n->relativeTransform.m12, 460);
    CHECK_NEAR("w", n->width, 100);
}

// Horizontal bar with SPACE_BETWEEN and centered cross axis.
static void testSpaceBetween() {
    const char* json = R"({
      "name": "t",
      "document": {
        "id": "0:0", "type": "DOCUMENT", "children": [{
          "id": "0:1", "type": "CANVAS", "children": [{
            "id": "1:1", "name": "root", "type": "FRAME",
            "size": {"x": 320, "y": 60},
            "relativeTransform": [[1,0,0],[0,1,0]],
            "layoutMode": "HORIZONTAL",
            "primaryAxisSizingMode": "FIXED", "counterAxisSizingMode": "FIXED",
            "primaryAxisAlignItems": "SPACE_BETWEEN",
            "counterAxisAlignItems": "CENTER",
            "paddingLeft": 10, "paddingRight": 10,
            "children": [
              {"id": "2:1", "name": "a", "type": "RECTANGLE",
               "size": {"x": 60, "y": 20},
               "relativeTransform": [[1,0,10],[0,1,20]]},
              {"id": "2:2", "name": "b", "type": "RECTANGLE",
               "size": {"x": 60, "y": 40},
               "relativeTransform": [[1,0,130],[0,1,10]]},
              {"id": "2:3", "name": "c", "type": "RECTANGLE",
               "size": {"x": 60, "y": 20},
               "relativeTransform": [[1,0,250],[0,1,20]]}
            ]
          }]
        }]
      }
    })";
    auto doc = figmalib::parseDocument(json);
    figmalib::Node* root = doc->findByName("root");
    figmalib::layoutFrame(*root, 620, 60);

    // inner = 600; gaps = (600 - 180) / 2 = 210
    figmalib::Node* n = child(*root, "a");
    CHECK_NEAR("x", n->relativeTransform.m02, 10);
    CHECK_NEAR("y", n->relativeTransform.m12, 20);  // centered in 60-high frame
    n = child(*root, "b");
    CHECK_NEAR("x", n->relativeTransform.m02, 280);
    n = child(*root, "c");
    CHECK_NEAR("x", n->relativeTransform.m02, 550);
}

// Hug-height container of a fixed list: the parent frame stretches a child
// column whose own height hugs its content (stays authored), while an
// absolutely positioned badge follows its constraints inside the stack.
static void testHugAndAbsolute() {
    const char* json = R"({
      "name": "t",
      "document": {
        "id": "0:0", "type": "DOCUMENT", "children": [{
          "id": "0:1", "type": "CANVAS", "children": [{
            "id": "1:1", "name": "root", "type": "FRAME",
            "size": {"x": 200, "y": 200},
            "relativeTransform": [[1,0,0],[0,1,0]],
            "layoutMode": "VERTICAL",
            "primaryAxisSizingMode": "FIXED", "counterAxisSizingMode": "FIXED",
            "children": [
              {"id": "2:1", "name": "card", "type": "FRAME",
               "size": {"x": 200, "y": 100},
               "relativeTransform": [[1,0,0],[0,1,0]],
               "layoutMode": "VERTICAL",
               "primaryAxisSizingMode": "AUTO", "counterAxisSizingMode": "FIXED",
               "layoutAlign": "STRETCH",
               "itemSpacing": 10,
               "children": [
                 {"id": "3:1", "name": "row1", "type": "RECTANGLE",
                  "size": {"x": 200, "y": 45},
                  "relativeTransform": [[1,0,0],[0,1,0]],
                  "layoutAlign": "STRETCH"},
                 {"id": "3:2", "name": "row2", "type": "RECTANGLE",
                  "size": {"x": 200, "y": 45},
                  "relativeTransform": [[1,0,0],[0,1,55]],
                  "layoutAlign": "STRETCH"},
                 {"id": "3:3", "name": "badge", "type": "RECTANGLE",
                  "size": {"x": 20, "y": 20},
                  "relativeTransform": [[1,0,170],[0,1,10]],
                  "layoutPositioning": "ABSOLUTE",
                  "constraints": {"horizontal": "RIGHT", "vertical": "TOP"}}
               ]}
            ]
          }]
        }]
      }
    })";
    auto doc = figmalib::parseDocument(json);
    figmalib::Node* root = doc->findByName("root");
    figmalib::layoutFrame(*root, 320, 200);

    figmalib::Node* card = child(*root, "card");
    CHECK_NEAR("card w", card->width, 320);   // stretched across the root
    CHECK_NEAR("card h", card->height, 100);  // hug: 45+10+45

    CHECK_NEAR("row1 w", child(*card, "row1")->width, 320);
    CHECK_NEAR("row2 y", child(*card, "row2")->relativeTransform.m12, 55);

    figmalib::Node* badge = child(*card, "badge");  // right margin stays 10
    CHECK_NEAR("badge x", badge->relativeTransform.m02, 290);
    CHECK_NEAR("badge y", badge->relativeTransform.m12, 10);
}

// Wrapping horizontal stack: 3 fixed 100×40 chips with spacing 10 in a frame
// narrowed to fit two per line; row gap from counterAxisSpacing.
static void testWrap() {
    const char* json = R"({
      "name": "t",
      "document": {
        "id": "0:0", "type": "DOCUMENT", "children": [{
          "id": "0:1", "type": "CANVAS", "children": [{
            "id": "1:1", "name": "root", "type": "FRAME",
            "size": {"x": 400, "y": 100},
            "relativeTransform": [[1,0,0],[0,1,0]],
            "layoutMode": "HORIZONTAL", "layoutWrap": "WRAP",
            "primaryAxisSizingMode": "FIXED", "counterAxisSizingMode": "FIXED",
            "itemSpacing": 10, "counterAxisSpacing": 6,
            "children": [
              {"id": "2:1", "name": "a", "type": "RECTANGLE",
               "size": {"x": 100, "y": 40}, "relativeTransform": [[1,0,0],[0,1,0]]},
              {"id": "2:2", "name": "b", "type": "RECTANGLE",
               "size": {"x": 100, "y": 40}, "relativeTransform": [[1,0,110],[0,1,0]]},
              {"id": "2:3", "name": "c", "type": "RECTANGLE",
               "size": {"x": 100, "y": 40}, "relativeTransform": [[1,0,220],[0,1,0]]}
            ]
          }]
        }]
      }
    })";
    auto doc = figmalib::parseDocument(json);
    figmalib::Node* root = doc->findByName("root");
    figmalib::layoutFrame(*root, 230, 100);  // fits two chips (100+10+100)

    figmalib::Node* n = child(*root, "a");
    CHECK_NEAR("x", n->relativeTransform.m02, 0);
    CHECK_NEAR("y", n->relativeTransform.m12, 0);
    n = child(*root, "b");
    CHECK_NEAR("x", n->relativeTransform.m02, 110);
    CHECK_NEAR("y", n->relativeTransform.m12, 0);
    n = child(*root, "c");  // wrapped onto the second row: 40 + 6 gap
    CHECK_NEAR("x", n->relativeTransform.m02, 0);
    CHECK_NEAR("y", n->relativeTransform.m12, 46);
}

// Min/max limits cap both constraint stretching and auto-layout grow.
static void testMinMax() {
    const char* json = R"({
      "name": "t",
      "document": {
        "id": "0:0", "type": "DOCUMENT", "children": [{
          "id": "0:1", "type": "CANVAS", "children": [{
            "id": "1:1", "name": "root", "type": "FRAME",
            "size": {"x": 200, "y": 100},
            "relativeTransform": [[1,0,0],[0,1,0]],
            "children": [
              {"id": "2:1", "name": "capped", "type": "RECTANGLE",
               "size": {"x": 180, "y": 80},
               "relativeTransform": [[1,0,10],[0,1,10]],
               "maxWidth": 300, "minHeight": 60,
               "constraints": {"horizontal": "LEFT_RIGHT", "vertical": "TOP_BOTTOM"}}
            ]
          }]
        }]
      }
    })";
    auto doc = figmalib::parseDocument(json);
    figmalib::Node* root = doc->findByName("root");

    figmalib::layoutFrame(*root, 600, 100);  // stretch would hit 580 → capped 300
    CHECK_NEAR("maxW", child(*root, "capped")->width, 300);

    figmalib::layoutFrame(*root, 200, 40);  // stretch would hit 20 → floored 60
    CHECK_NEAR("minH", child(*root, "capped")->height, 60);
}

// Variant switching: a component set with Default/Hover states, an instance
// wider than the master. Switching must swap the subtree, remap geometry to
// the instance size, and be reversible.
static void testVariants() {
    const char* json = R"({
      "name": "t",
      "document": {
        "id": "0:0", "type": "DOCUMENT", "children": [{
          "id": "0:1", "type": "CANVAS", "children": [
            {"id": "5:1", "name": "Button", "type": "COMPONENT_SET",
             "size": {"x": 100, "y": 100},
             "relativeTransform": [[1,0,500],[0,1,0]],
             "children": [
               {"id": "5:2", "name": "State=Default", "type": "COMPONENT",
                "size": {"x": 100, "y": 40},
                "relativeTransform": [[1,0,0],[0,1,0]],
                "children": [
                  {"id": "5:3", "name": "bg-default", "type": "RECTANGLE",
                   "size": {"x": 90, "y": 30},
                   "relativeTransform": [[1,0,5],[0,1,5]],
                   "constraints": {"horizontal": "LEFT_RIGHT", "vertical": "TOP_BOTTOM"},
                   "fills": [{"type": "SOLID", "color": {"r": 1, "g": 0, "b": 0, "a": 1}}]}
                ]},
               {"id": "5:4", "name": "State=Hover", "type": "COMPONENT",
                "size": {"x": 100, "y": 40},
                "relativeTransform": [[1,0,0],[0,1,50]],
                "children": [
                  {"id": "5:5", "name": "bg-hover", "type": "RECTANGLE",
                   "size": {"x": 90, "y": 30},
                   "relativeTransform": [[1,0,5],[0,1,5]],
                   "constraints": {"horizontal": "LEFT_RIGHT", "vertical": "TOP_BOTTOM"},
                   "fills": [{"type": "SOLID", "color": {"r": 0, "g": 1, "b": 0, "a": 1}}]}
                ]}
             ]},
            {"id": "1:1", "name": "root", "type": "FRAME",
             "size": {"x": 400, "y": 300},
             "relativeTransform": [[1,0,0],[0,1,0]],
             "children": [
               {"id": "2:1", "name": "btn", "type": "INSTANCE", "componentId": "5:2",
                "size": {"x": 150, "y": 40},
                "relativeTransform": [[1,0,20],[0,1,20]],
                "children": [
                  {"id": "2:2", "name": "bg-default", "type": "RECTANGLE",
                   "size": {"x": 140, "y": 30},
                   "relativeTransform": [[1,0,5],[0,1,5]],
                   "constraints": {"horizontal": "LEFT_RIGHT", "vertical": "TOP_BOTTOM"},
                   "fills": [{"type": "SOLID", "color": {"r": 1, "g": 0, "b": 0, "a": 1}}]}
                ]}
             ]}
          ]
        }]
      }
    })";
    auto ui = figmalib::FigmaUI::fromJson(json);

    if (!ui->setVariant("btn", "State", "Hover")) {
        std::printf("FAIL %s: setVariant(Hover) returned false\n", __func__);
        ++g_failures;
        return;
    }
    figmalib::Node* btn = ui->document().findByName("btn");
    CHECK_NEAR("componentId", btn->componentId == "5:4" ? 1.f : 0.f, 1.f);
    figmalib::Node* bg = btn->findByName("bg-hover");
    if (!bg) {
        std::printf("FAIL %s: hover background missing after switch\n", __func__);
        ++g_failures;
        return;
    }
    CHECK_NEAR("fill g", bg->fills.empty() ? -1.f : bg->fills[0].color.g, 1.f);
    // Stretch constraint remapped from the 100-wide master to the 150-wide
    // instance: margins of 5 stay, so 150 - 10 = 140.
    CHECK_NEAR("bg w", bg->width, 140);
    CHECK_NEAR("bg x", bg->relativeTransform.m02, 5);

    // And back.
    if (!ui->setVariant("btn", "state", "default")) {  // case-insensitive
        std::printf("FAIL %s: setVariant(Default) returned false\n", __func__);
        ++g_failures;
        return;
    }
    btn = ui->document().findByName("btn");
    if (!btn->findByName("bg-default")) {
        std::printf("FAIL %s: default background missing after switch back\n", __func__);
        ++g_failures;
    }
}

int main() {
    testConstraints();
    testAutoLayoutColumn();
    testSpaceBetween();
    testHugAndAbsolute();
    testWrap();
    testMinMax();
    testVariants();
    if (g_failures == 0) {
        std::printf("layout_test: all checks passed\n");
        return 0;
    }
    std::printf("layout_test: %d check(s) FAILED\n", g_failures);
    return 1;
}
