// Offscreen smoke test: render each frame of the sample UI to a BMP file.
// No engine backend involved — exercises parser + scene builder + renderer.

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <figmalib/figmalib.h>

#ifndef ASSETS_DIR
#define ASSETS_DIR "."
#endif

// 32-bit BGRA top-down BMP.
static bool writeBmp(const char* path, const uint32_t* rgba, uint32_t w, uint32_t h) {
    FILE* f = nullptr;
    if (fopen_s(&f, path, "wb") != 0 || !f) return false;

    const uint32_t imageSize = w * h * 4;
    uint8_t fileHeader[14] = {'B', 'M'};
    const uint32_t fileSize = 14 + 40 + imageSize;
    std::memcpy(fileHeader + 2, &fileSize, 4);
    const uint32_t dataOffset = 54;
    std::memcpy(fileHeader + 10, &dataOffset, 4);

    uint8_t infoHeader[40] = {};
    const uint32_t infoSize = 40;
    const int32_t negH = -static_cast<int32_t>(h);  // top-down
    const uint16_t planes = 1, bpp = 32;
    std::memcpy(infoHeader + 0, &infoSize, 4);
    std::memcpy(infoHeader + 4, &w, 4);
    std::memcpy(infoHeader + 8, &negH, 4);
    std::memcpy(infoHeader + 12, &planes, 2);
    std::memcpy(infoHeader + 14, &bpp, 2);
    std::memcpy(infoHeader + 20, &imageSize, 4);

    fwrite(fileHeader, 1, 14, f);
    fwrite(infoHeader, 1, 40, f);

    // RGBA (R first in memory) → BGRA, compositing onto dark gray so
    // transparency is visible in the output.
    std::vector<uint8_t> row(w * 4);
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            const uint8_t* p = reinterpret_cast<const uint8_t*>(&rgba[y * w + x]);
            const float a = p[3] / 255.0f;
            row[x * 4 + 0] = static_cast<uint8_t>(p[2] * a + 40 * (1 - a));
            row[x * 4 + 1] = static_cast<uint8_t>(p[1] * a + 40 * (1 - a));
            row[x * 4 + 2] = static_cast<uint8_t>(p[0] * a + 40 * (1 - a));
            row[x * 4 + 3] = 255;
        }
        fwrite(row.data(), 1, row.size(), f);
    }
    fclose(f);
    return true;
}

int main(int argc, char** argv) {
    // Optional argument: any supported input (.fig, canvas.json, REST JSON).
    const std::string input = argc > 1 ? argv[1] : ASSETS_DIR "/sample_ui.json";
    std::printf("loading %s\n", input.c_str());
    auto ui = figmalib::FigmaUI::fromFile(input);
    ui->setViewport(900, 640);

    int failures = 0;
    std::vector<std::string> seen;
    for (const auto& frameName : ui->frameNames()) {
        // selectFrame() picks by name; duplicate names would re-select the
        // same node and render() would correctly report "nothing changed".
        if (std::find(seen.begin(), seen.end(), frameName) != seen.end()) continue;
        seen.push_back(frameName);
        if (!ui->selectFrame(frameName)) {
            std::printf("FAIL: selectFrame(%s)\n", frameName.c_str());
            ++failures;
            continue;
        }
        if (!ui->render()) {
            std::printf("FAIL: render(%s) produced no pixels\n", frameName.c_str());
            ++failures;
            continue;
        }
        // Sanity: count non-transparent pixels.
        const uint32_t* px = ui->pixels();
        const uint32_t total = ui->pixelWidth() * ui->pixelHeight();
        uint32_t opaque = 0;
        for (uint32_t i = 0; i < total; ++i) {
            if (reinterpret_cast<const uint8_t*>(&px[i])[3] > 8) ++opaque;
        }
        std::string safe = frameName;
        for (char& c : safe) {
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_') c = '_';
        }
        const std::string out = "render_" + safe + ".bmp";
        writeBmp(out.c_str(), px, ui->pixelWidth(), ui->pixelHeight());
        std::printf("%s: %ux%u, %.1f%% painted -> %s\n", frameName.c_str(),
                    ui->pixelWidth(), ui->pixelHeight(), 100.0 * opaque / total,
                    out.c_str());
        if (opaque < total / 10) {
            std::printf("FAIL: %s mostly empty\n", frameName.c_str());
            ++failures;
        }
    }

    // Responsive reflow: render the first frame at a different aspect ratio
    // with constraints/auto-layout active instead of scale-to-fit.
    if (!seen.empty() && ui->selectFrame(seen.front())) {
        ui->setResizeMode(figmalib::FigmaUI::ResizeMode::Reflow);
        ui->setViewport(1280, 720);
        if (!ui->render()) {
            std::printf("FAIL: reflow render produced no pixels\n");
            ++failures;
        } else {
            const uint32_t* px = ui->pixels();
            const uint32_t total = ui->pixelWidth() * ui->pixelHeight();
            uint32_t opaque = 0;
            for (uint32_t i = 0; i < total; ++i) {
                if (reinterpret_cast<const uint8_t*>(&px[i])[3] > 8) ++opaque;
            }
            writeBmp("render_reflow.bmp", px, ui->pixelWidth(), ui->pixelHeight());
            std::printf("reflow %s: %ux%u, %.1f%% painted -> render_reflow.bmp\n",
                        seen.front().c_str(), ui->pixelWidth(), ui->pixelHeight(),
                        100.0 * opaque / total);
            if (opaque < total / 10) {
                std::printf("FAIL: reflow render mostly empty\n");
                ++failures;
            }
        }
        ui->setResizeMode(figmalib::FigmaUI::ResizeMode::Scale);
        ui->setViewport(900, 640);
    }

    // Scrolling: find a scrollable frame, scroll it to the end, and verify
    // the pixels actually moved (and that fixed children kept the frame from
    // being a pure translate — any change at all passes).
    {
        figmalib::Node* scrollable = nullptr;
        for (const auto& frameName : seen) {
            if (!ui->selectFrame(frameName)) continue;
            ui->currentFrame()->visit([&](figmalib::Node& n) {
                if (!scrollable && n.scrolls() &&
                    (n.maxScrollX() > 0 || n.maxScrollY() > 0)) {
                    scrollable = &n;
                }
                return true;
            });
            if (scrollable) break;
        }
        if (!scrollable) {
            std::printf("scroll: no scrollable node in document (skipped)\n");
        } else if (!ui->render()) {
            std::printf("FAIL: scroll baseline render\n");
            ++failures;
        } else {
            const uint32_t total = ui->pixelWidth() * ui->pixelHeight();
            std::vector<uint32_t> before(ui->pixels(), ui->pixels() + total);
            const float toX = scrollable->maxScrollX(), toY = scrollable->maxScrollY();
            if (!ui->setScroll(scrollable->name, toX, toY) || !ui->render()) {
                std::printf("FAIL: setScroll(%s)\n", scrollable->name.c_str());
                ++failures;
            } else {
                uint32_t diff = 0;
                for (uint32_t i = 0; i < total; ++i) {
                    if (ui->pixels()[i] != before[i]) ++diff;
                }
                writeBmp("render_scrolled.bmp", ui->pixels(), ui->pixelWidth(),
                         ui->pixelHeight());
                std::printf("scroll %s by (%.0f, %.0f): %.1f%% pixels changed -> "
                            "render_scrolled.bmp\n",
                            scrollable->name.c_str(), toX, toY, 100.0 * diff / total);
                if (diff == 0) {
                    std::printf("FAIL: scrolling changed nothing\n");
                    ++failures;
                }
            }
            // Resize while scrolled: a viewport tall enough to swallow the
            // scroll range must pull the offset back in (window-resize bug).
            ui->setResizeMode(figmalib::FigmaUI::ResizeMode::Reflow);
            ui->setViewport(900, 640);
            ui->setScroll(scrollable->name, scrollable->maxScrollX(),
                          scrollable->maxScrollY());
            ui->setViewport(900, 2400);
            ui->render();
            if (scrollable->scrollY > scrollable->maxScrollY() + 0.01f ||
                scrollable->scrollX > scrollable->maxScrollX() + 0.01f) {
                std::printf("FAIL: scroll offset (%.1f, %.1f) beyond range "
                            "(%.1f, %.1f) after resize\n",
                            scrollable->scrollX, scrollable->scrollY,
                            scrollable->maxScrollX(), scrollable->maxScrollY());
                ++failures;
            } else {
                std::printf("scroll clamp after resize: (%.1f, %.1f) within "
                            "(%.1f, %.1f)\n",
                            scrollable->scrollX, scrollable->scrollY,
                            scrollable->maxScrollX(), scrollable->maxScrollY());
            }
            ui->setResizeMode(figmalib::FigmaUI::ResizeMode::Scale);
            ui->setViewport(900, 640);
            ui->setScroll(scrollable->name, 0, 0);
        }
    }

    // Text measurement drives layout: in Reflow mode an auto-height text node
    // must grow when a longer string wraps to more lines.
    {
        figmalib::Node* textNode = nullptr;
        for (const auto& frameName : seen) {
            if (!ui->selectFrame(frameName)) continue;
            ui->currentFrame()->visit([&](figmalib::Node& n) {
                if (!textNode && n.type == figmalib::NodeType::Text && n.width > 40 &&
                    !n.characters.empty()) {
                    textNode = &n;
                }
                return true;
            });
            if (textNode) break;
        }
        if (!textNode) {
            std::printf("text-measure: no text node (skipped)\n");
        } else {
            const std::string savedChars = textNode->characters;
            const std::string savedResize = textNode->textStyle.autoResize;
            textNode->textStyle.autoResize = "HEIGHT";
            ui->setResizeMode(figmalib::FigmaUI::ResizeMode::Reflow);
            ui->setViewport(900, 640);
            ui->render();
            const float before = textNode->height;
            // Mutate the node directly — setText() resolves by name, which can
            // collide with the frame itself (union.fig "Practice designs").
            textNode->characters =
                "The quick brown fox jumps over the lazy dog again and "
                "again and again until the line is far too long to fit";
            textNode->textRuns.clear();
            ui->setViewport(899, 640);  // size change forces a reflow
            ui->render();
            const float after = textNode->height;
            std::printf("text-measure %s: height %.1f -> %.1f\n",
                        textNode->name.c_str(), before, after);
            if (!(after > before + 1)) {
                std::printf("FAIL: auto-height text did not grow\n");
                ++failures;
            }
            textNode->characters = savedChars;
            textNode->textStyle.autoResize = savedResize;
            ui->setResizeMode(figmalib::FigmaUI::ResizeMode::Scale);
            ui->setViewport(900, 640);
        }
    }

    // Text editing: focus an editable node, type (incl. CJK), navigate, and
    // delete; the caret must also show up in the pixels.
    {
        figmalib::Node* textNode = nullptr;
        for (const auto& frameName : seen) {
            if (!ui->selectFrame(frameName)) continue;
            ui->currentFrame()->visit([&](figmalib::Node& n) {
                if (!textNode && n.type == figmalib::NodeType::Text &&
                    !n.characters.empty()) {
                    textNode = &n;
                }
                return true;
            });
            if (textNode) break;
        }
        if (!textNode) {
            std::printf("edit: no text node (skipped)\n");
        } else {
            const std::string savedChars = textNode->characters;
            const std::string savedName = textNode->name;
            textNode->name = "__edit_target__";
            textNode->characters = "ab";
            ui->setEditable("__edit_target__");
            ui->render();
            const uint32_t total = ui->pixelWidth() * ui->pixelHeight();
            std::vector<uint32_t> before(ui->pixels(), ui->pixels() + total);

            ui->focusText("__edit_target__");          // caret at end
            ui->textInput("c你好");                     // ab|c你好
            ui->editKey(figmalib::FigmaUI::EditKey::Backspace);  // drop 好
            ui->editKey(figmalib::FigmaUI::EditKey::Left);       // skip 你
            ui->editKey(figmalib::FigmaUI::EditKey::Left);       // skip c
            ui->textInput("X");                         // ab X c你
            bool ok = textNode->characters == "abXc\xe4\xbd\xa0";
            if (!ok) {
                std::printf("FAIL: edit ops produced \"%s\"\n",
                            textNode->characters.c_str());
                ++failures;
            }
            if (!ui->render()) {
                std::printf("FAIL: edit render\n");
                ++failures;
            } else {
                uint32_t diff = 0;
                for (uint32_t i = 0; i < total; ++i) {
                    if (ui->pixels()[i] != before[i]) ++diff;
                }
                std::printf("edit \"%s\": %u pixels changed (text + caret)\n",
                            textNode->characters.c_str(), diff);
                if (diff == 0) {
                    std::printf("FAIL: editing changed no pixels\n");
                    ++failures;
                }
            }
            ui->blur();
            textNode->characters = savedChars;
            textNode->name = savedName;
            textNode->textRuns.clear();
            ui->markDirty();
        }
    }

    // Data-driven list: clone an auto-layout container's template child per
    // item, stamp each with text, and verify stacking + pixel changes.
    {
        figmalib::Node* list = nullptr;
        for (const auto& frameName : seen) {
            if (!ui->selectFrame(frameName)) continue;
            ui->currentFrame()->visit([&](figmalib::Node& n) {
                if (!list && n.autoLayout.enabled() && !n.children.empty() &&
                    n.parent && n.width > 0) {
                    // Need a text somewhere in the template to stamp.
                    bool hasText = false;
                    n.children.front()->visit([&](figmalib::Node& t) {
                        if (t.type == figmalib::NodeType::Text) hasText = true;
                        return !hasText;
                    });
                    if (hasText) list = &n;
                }
                return true;
            });
            if (list) break;
        }
        if (!list) {
            std::printf("bindList: no auto-layout list candidate (skipped)\n");
        } else {
            const std::string savedName = list->name;
            list->name = "__list__";
            ui->render();
            const uint32_t total = ui->pixelWidth() * ui->pixelHeight();
            std::vector<uint32_t> before(ui->pixels(), ui->pixels() + total);

            const bool ok = ui->bindList("__list__", 5, [](figmalib::Node& item, size_t i) {
                bool done = false;
                item.visit([&](figmalib::Node& t) {
                    if (!done && t.type == figmalib::NodeType::Text) {
                        figmalib::setNodeText(t, "Item " + std::to_string(i + 1));
                        done = true;
                    }
                    return !done;
                });
            });
            if (!ok || list->children.size() != 5) {
                std::printf("FAIL: bindList -> %zu items\n", list->children.size());
                ++failures;
            } else {
                const auto& a = *list->children[0];
                const auto& b = *list->children[1];
                const bool stacked = a.relativeTransform.m02 != b.relativeTransform.m02 ||
                                     a.relativeTransform.m12 != b.relativeTransform.m12;
                ui->render();
                uint32_t diff = 0;
                for (uint32_t i = 0; i < total; ++i) {
                    if (ui->pixels()[i] != before[i]) ++diff;
                }
                std::printf("bindList %s: 5 items, stacked=%d, %u pixels changed\n",
                            savedName.c_str(), stacked ? 1 : 0, diff);
                if (!stacked || diff == 0) {
                    std::printf("FAIL: bindList items not laid out / not drawn\n");
                    ++failures;
                }
            }
            list->name = savedName;
        }
    }

    // Navigation: transitions composite cached textures in the BACKEND, so
    // the renderer's pixel buffer holds the incoming frame for the whole
    // animation. The library-side contract is: the incoming frame rasterizes
    // once right away, transitionId bumps for the backend snapshot, progress
    // advances while animating, and back returns.
    if (seen.size() >= 2) {
        ui->selectFrame(seen[0]);
        ui->render();
        const uint32_t total = ui->pixelWidth() * ui->pixelHeight();
        ui->selectFrame(seen[1]);
        ui->render();
        std::vector<uint32_t> b(ui->pixels(), ui->pixels() + total);
        ui->selectFrame(seen[0]);
        ui->render();
        const uint32_t idBefore = ui->transitionId();

        if (!ui->navigateTo(seen[1], figmalib::FigmaUI::Transition::SlideLeft, 0.3f)) {
            std::printf("FAIL: navigateTo(%s)\n", seen[1].c_str());
            ++failures;
        } else {
            const bool idBumped = ui->transitionId() != idBefore;
            // The animation clock clamps each tick to 1/30 s, so step in
            // frame-sized increments like a real host would.
            for (int i = 0; i < 5; ++i) ui->update(1.0f / 30);  // ~halfway
            const bool midAnimating = ui->animating();
            const float midProgress = ui->transitionProgress();
            ui->render();
            uint32_t midDiffB = 0;  // pixels must already show the incoming frame
            for (uint32_t i = 0; i < total; ++i) {
                if (ui->pixels()[i] != b[i]) ++midDiffB;
            }
            for (int i = 0; i < 20; ++i) ui->update(1.0f / 30);  // finish
            const bool doneAnimating = ui->animating();
            const bool backOk =
                ui->canGoBack() && ui->navigateBack(0.0f) && ui->currentFrame() &&
                ui->currentFrame()->name == seen[0];
            std::printf("navigate %s -> %s: id+%d mid(anim=%d p=%.2f diffB=%u) "
                        "end(anim=%d) back=%d\n",
                        seen[0].c_str(), seen[1].c_str(), idBumped ? 1 : 0,
                        midAnimating ? 1 : 0, midProgress, midDiffB,
                        doneAnimating ? 1 : 0, backOk ? 1 : 0);
            if (!idBumped || !midAnimating || midProgress <= 0 || midProgress >= 1 ||
                midDiffB != 0 || doneAnimating || !backOk) {
                std::printf("FAIL: navigation transition wrong\n");
                ++failures;
            }
        }
    }

    // Click-to-navigate consumes the event: nav items share names with their
    // destination frames, so a click must not bubble past the navigation and
    // fire the old frame's own handler (Discover page -> tap Trade ->
    // Marketplace must stick, not bounce back to Discover).
    {
        const bool hasFrames = ui->selectFrame("Discover") &&
                               ui->selectFrame("Marketplace") &&
                               ui->selectFrame("Discover");
        figmalib::Node* tradeItem =
            hasFrames ? ui->currentFrame()->findByName("Trade") : nullptr;
        if (!tradeItem) {
            std::printf("nav-bubble: no Discover/Trade setup (skipped)\n");
        } else {
            ui->onClick("Trade", [&](figmalib::Node&) { ui->navigateTo("Marketplace"); });
            ui->onClick("Discover", [&](figmalib::Node&) { ui->navigateTo("Discover"); });
            ui->render();  // refresh absoluteTransform for the hit test
            float cx, cy, vx, vy;
            tradeItem->absoluteTransform.apply(tradeItem->width * 0.5f,
                                               tradeItem->height * 0.5f, cx, cy);
            ui->renderer().contentTransform().apply(cx, cy, vx, vy);
            ui->pointerDown(vx, vy);
            ui->pointerUp(vx, vy);
            ui->update(10.0f);  // finish any transition
            const std::string where =
                ui->currentFrame() ? ui->currentFrame()->name : "(null)";
            std::printf("nav-bubble: Discover + Trade click -> %s\n", where.c_str());
            if (where != "Marketplace") {
                std::printf("FAIL: navigation did not consume the click\n");
                ++failures;
            }
        }
    }

    // Hit-test sanity on the sample UI: center of the start button.
    if (ui->selectFrame("MainMenu")) {
        ui->render();
        if (figmalib::Node* hit = ui->hitTest(450, 320)) {
            std::printf("hitTest(450,320) -> %s\n", hit->name.c_str());
        } else {
            std::printf("FAIL: hitTest(450,320) -> null\n");
            ++failures;
        }
    }

    std::printf(failures ? "RESULT: %d failure(s)\n" : "RESULT: OK\n", failures);
    return failures;
}
