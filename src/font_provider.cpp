#include "font_provider.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <unordered_map>
#include <vector>

#include <thorvg.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace figo {

namespace {

std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// Style-name candidates per CSS weight, most specific first.
const std::vector<std::string>& styleNamesFor(int weight) {
    static const std::map<int, std::vector<std::string>> table = {
        {100, {"thin", "hairline"}},
        {200, {"extralight", "extra light", "ultralight"}},
        {300, {"light"}},
        {400, {"regular", "normal", "book"}},
        {500, {"medium"}},
        {600, {"semibold", "semi bold", "demibold", "demi bold"}},
        {700, {"bold"}},
        {800, {"extrabold", "extra bold", "ultrabold"}},
        {900, {"black", "heavy"}},
    };
    auto it = table.lower_bound(weight - 49);
    if (it == table.end()) --it;
    return it->second;
}

struct SystemFont {
    std::string displayName;  // lowercased registry value name, "(truetype)" stripped
    std::string path;
};

#ifdef _WIN32
std::vector<SystemFont> enumerateSystemFonts() {
    std::vector<SystemFont> fonts;
    HKEY key;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                      "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Fonts", 0,
                      KEY_READ, &key) != ERROR_SUCCESS) {
        return fonts;
    }
    char winDir[MAX_PATH] = {};
    GetWindowsDirectoryA(winDir, MAX_PATH);
    const std::string fontDir = std::string(winDir) + "\\Fonts\\";

    for (DWORD i = 0;; ++i) {
        char name[512];
        BYTE data[1024];
        DWORD nameLen = sizeof(name), dataLen = sizeof(data), type = 0;
        const LONG rc = RegEnumValueA(key, i, name, &nameLen, nullptr, &type, data, &dataLen);
        if (rc == ERROR_NO_MORE_ITEMS) break;
        if (rc != ERROR_SUCCESS || type != REG_SZ) continue;

        std::string display = toLower(name);
        for (const char* suffix : {" (truetype)", " (opentype)"}) {
            const size_t pos = display.find(suffix);
            if (pos != std::string::npos) display.resize(pos);
        }
        std::string file(reinterpret_cast<char*>(data));
        // ThorVG's loader handles .ttf/.otf; .ttc collections are unpacked
        // into standalone faces at load time (see extractTtcFace).
        const std::string lowerFile = toLower(file);
        if (lowerFile.size() < 4) continue;
        const std::string ext = lowerFile.substr(lowerFile.size() - 4);
        if (ext != ".ttf" && ext != ".otf" && ext != ".ttc") continue;

        std::string path = file.find(':') != std::string::npos ? file : fontDir + file;
        // Collection registry entries list every face joined by " & "
        // (e.g. "Microsoft YaHei & Microsoft YaHei UI"): one entry per face.
        size_t start = 0;
        while (start < display.size()) {
            size_t amp = display.find(" & ", start);
            if (amp == std::string::npos) amp = display.size();
            const std::string one = display.substr(start, amp - start);
            if (!one.empty()) fonts.push_back({one, path});
            start = amp + 3;
        }
    }
    RegCloseKey(key);
    return fonts;
}
#elif !defined(__APPLE__)
// Linux/other: no enumeration — apps should bundle fonts. (macOS impl below,
// after the sfnt readers it depends on.)
std::vector<SystemFont> enumerateSystemFonts() { return {}; }
#endif

// ---- minimal sfnt (TTF/OTF) metadata reader --------------------------------
// Pulls the typographic family from the `name` table (nameID 16, falling back
// to 1) and weight/italic from the OS/2 table. Enough to register arbitrary
// font files under the exact family names Figma documents reference.

struct SfntInfo {
    std::string family;
    std::string subfamily;  // nameID 2 / 17 — style word ("Bold", "Italic", …)
    int weight = 400;
    bool italic = false;
};

uint16_t be16(const uint8_t* p) { return static_cast<uint16_t>(p[0] << 8 | p[1]); }
uint32_t be32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) << 24 | static_cast<uint32_t>(p[1]) << 16 |
           static_cast<uint32_t>(p[2]) << 8 | p[3];
}

// `dirOff` points at the sfnt table directory: 0 for standalone fonts, the
// per-face offset for faces inside a .ttc (table offsets stay file-absolute).
bool parseSfnt(const std::vector<uint8_t>& d, SfntInfo& out, uint32_t dirOff = 0) {
    if (d.size() < dirOff + 12) return false;
    const uint32_t tag = be32(d.data() + dirOff);
    if (tag != 0x00010000 && tag != 0x4F54544F /*OTTO*/ && tag != 0x74727565 /*true*/)
        return false;
    const uint16_t numTables = be16(d.data() + dirOff + 4);
    if (d.size() < dirOff + 12 + numTables * 16u) return false;

    uint32_t nameOff = 0, nameLen = 0, os2Off = 0, os2Len = 0;
    for (uint16_t i = 0; i < numTables; ++i) {
        const uint8_t* rec = d.data() + dirOff + 12 + i * 16;
        const uint32_t t = be32(rec);
        const uint32_t off = be32(rec + 8), len = be32(rec + 12);
        if (t == 0x6E616D65) { nameOff = off; nameLen = len; }        // 'name'
        else if (t == 0x4F532F32) { os2Off = off; os2Len = len; }     // 'OS/2'
    }

    if (os2Off && os2Len >= 64 && os2Off + 64 <= d.size()) {
        const uint16_t weightClass = be16(d.data() + os2Off + 4);
        if (weightClass >= 100 && weightClass <= 1000) out.weight = weightClass;
        const uint16_t fsSelection = be16(d.data() + os2Off + 62);
        out.italic = (fsSelection & 0x01) != 0;
    }

    if (!nameOff || nameLen < 6 || nameOff + nameLen > d.size()) return false;
    const uint8_t* nt = d.data() + nameOff;
    const uint16_t count = be16(nt + 2);
    const uint16_t strOff = be16(nt + 4);
    std::string family1, family16, subfam2, subfam17;
    for (uint16_t i = 0; i < count; ++i) {
        const uint8_t* rec = nt + 6 + i * 12;
        if (rec + 12 > d.data() + nameOff + nameLen) break;
        const uint16_t platform = be16(rec), encoding = be16(rec + 2);
        const uint16_t nameId = be16(rec + 6);
        const uint16_t len = be16(rec + 8), off = be16(rec + 10);
        if (nameId != 1 && nameId != 16 && nameId != 2 && nameId != 17) continue;
        const uint8_t* s = nt + strOff + off;
        if (s + len > d.data() + nameOff + nameLen) continue;
        std::string value;
        if (platform == 3 || (platform == 0)) {  // UTF-16BE
            for (uint16_t k = 0; k + 1 < len; k += 2) {
                const uint16_t cp = be16(s + k);
                if (cp < 0x80) value += static_cast<char>(cp);
                else value += '?';  // family matching is ASCII-insensitive anyway
            }
        } else if (platform == 1 && encoding == 0) {  // Mac Roman ≈ ASCII
            value.assign(reinterpret_cast<const char*>(s), len);
        } else {
            continue;
        }
        if (value.empty()) continue;
        if (nameId == 16 && family16.empty()) family16 = value;
        if (nameId == 1 && family1.empty()) family1 = value;
        if (nameId == 17 && subfam17.empty()) subfam17 = value;
        if (nameId == 2 && subfam2.empty()) subfam2 = value;
    }
    out.family = !family16.empty() ? family16 : family1;
    out.subfamily = !subfam17.empty() ? subfam17 : subfam2;
    return !out.family.empty();
}

// ---- cmap coverage ----------------------------------------------------------
// Codepoint ranges a font actually maps to glyphs, read from cmap format 12
// (preferred) or format 4. Used for glyph-level font fallback: TTF renderers
// happily draw .notdef boxes, so coverage must be checked up front.

std::vector<std::pair<uint32_t, uint32_t>> parseCmapRanges(const std::vector<uint8_t>& d) {
    std::vector<std::pair<uint32_t, uint32_t>> out;
    if (d.size() < 12) return out;
    const uint16_t numTables = be16(d.data() + 4);
    if (d.size() < 12 + numTables * 16u) return out;
    uint32_t cmapOff = 0, cmapLen = 0;
    for (uint16_t i = 0; i < numTables; ++i) {
        const uint8_t* rec = d.data() + 12 + i * 16;
        if (be32(rec) == 0x636D6170) {  // 'cmap'
            cmapOff = be32(rec + 8);
            cmapLen = be32(rec + 12);
            break;
        }
    }
    if (!cmapOff || cmapLen < 4 || cmapOff > d.size() || cmapLen > d.size() - cmapOff) return out;
    const uint8_t* c = d.data() + cmapOff;

    const uint16_t n = be16(c + 2);
    uint32_t best = 0;
    int bestScore = -1;
    for (uint16_t i = 0; i < n && 4 + (i + 1) * 8u <= cmapLen; ++i) {
        const uint8_t* rec = c + 4 + i * 8;
        const uint16_t pid = be16(rec), eid = be16(rec + 2);
        const uint32_t off = be32(rec + 4);
        if (off + 4 > cmapLen) continue;
        int score = -1;
        if (pid == 3 && eid == 10) score = 5;       // Windows, full Unicode
        else if (pid == 0 && eid >= 4) score = 4;   // Unicode, full repertoire
        else if (pid == 3 && eid == 1) score = 3;   // Windows, BMP
        else if (pid == 0) score = 2;
        if (score > bestScore) {
            bestScore = score;
            best = off;
        }
    }
    if (bestScore < 0) return out;

    const uint8_t* sub = c + best;
    const uint32_t subAvail = cmapLen - best;
    const uint16_t format = be16(sub);
    if (format == 4 && subAvail >= 14) {
        const uint16_t segX2 = be16(sub + 6);
        if (14u + segX2 * 2u + 2u + segX2 <= subAvail) {
            const uint8_t* endCodes = sub + 14;
            const uint8_t* startCodes = sub + 14 + segX2 + 2;  // +2 reservedPad
            for (uint16_t s = 0; s < segX2; s += 2) {
                const uint32_t endC = be16(endCodes + s);
                const uint32_t startC = be16(startCodes + s);
                if (startC == 0xFFFF) continue;  // sentinel segment
                if (startC <= endC) out.emplace_back(startC, endC);
            }
        }
    } else if (format == 12 && subAvail >= 16) {
        const uint32_t groups = be32(sub + 12);
        for (uint32_t g = 0; g < groups && 16 + (g + 1) * 12u <= subAvail; ++g) {
            const uint8_t* gr = sub + 16 + g * 12;
            out.emplace_back(be32(gr), be32(gr + 4));
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

// ---- TTC (TrueType Collection) face extraction -----------------------------
// ThorVG only loads standalone sfnt fonts. Collection faces share glyph
// tables addressed by file-absolute offsets, so the wanted face is repacked
// into a self-contained blob with rewritten table offsets.

std::vector<uint8_t> repackFace(const std::vector<uint8_t>& d, uint32_t dirOff) {
    if (dirOff + 12 > d.size()) return {};
    const uint16_t numTables = be16(d.data() + dirOff + 4);
    if (dirOff + 12 + numTables * 16u > d.size()) return {};

    std::vector<uint8_t> out(d.begin() + dirOff, d.begin() + dirOff + 12 + numTables * 16u);
    for (uint16_t i = 0; i < numTables; ++i) {
        const uint32_t off = be32(out.data() + 12 + i * 16 + 8);
        const uint32_t len = be32(out.data() + 12 + i * 16 + 12);
        if (off > d.size() || len > d.size() - off) return {};
        const uint32_t newOff = static_cast<uint32_t>(out.size());
        out.insert(out.end(), d.begin() + off, d.begin() + off + len);
        out.resize((out.size() + 3) & ~size_t(3));  // 4-byte table alignment
        uint8_t* rec = out.data() + 12 + i * 16 + 8;
        rec[0] = static_cast<uint8_t>(newOff >> 24);
        rec[1] = static_cast<uint8_t>(newOff >> 16);
        rec[2] = static_cast<uint8_t>(newOff >> 8);
        rec[3] = static_cast<uint8_t>(newOff);
    }
    return out;
}

// Standalone blob for the collection face matching `family` (lowercased);
// falls back to the first parsable face. Empty when `d` is not a ttc.
std::vector<uint8_t> extractTtcFace(const std::vector<uint8_t>& d, const std::string& family) {
    if (d.size() < 16 || be32(d.data()) != 0x74746366 /*ttcf*/) return {};
    const uint32_t numFonts = std::min<uint32_t>(be32(d.data() + 8), 64);
    uint32_t chosen = 0;
    bool haveAny = false;
    for (uint32_t i = 0; i < numFonts; ++i) {
        if (12 + (i + 1) * 4u > d.size()) break;
        const uint32_t off = be32(d.data() + 12 + i * 4);
        SfntInfo info;
        if (!parseSfnt(d, info, off)) continue;
        if (!haveAny) {
            chosen = off;
            haveAny = true;
        }
        if (toLower(info.family) == family) {
            chosen = off;
            break;
        }
    }
    return haveAny ? repackFace(d, chosen) : std::vector<uint8_t>();
}

#ifdef __APPLE__
// macOS has no font registry; scan the standard font dirs and read each file's
// sfnt name table (handling .ttc collections) so findSystemPath can resolve
// "Helvetica", "Arial Unicode MS", "PingFang SC", etc. — mirroring the Windows
// registry path. system() caches the result, so this directory walk runs once.
std::vector<SystemFont> enumerateSystemFonts() {
    namespace fs = std::filesystem;
    std::vector<SystemFont> fonts;
    std::vector<std::string> dirs = {
        "/System/Library/Fonts", "/System/Library/Fonts/Supplemental", "/Library/Fonts"};
    if (const char* home = std::getenv("HOME"))
        dirs.push_back(std::string(home) + "/Library/Fonts");

    auto add = [&](const SfntInfo& info, const std::string& path) {
        if (info.family.empty()) return;
        const std::string sub = toLower(info.subfamily);
        const bool regularish =
            sub.empty() || sub == "regular" || sub == "book" || sub == "roman";
        // "helvetica" for the regular face, "helvetica bold" for styled faces —
        // matching findSystemPath's "<family> <style>" candidate strings.
        fonts.push_back({toLower(info.family) + (regularish ? "" : " " + sub), path});
    };

    for (const auto& dir : dirs) {
        std::error_code ec;
        if (!fs::is_directory(dir, ec)) continue;
        for (fs::directory_iterator it(dir, ec), end; it != end && !ec; it.increment(ec)) {
            const fs::path p = it->path();
            const std::string ext = toLower(p.extension().string());
            if (ext != ".ttf" && ext != ".otf" && ext != ".ttc") continue;
            std::ifstream f(p, std::ios::binary);
            if (!f) continue;
            std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)),
                                   std::istreambuf_iterator<char>());
            if (d.size() < 12) continue;
            if (be32(d.data()) == 0x74746366 /*ttcf*/) {  // collection: one entry per face
                const uint32_t numFonts = std::min<uint32_t>(be32(d.data() + 8), 64);
                for (uint32_t i = 0; i < numFonts; ++i) {
                    if (12 + (i + 1) * 4u > d.size()) break;
                    SfntInfo info;
                    if (parseSfnt(d, info, be32(d.data() + 12 + i * 4))) add(info, p.string());
                }
            } else {
                SfntInfo info;
                if (parseSfnt(d, info)) add(info, p.string());
            }
        }
    }
    return fonts;
}
#endif

}  // namespace

struct FontProvider::Impl {
    std::vector<SystemFont> systemFonts;
    bool systemFontsLoaded = false;
    // (family|weight|italic) → explicit path
    std::unordered_map<std::string, std::string> registered;
    // composite key → loaded into ThorVG
    std::unordered_map<std::string, bool> loaded;
    // composite key → sorted cmap coverage ranges
    std::unordered_map<std::string, std::vector<std::pair<uint32_t, uint32_t>>> coverage;
    std::string defaultFamily =
#ifdef __APPLE__
        "Helvetica";
#else
        "Segoe UI";
#endif
    std::string lastGoodKey;

    static std::string composite(const std::string& family, int weight, bool italic) {
        return toLower(family) + "|" + std::to_string(weight) + (italic ? "|i" : "");
    }

    const std::vector<SystemFont>& system() {
        if (!systemFontsLoaded) {
            systemFonts = enumerateSystemFonts();
            systemFontsLoaded = true;
        }
        return systemFonts;
    }

    std::string findSystemPath(const std::string& family, int weight, bool italic) {
        const std::string fam = toLower(family);
        const auto& styles = styleNamesFor(weight);

        // Build candidate display names, most specific first.
        std::vector<std::string> candidates;
        for (const auto& s : styles) {
            if (italic) candidates.push_back(fam + " " + s + " italic");
        }
        if (italic) candidates.push_back(fam + " italic");
        for (const auto& s : styles) candidates.push_back(fam + " " + s);
        if (weight < 550 && !italic) candidates.push_back(fam + " regular");
        candidates.push_back(fam);

        for (const auto& want : candidates) {
            for (const auto& f : system()) {
                if (f.displayName == want) return f.path;
            }
        }
        // Last resort: any face whose name starts with the family.
        for (const auto& f : system()) {
            if (f.displayName.rfind(fam + " ", 0) == 0) return f.path;
        }
        return {};
    }

    // Loads the font file under `key` so tvg::Text::font(key) resolves.
    // Collections (.ttc) are repacked to the face matching the key's family.
    bool loadAs(const std::string& key, const std::string& path) {
        // Short-circuit only on a prior SUCCESS. A cached failure must not block
        // a retry under the same key with a DIFFERENT path: fontKeyFor falls back
        // (requested family → defaultFamily → Arial) reusing one key, so caching
        // the first failed path as final would poison every later fallback and
        // drop the text entirely.
        if (auto it = loaded.find(key); it != loaded.end() && it->second) return true;

        std::ifstream f(path, std::ios::binary);
        bool ok = false;
        if (f) {
            std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)),
                                      std::istreambuf_iterator<char>());
            if (data.size() >= 4 && be32(data.data()) == 0x74746366 /*ttcf*/) {
                const std::string family = key.substr(0, key.find('|'));
                std::vector<uint8_t> face = extractTtcFace(data, family);
                if (!face.empty()) data = std::move(face);
            }
            if (!data.empty()) {
                ok = tvg::Text::load(key.c_str(), reinterpret_cast<char*>(data.data()),
                                     static_cast<uint32_t>(data.size()), "ttf",
                                     true /*copy*/) == tvg::Result::Success;
                if (ok) coverage[key] = parseCmapRanges(data);
            }
        }
        loaded[key] = ok;
        if (ok) lastGoodKey = key;
        return ok;
    }
};

FontProvider::FontProvider() : impl_(new Impl) {}
FontProvider::~FontProvider() { delete impl_; }

void FontProvider::registerFont(const std::string& family, const std::string& ttfPath,
                                int weight, bool italic) {
    impl_->registered[Impl::composite(family, weight, italic)] = ttfPath;
}

int FontProvider::registerFontsFromDirectory(const std::string& dir) {
    namespace fs = std::filesystem;
    std::error_code ec;
    int count = 0;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        const std::string ext = toLower(entry.path().extension().string());
        if (ext != ".ttf" && ext != ".otf") continue;

        std::ifstream f(entry.path(), std::ios::binary);
        if (!f) continue;
        std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)),
                                  std::istreambuf_iterator<char>());

        SfntInfo info;
        if (!parseSfnt(data, info)) continue;
        registerFont(info.family, entry.path().string(), info.weight, info.italic);
        ++count;
    }
    return count;
}

void FontProvider::setDefaultFamily(const std::string& family) {
    impl_->defaultFamily = family;
}

std::string FontProvider::fontKeyFor(const std::string& family, int weight, bool italic) {
    const std::string key = Impl::composite(family, weight, italic);

    if (auto it = impl_->registered.find(key); it != impl_->registered.end()) {
        if (impl_->loadAs(key, it->second)) return key;
    }
    // No exact registered match: pick the registered face of this family with
    // the nearest weight, preferring the requested italic flag.
    {
        const std::string famPrefix = toLower(family) + "|";
        std::string bestPath;
        int bestScore = 1 << 30;
        for (const auto& [k, path] : impl_->registered) {
            if (k.rfind(famPrefix, 0) != 0) continue;
            const size_t bar = famPrefix.size() - 1;
            const size_t bar2 = k.find('|', bar + 1);
            const int w = std::atoi(k.substr(bar + 1, bar2 == std::string::npos
                                                          ? std::string::npos
                                                          : bar2 - bar - 1).c_str());
            const bool ital = k.size() > 2 && k.compare(k.size() - 2, 2, "|i") == 0;
            const int score = std::abs(w - weight) + (ital != italic ? 1000 : 0);
            if (score < bestScore) {
                bestScore = score;
                bestPath = path;
            }
        }
        if (!bestPath.empty() && impl_->loadAs(key, bestPath)) return key;
    }

    if (std::string path = impl_->findSystemPath(family, weight, italic); !path.empty()) {
        if (impl_->loadAs(key, path)) return key;
    }

    if (toLower(family) != toLower(impl_->defaultFamily)) {
        return fontKeyFor(impl_->defaultFamily, weight, italic);
    }
    // Default family also missing — try Arial, then anything we ever loaded.
    if (toLower(family) != "arial") {
        if (std::string path = impl_->findSystemPath("Arial", weight, italic); !path.empty()) {
            if (impl_->loadAs(key, path)) return key;
        }
    }
    return impl_->lastGoodKey;
}

bool FontProvider::hasGlyph(const std::string& fontKey, unsigned long codepoint) {
    auto it = impl_->coverage.find(fontKey);
    if (it == impl_->coverage.end() || it->second.empty()) return true;  // unknown → assume yes
    const auto& r = it->second;
    auto lb = std::upper_bound(
        r.begin(), r.end(), std::make_pair(static_cast<uint32_t>(codepoint), UINT32_MAX));
    if (lb == r.begin()) return false;
    --lb;
    return codepoint >= lb->first && codepoint <= lb->second;
}

std::string FontProvider::fallbackFontFor(unsigned long codepoint, int weight, bool italic) {
    // Families with broad CJK / symbol coverage. fontKeyFor itself may resolve
    // to the default family — hasGlyph filters those out. Platform-ordered so the
    // locally-present families are tried first.
    static const char* candidates[] = {
#ifdef __APPLE__
        "PingFang SC",    "PingFang TC",     "Hiragino Sans GB", "Hiragino Sans",
        "Heiti SC",       "Arial Unicode MS", "Apple Color Emoji", "Apple Symbols",
#endif
        "Microsoft YaHei", "SimHei",          "SimSun",
        "Malgun Gothic",   "Yu Gothic UI",    "MS Gothic",
        "Segoe UI Symbol", "Segoe UI Emoji"};
    for (const char* fam : candidates) {
        const std::string key = fontKeyFor(fam, weight, italic);
        if (!key.empty() && hasGlyph(key, codepoint)) return key;
    }
    return {};
}

}  // namespace figo
