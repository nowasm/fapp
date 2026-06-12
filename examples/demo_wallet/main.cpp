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

    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_VSYNC_HINT | FLAG_WINDOW_HIGHDPI);
    InitWindow(420, 900, "wallet — a Figma file running as an app");

    auto ui = figmalib::FigmaUI::fromFile(input);
    ui->setResizeMode(figmalib::FigmaUI::ResizeMode::Reflow);
    ui->selectFrame("Home");

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
    ui->onClick("Discover", [&](figmalib::Node&) { ui->navigateTo("Discover"); });
    ui->onClick("Trade", [&](figmalib::Node&) { ui->navigateTo("Marketplace"); });
    ui->onClick("Account", [&](figmalib::Node&) { ui->navigateTo("Profile"); });
    ui->onClick("Wallet", [&](figmalib::Node&) {  // center button = home
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

    while (!WindowShouldClose() && !quit) {
        // Back: Backspace (when not typing) or right mouse button.
        if ((IsKeyPressed(KEY_BACKSPACE) && !ui->focusedNode()) ||
            IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) {
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
            } else if (frame == 90) {
                shot("coin");
                ui->navigateBack(0.0f);
                ui->focusText("greeting");
                ui->textInput(" — 你好");
            } else if (frame == 120) {
                shot("edit");
                ui->blur();
                ui->scrollBy(GetScreenWidth() * 0.5f, GetScreenHeight() * 0.5f, 0, 400);
            } else if (frame == 150) {
                shot("scroll");
                quit = true;
            }
        }
    }

    CloseWindow();
    return 0;
}
