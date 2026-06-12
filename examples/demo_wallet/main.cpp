// demo_wallet: a real "native app" built from wallet.fig with figmalib.
//
// Everything on screen is the Figma design; this file only supplies data and
// behavior — exactly the split the library is meant to enable:
//   - the portfolio list is data-driven (bindList over the Card template)
//   - tapping a coin opens Coin Info with a slide transition (navigation stack)
//   - the bottom nav bar switches screens; Backspace / right-click goes back
//   - the greeting is an editable text field (click it and type, CJK included)
//   - scrolling (wheel or touch-drag) comes from the design's scrollDirection
//
// --selfdrive <prefix> runs an unattended tour (home → coin info → edit →
// scroll), saving <prefix>_home/coin/edit/scroll.png for visual verification.

#include <cstdio>
#include <string>
#include <vector>

#include <raylib.h>

#include <figmalib/figmalib.h>
#include <figmalib_raylib.h>

namespace {

struct Coin {
    const char* symbol;
    const char* change;   // e.g. "+ 2.56"
    const char* usd;      // formatted balance
    const char* amount;   // holdings
    const char* rate;     // unit price for the detail screen
};

const std::vector<Coin> kPortfolio = {
    {"ETH", "+ 2.56", "$4.240,50", "25 ETH", "$420,50"},
    {"BTC", "- 1.20", "$2.890,00", "0.10 BTC", "$28.900,00"},
    {"BNB", "+ 0.88", "$1.024,37", "3.2 BNB", "$324,37"},
    {"XRP", "- 0.45", "$830,90", "1.800 XRP", "$0,46"},
    {"ADA", "+ 1.15", "$640,12", "1.700 ADA", "$0,38"},
    {"DOGE", "+ 5.02", "$420,69", "6.000 DOGE", "$0,07"},
    {"SOL", "- 2.31", "$386,40", "4.2 SOL", "$92,00"},
};

// First TEXT node in a subtree (template stamping helper).
figmalib::Node* firstText(figmalib::Node* root) {
    figmalib::Node* found = nullptr;
    if (root) {
        root->visit([&](figmalib::Node& n) {
            if (!found && n.type == figmalib::NodeType::Text) found = &n;
            return !found;
        });
    }
    return found;
}

}  // namespace

int main(int argc, char** argv) {
    std::string input;
    const char* drivePrefix = nullptr;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--selfdrive" && i + 1 < argc) drivePrefix = argv[++i];
        else input = arg;
    }
    if (input.empty()) {
        for (const char* cand : {"wallet.fig", "../wallet.fig",
                                 "D:/work_open/fig2psd/test/figma/wallet.fig"}) {
            if (FILE* f = fopen(cand, "rb")) {
                fclose(f);
                input = cand;
                break;
            }
        }
    }
    if (input.empty()) {
        std::printf("usage: demo_wallet [--selfdrive prefix] <wallet.fig>\n");
        return 1;
    }

    // No FLAG_WINDOW_HIGHDPI: on Windows raylib scales mouse input by 1/dpi
    // for that flag without scaling the framebuffer, so every click lands at
    // 2/3 of the pointer position (hitting the wrong element).
    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_VSYNC_HINT);
    InitWindow(420, 900, "wallet — a Figma file running as an app");

    auto ui = figmalib::FigmaUI::fromFile(input);
    ui->setResizeMode(figmalib::FigmaUI::ResizeMode::Reflow);
    ui->selectFrame("Home");

    // wallet.fig quirk: Discover/Marketplace/Profile author their "Bottom Nav
    // Bar" as fixed-when-scrolling, but the Home screens forgot to — pin them
    // all so the navigation never scrolls away with the content.
    ui->document().root->visit([](figmalib::Node& n) {
        if (n.name == "Bottom Nav Bar") n.scrollFixed = true;
        return true;
    });

    // ---- data: the portfolio list ----
    // Several frames contain a node named "List" (Trending is one); scope the
    // binding to the Portfolio section by renaming its list first.
    if (auto* pf = ui->document().findByName("Portfolio")) {
        if (auto* list = pf->findByName("List")) list->name = "portfolio-list";
        // The design sized this section for 4 rows (fixed height, centered);
        // with live data the section should grow downward instead.
        pf->autoLayout.primarySizing = figmalib::AutoLayout::Sizing::Hug;
        pf->autoLayout.primaryAlign = figmalib::AutoLayout::Align::Min;
    }
    auto bindPortfolio = [&]() {
        ui->bindList("portfolio-list", kPortfolio.size(),
                     [](figmalib::Node& item, size_t i) {
            const Coin& c = kPortfolio[i];
            auto* heading = item.findByName("Heading");
            if (heading && heading->children.size() >= 2) {
                figmalib::setNodeText(*heading->children[0], c.symbol);
                figmalib::setNodeText(*heading->children[1], c.change);
            }
            auto* balance = item.findByName("Balance");
            if (balance && balance->children.size() >= 2) {
                figmalib::setNodeText(*balance->children[0], c.usd);
                figmalib::setNodeText(*balance->children[1], c.amount);
            }
        });
    };
    bindPortfolio();

    // ---- behavior: tap a coin row → Coin Info ----
    auto openCoin = [&](const Coin& c) {
        ui->navigateTo("Coin Info", figmalib::FigmaUI::Transition::SlideLeft, 0.28f);
        // Stamp the detail screen for the tapped coin.
        if (auto* conv = ui->currentFrame()->findByName("Conversion Value")) {
            if (auto* unit = firstText(conv)) {
                figmalib::setNodeText(*unit, std::string("1 ") + c.symbol);
            }
            figmalib::Node* price = nullptr;  // the price is the last text child
            for (auto& child : conv->children) {
                if (child->type == figmalib::NodeType::Text) price = child.get();
            }
            if (price) figmalib::setNodeText(*price, c.rate);
            ui->markDirty();
        }
    };
    ui->onClick("Card", [&](figmalib::Node& n) {
        if (!n.parent || n.parent->name != "portfolio-list") return;  // hero card etc.
        size_t idx = 0;
        for (size_t i = 0; i < n.parent->children.size(); ++i) {
            if (n.parent->children[i].get() == &n) idx = i;
        }
        if (idx < kPortfolio.size()) openCoin(kPortfolio[idx]);
    });

    // ---- behavior: bottom navigation ----
    ui->onClick("Discover", [&](figmalib::Node&) {
        std::printf("[nav] Discover clicked\n");
        ui->navigateTo("Discover");
    });
    ui->onClick("Trade", [&](figmalib::Node&) {
        std::printf("[nav] Trade clicked\n");
        ui->navigateTo("Marketplace");
    });
    ui->onClick("Account", [&](figmalib::Node&) {
        std::printf("[nav] Account clicked\n");
        ui->navigateTo("Profile");
    });
    ui->onClick("Wallet", [&](figmalib::Node&) {  // center button = home
        std::printf("[nav] Wallet clicked\n");
        while (ui->canGoBack()) ui->navigateBack(0.0f);
        ui->navigateTo("Home", figmalib::FigmaUI::Transition::Dissolve, 0.2f);
    });

    // ---- behavior: the greeting is a text field ----
    if (auto* hero = ui->document().findByName("Hero")) {
        if (auto* greeting = firstText(hero)) {
            greeting->name = "greeting";
            ui->setEditable("greeting");
        }
    }

    figmalib::RaylibFigmaView view(*ui);
    int frame = 0;
    bool quit = false;

    figmalib::Node* lastFrameNode = ui->currentFrame();
    while (!WindowShouldClose() && !quit) {
        if (ui->currentFrame() != lastFrameNode) {
            std::printf("[frame] -> %s\n", ui->currentFrame()->name.c_str());
            std::fflush(stdout);
            lastFrameNode = ui->currentFrame();
        }
        // Back: Backspace (when not typing) or right mouse button.
        if ((IsKeyPressed(KEY_BACKSPACE) && !ui->focusedNode()) ||
            IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) {
            std::printf("[nav] back (key/right-click)\n");
            ui->navigateBack();
        }

        view.resize(GetScreenWidth(), GetScreenHeight());
        view.update();

        BeginDrawing();
        ClearBackground(Color{12, 14, 18, 255});
        view.draw();
        EndDrawing();

        // ---- unattended tour for automated visual verification ----
        if (drivePrefix) {
            ++frame;
            const auto shot = [&](const char* tag) {
                TakeScreenshot((std::string(drivePrefix) + "_" + tag + ".png").c_str());
            };
            if (frame == 30) {
                shot("home");
                // Tap the second portfolio row at its on-screen center.
                if (auto* list = ui->document().findByName("portfolio-list")) {
                    if (list->children.size() > 1) {
                        auto& card = *list->children[1];
                        float cx, cy;
                        card.absoluteTransform.apply(card.width * 0.5f,
                                                     card.height * 0.5f, cx, cy);
                        const auto t = ui->renderer().contentTransform();
                        float vx, vy;
                        t.apply(cx, cy, vx, vy);
                        ui->pointerDown(vx, vy);
                        ui->pointerUp(vx, vy);
                    }
                }
            } else if (frame == 36) {
                // Mid-slide: outgoing Home and incoming Coin Info composited
                // from the backend's cached textures, side by side.
                shot("transition");
            } else if (frame == 90) {
                shot("coin");
                ui->navigateBack(0.0f);
                ui->focusText("greeting");
                ui->textInput(" — 你好");
            } else if (frame == 104) {
                // Click-to-place-caret + drag-selection: sweep the pointer
                // across the focused greeting (one block — interleaving real
                // frames would fight the backend's mouse feed).
                if (auto* g = ui->document().findByName("greeting")) {
                    const auto t = ui->renderer().contentTransform();
                    auto vp = [&](float lx, float ly, float& vx, float& vy) {
                        float fx, fy;
                        g->absoluteTransform.apply(lx, ly, fx, fy);
                        t.apply(fx, fy, vx, vy);
                    };
                    float ax, ay, bx, by;
                    vp(g->width * 0.15f, g->height * 0.5f, ax, ay);
                    vp(g->width * 0.75f, g->height * 0.5f, bx, by);
                    ui->pointerDown(ax, ay);
                    ui->pointerMove(bx, by);
                    ui->pointerUp(bx, by);
                    std::printf("selection: anchor=%d caret=%d (text=%zu bytes)\n",
                                g->selAnchorByte, g->caretByte, g->characters.size());
                }
            } else if (frame == 112) {
                shot("select");
            } else if (frame == 116) {
                ui->editKey(figmalib::FigmaUI::EditKey::Left);  // collapse to start
                if (auto* g = ui->document().findByName("greeting")) {
                    std::printf("after Left: anchor=%d caret=%d\n", g->selAnchorByte,
                                g->caretByte);
                }
            } else if (frame == 120) {
                shot("edit");
                ui->blur();
                ui->scrollBy(GetScreenWidth() * 0.5f, GetScreenHeight() * 0.5f, 0, 400);
            } else if (frame == 144) {
                // Hit-test sanity after pure-scroll updates (no scene
                // rebuild): the node under the viewport center must match the
                // scrolled layout, not the pre-scroll one.
                figmalib::Node* hit =
                    ui->hitTest(GetScreenWidth() * 0.5f, GetScreenHeight() * 0.5f);
                std::printf("post-scroll hit at center: %s (scrollY=%.1f)\n",
                            hit ? hit->name.c_str() : "(none)",
                            ui->currentFrame()->scrollY);
            } else if (frame == 150) {
                shot("scroll");
                SetWindowSize(700, 560);  // resize coverage (landscape-ish)
            } else if (frame == 158) {
                // Drag-fling check: a quick upward drag synthesized in one
                // block (interleaving real frames would fight the backend's
                // own mouse feed), then release — the content must keep
                // coasting after the pointer lifts.
                const float cx = GetScreenWidth() * 0.5f;
                ui->pointerDown(cx, 400);
                for (int i = 1; i <= 5; ++i) {
                    ui->pointerMove(cx, 400.0f - i * 15.0f);
                    ui->update(1.0f / 60.0f);  // velocity sampling between moves
                }
                ui->pointerUp(cx, 325);
                std::printf("fling release: scrollY=%.1f\n", ui->currentFrame()->scrollY);
            } else if (frame == 172) {
                std::printf("fling +14 frames: scrollY=%.1f (should keep growing)\n",
                            ui->currentFrame()->scrollY);
            } else if (frame == 180) {
                std::printf("after resize: screen=%dx%d render=%dx%d dpi=%.2f "
                            "uiPixels=%ux%u\n",
                            GetScreenWidth(), GetScreenHeight(), GetRenderWidth(),
                            GetRenderHeight(), GetWindowScaleDPI().x,
                            ui->pixelWidth(), ui->pixelHeight());
                if (auto* home = ui->currentFrame()) {
                    std::printf("home scrollY=%.1f maxScrollY=%.1f height=%.1f\n",
                                home->scrollY, home->maxScrollY(), home->height);
                }
                shot("resized");
                SetWindowSize(420, 1500);  // taller than the content
            } else if (frame == 210) {
                if (auto* home = ui->currentFrame()) {
                    std::printf("tall window: scrollY=%.1f maxScrollY=%.1f height=%.1f\n",
                                home->scrollY, home->maxScrollY(), home->height);
                }
                shot("tall");
                SetWindowSize(420, 900);
            } else if (frame == 240) {
                // Reproduce the nav-bounce report: click the Discover nav item.
                figmalib::Node* item = nullptr;
                if (auto* nav = ui->currentFrame()->findByName("Navigation Bar")) {
                    item = nav->findByName("Discover");
                }
                if (item) {
                    float cx, cy;
                    item->absoluteTransform.apply(item->width * 0.5f,
                                                  item->height * 0.5f, cx, cy);
                    float vx, vy;
                    ui->renderer().contentTransform().apply(cx, cy, vx, vy);
                    std::printf("clicking Discover nav item at (%.0f, %.0f)\n", vx, vy);
                    ui->pointerDown(vx, vy);
                    ui->pointerUp(vx, vy);
                } else {
                    std::printf("Discover nav item not found\n");
                }
            } else if (frame == 280) {
                // The reported bounce: on the Discover page, tap Trade.
                figmalib::Node* item = nullptr;
                if (auto* nav = ui->currentFrame()->findByName("Navigation Bar")) {
                    item = nav->findByName("Trade");
                }
                if (item) {
                    float cx, cy, vx, vy;
                    item->absoluteTransform.apply(item->width * 0.5f,
                                                  item->height * 0.5f, cx, cy);
                    ui->renderer().contentTransform().apply(cx, cy, vx, vy);
                    std::printf("clicking Trade nav item at (%.0f, %.0f) on %s\n", vx,
                                vy, ui->currentFrame()->name.c_str());
                    ui->pointerDown(vx, vy);
                    ui->pointerUp(vx, vy);
                } else {
                    std::printf("Trade nav item not found on %s\n",
                                ui->currentFrame()->name.c_str());
                }
            } else if (frame > 240 && frame <= 360) {
                if (frame % 20 == 0) {
                    std::printf("frame %d: current=%s animating=%d\n", frame,
                                ui->currentFrame()->name.c_str(),
                                ui->animating() ? 1 : 0);
                }
                if (frame == 360) quit = true;
            }
        }
    }

    CloseWindow();
    return 0;
}
