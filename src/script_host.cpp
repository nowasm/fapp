#include "figmalib/script.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <quickjs.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#endif

#ifdef __EMSCRIPTEN__
#include <emscripten/fetch.h>
#endif

#include <map>

#include <nlohmann/json.hpp>

#include "figmalib/document.h"
#include "figmalib/ui.h"

namespace figmalib {

namespace {

// RAII for JS_ToCString.
struct CStr {
    JSContext* ctx;
    const char* s;
    CStr(JSContext* c, JSValueConst v) : ctx(c), s(JS_ToCString(c, v)) {}
    ~CStr() {
        if (s) JS_FreeCString(ctx, s);
    }
    explicit operator bool() const { return s != nullptr; }
    operator std::string() const { return s ? s : ""; }
};

const char* nodeTypeName(NodeType t) {
    switch (t) {
    case NodeType::Document: return "Document";
    case NodeType::Canvas: return "Canvas";
    case NodeType::Frame: return "Frame";
    case NodeType::Group: return "Group";
    case NodeType::Section: return "Section";
    case NodeType::Rectangle: return "Rectangle";
    case NodeType::Ellipse: return "Ellipse";
    case NodeType::Line: return "Line";
    case NodeType::Vector: return "Vector";
    case NodeType::BooleanOperation: return "BooleanOperation";
    case NodeType::Star: return "Star";
    case NodeType::RegularPolygon: return "RegularPolygon";
    case NodeType::Text: return "Text";
    case NodeType::Component: return "Component";
    case NodeType::ComponentSet: return "ComponentSet";
    case NodeType::Instance: return "Instance";
    case NodeType::Slice: return "Slice";
    default: return "Unknown";
    }
}

FigmaUI::Transition parseTransition(const std::string& s) {
    if (s == "slideLeft") return FigmaUI::Transition::SlideLeft;
    if (s == "slideRight") return FigmaUI::Transition::SlideRight;
    if (s == "slideUp") return FigmaUI::Transition::SlideUp;
    if (s == "slideDown") return FigmaUI::Transition::SlideDown;
    if (s == "dissolve") return FigmaUI::Transition::Dissolve;
    return FigmaUI::Transition::None;
}

// ---- fetch: background worker + main-thread result queue ----

struct FetchResult {
    uint64_t id = 0;
    int status = 0;
    std::string body;
    std::string error;  // non-empty → reject
};

// Shared with worker threads; may outlive the ScriptHost (results are then
// simply never drained).
struct FetchQueue {
    std::mutex mutex;
    std::vector<FetchResult> results;
};

#ifdef _WIN32

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n =
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

void fetchWorker(std::shared_ptr<FetchQueue> queue, uint64_t id, std::string url,
                 std::string method, std::string headers, std::string body) {
    FetchResult res;
    res.id = id;
    HINTERNET ses = nullptr, con = nullptr, req = nullptr;
    auto finish = [&] {
        if (req) WinHttpCloseHandle(req);
        if (con) WinHttpCloseHandle(con);
        if (ses) WinHttpCloseHandle(ses);
        std::lock_guard<std::mutex> lock(queue->mutex);
        queue->results.push_back(std::move(res));
    };
    auto fail = [&](const char* what) {
        res.error = std::string(what) + " (code " + std::to_string(GetLastError()) + ")";
        finish();
    };

    const std::wstring wurl = widen(url);
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256] = {}, path[2048] = {};
    uc.lpszHostName = host;
    uc.dwHostNameLength = 255;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 2047;
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) return fail("bad url");

    ses = WinHttpOpen(L"figmalib/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) return fail("WinHttpOpen");
    con = WinHttpConnect(ses, host, uc.nPort, 0);
    if (!con) return fail("connect");
    const bool https = uc.nScheme == INTERNET_SCHEME_HTTPS;
    req = WinHttpOpenRequest(con, widen(method).c_str(), path, nullptr, WINHTTP_NO_REFERER,
                             WINHTTP_DEFAULT_ACCEPT_TYPES, https ? WINHTTP_FLAG_SECURE : 0);
    if (!req) return fail("open request");

    const std::wstring whdrs = widen(headers);
    if (!WinHttpSendRequest(req,
                            whdrs.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : whdrs.c_str(),
                            whdrs.empty() ? 0 : static_cast<DWORD>(whdrs.size()),
                            body.empty() ? WINHTTP_NO_REQUEST_DATA
                                         : const_cast<char*>(body.data()),
                            static_cast<DWORD>(body.size()),
                            static_cast<DWORD>(body.size()), 0)) {
        return fail("send");
    }
    if (!WinHttpReceiveResponse(req, nullptr)) return fail("receive");

    DWORD status = 0, sz = sizeof(status);
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);
    res.status = static_cast<int>(status);

    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(req, &avail)) return fail("read");
        if (avail == 0) break;
        const size_t at = res.body.size();
        res.body.resize(at + avail);
        DWORD got = 0;
        if (!WinHttpReadData(req, res.body.data() + at, avail, &got)) return fail("read");
        res.body.resize(at + got);
    }
    finish();
}

#elif defined(__EMSCRIPTEN__)

// Browser XHR via the emscripten Fetch API (-sFETCH, async — works without
// pthreads). fetchWorker only STARTS the request and returns; the completion
// callbacks run on the main thread between frames and push into the shared
// queue exactly like the Windows worker threads do. Subject to the browser's
// CORS rules: cross-origin servers must send Access-Control-Allow-Origin.
struct EmFetch {
    std::shared_ptr<FetchQueue> queue;
    uint64_t id = 0;
    std::string method, body;
    std::vector<std::string> headerStorage;  // alternating key, value
    std::vector<const char*> headerPtrs;     // …as C strings, null-terminated
};

void emFetchDone(emscripten_fetch_t* f) {
    std::unique_ptr<EmFetch> c(static_cast<EmFetch*>(f->userData));
    FetchResult res;
    res.id = c->id;
    res.status = f->status;
    if (f->data && f->numBytes > 0) {
        res.body.assign(f->data, f->data + f->numBytes);
    }
    // onerror also fires for non-2xx statuses, but those ARE completed HTTP
    // responses — resolve with the status (fetch semantics: ok=false), and
    // reject only when nothing came back at all (network/CORS failure).
    if (res.status == 0) res.error = "network error (unreachable or CORS)";
    {
        std::lock_guard<std::mutex> lock(c->queue->mutex);
        c->queue->results.push_back(std::move(res));
    }
    emscripten_fetch_close(f);
}

void fetchWorker(std::shared_ptr<FetchQueue> queue, uint64_t id, std::string url,
                 std::string method, std::string headers, std::string body) {
    auto c = std::make_unique<EmFetch>();
    c->queue = std::move(queue);
    c->id = id;
    c->method = method.empty() ? "GET" : std::move(method);
    c->body = std::move(body);
    // "Key: Value\r\n…" → alternating key/value strings.
    for (size_t at = 0; at < headers.size();) {
        size_t end = headers.find("\r\n", at);
        if (end == std::string::npos) end = headers.size();
        const std::string line = headers.substr(at, end - at);
        at = end + 2;
        const size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string val = line.substr(colon + 1);
        val.erase(0, val.find_first_not_of(' '));
        c->headerStorage.push_back(line.substr(0, colon));
        c->headerStorage.push_back(std::move(val));
    }
    for (const std::string& s : c->headerStorage) c->headerPtrs.push_back(s.c_str());
    c->headerPtrs.push_back(nullptr);

    emscripten_fetch_attr_t attr;
    emscripten_fetch_attr_init(&attr);
    std::snprintf(attr.requestMethod, sizeof(attr.requestMethod), "%s",
                  c->method.c_str());
    attr.attributes = EMSCRIPTEN_FETCH_LOAD_TO_MEMORY;
    attr.onsuccess = emFetchDone;
    attr.onerror = emFetchDone;
    if (c->headerPtrs.size() > 1) attr.requestHeaders = c->headerPtrs.data();
    if (!c->body.empty()) {
        attr.requestData = c->body.data();
        attr.requestDataSize = c->body.size();
    }
    attr.userData = c.get();
    if (emscripten_fetch(&attr, url.c_str())) {
        c.release();  // owned by the request now; emFetchDone frees it
    } else {
        FetchResult res;
        res.id = id;
        res.error = "fetch failed to start";
        std::lock_guard<std::mutex> lock(c->queue->mutex);
        c->queue->results.push_back(std::move(res));
    }
}

#else

void fetchWorker(std::shared_ptr<FetchQueue> queue, uint64_t id, std::string, std::string,
                 std::string, std::string) {
    FetchResult res;
    res.id = id;
    res.error = "fetch is not supported on this platform yet";
    std::lock_guard<std::mutex> lock(queue->mutex);
    queue->results.push_back(std::move(res));
}

#endif

}  // namespace

struct ScriptHost::Impl {
    FigmaUI& ui;
    JSRuntime* rt = nullptr;
    JSContext* ctx = nullptr;
    JSClassID nodeClass = 0;
    // Handler functions handed to FigmaUI callbacks live as long as the
    // context; freed in the destructor.
    std::vector<JSValue> retained;
    std::vector<JSValue> updateFns;

    // setTimeout/setInterval, driven by update(dt)'s accumulated clock.
    struct Timer {
        int64_t id;
        double due;          // clock seconds
        double intervalSec;  // repeat period (setInterval)
        bool repeat;
        JSValue fn;
    };
    double clock = 0;
    int64_t nextTimerId = 1;
    std::vector<Timer> timers;

    // fetch: worker threads push results; update() resolves the promises.
    std::shared_ptr<FetchQueue> fetchQueue = std::make_shared<FetchQueue>();
    uint64_t nextFetchId = 1;
    struct PendingFetch {
        JSValue resolve, reject;
    };
    std::unordered_map<uint64_t, PendingFetch> pendingFetch;

    // localStorage: in-memory map, write-through to a JSON file when a
    // storage path is set (std::map keeps the file diff-stable).
    std::string storagePath;
    std::map<std::string, std::string> storage;
    bool storageLoaded = false;

    explicit Impl(FigmaUI& u) : ui(u) {}

    void loadStorage() {
        if (storageLoaded) return;
        storageLoaded = true;
        if (storagePath.empty()) return;
        std::ifstream f(storagePath, std::ios::binary);
        if (!f) return;
        const auto j = nlohmann::json::parse(f, nullptr, false /*no throw*/);
        if (!j.is_object()) return;
        for (const auto& [k, v] : j.items()) {
            if (v.is_string()) storage[k] = v.get<std::string>();
        }
    }

    void saveStorage() {
        if (storagePath.empty()) return;
        nlohmann::json j = nlohmann::json::object();
        for (const auto& [k, v] : storage) j[k] = v;
        std::ofstream f(storagePath, std::ios::binary | std::ios::trunc);
        if (f) f << j.dump(2);
    }

    void fireDueTimers() {
        std::vector<JSValue> fire;
        for (auto it = timers.begin(); it != timers.end();) {
            if (clock >= it->due) {
                fire.push_back(JS_DupValue(ctx, it->fn));
                if (it->repeat) {
                    it->due += it->intervalSec;
                    if (it->due <= clock) it->due = clock + it->intervalSec;
                    ++it;
                } else {
                    JS_FreeValue(ctx, it->fn);
                    it = timers.erase(it);
                }
            } else {
                ++it;
            }
        }
        for (JSValue f : fire) {  // callbacks may add/clear timers freely now
            JSValue r = JS_Call(ctx, f, JS_UNDEFINED, 0, nullptr);
            if (JS_IsException(r)) dumpError();
            JS_FreeValue(ctx, r);
            JS_FreeValue(ctx, f);
        }
    }

    void clearTimer(int64_t id) {
        for (auto it = timers.begin(); it != timers.end(); ++it) {
            if (it->id == id) {
                JS_FreeValue(ctx, it->fn);
                timers.erase(it);
                return;
            }
        }
    }

    void drainFetchResults() {
        std::vector<FetchResult> done;
        {
            std::lock_guard<std::mutex> lock(fetchQueue->mutex);
            done.swap(fetchQueue->results);
        }
        for (FetchResult& r : done) {
            auto it = pendingFetch.find(r.id);
            if (it == pendingFetch.end()) continue;
            PendingFetch p = it->second;
            pendingFetch.erase(it);
            if (r.error.empty()) {
                JSValue obj = JS_NewObject(ctx);
                JS_SetPropertyStr(ctx, obj, "status", JS_NewInt32(ctx, r.status));
                JS_SetPropertyStr(ctx, obj, "ok",
                                  JS_NewBool(ctx, r.status >= 200 && r.status < 300));
                JS_SetPropertyStr(ctx, obj, "body",
                                  JS_NewStringLen(ctx, r.body.data(), r.body.size()));
                callVoid(p.resolve, 1, &obj);
            } else {
                JSValue err = JS_NewString(ctx, r.error.c_str());
                callVoid(p.reject, 1, &err);
            }
            JS_FreeValue(ctx, p.resolve);
            JS_FreeValue(ctx, p.reject);
        }
    }

    static Impl* from(JSContext* c) {
        return static_cast<Impl*>(JS_GetContextOpaque(c));
    }

    void dumpError() {
        JSValue exc = JS_GetException(ctx);
        {
            CStr s(ctx, exc);
            std::fprintf(stderr, "[script] %s\n", s.s ? s.s : "(unprintable exception)");
        }
        JSValue stack = JS_GetPropertyStr(ctx, exc, "stack");
        if (JS_IsString(stack)) {
            CStr st(ctx, stack);
            if (st.s && *st.s) std::fprintf(stderr, "%s", st.s);
        }
        JS_FreeValue(ctx, stack);
        JS_FreeValue(ctx, exc);
    }

    // Call a JS function, dumping (not propagating) any exception.
    void callVoid(JSValueConst fn, int argc, JSValue* argv) {
        JSValue r = JS_Call(ctx, fn, JS_UNDEFINED, argc, argv);
        if (JS_IsException(r)) dumpError();
        JS_FreeValue(ctx, r);
        for (int i = 0; i < argc; ++i) JS_FreeValue(ctx, argv[i]);
    }

    JSValue wrapNode(Node* n) {
        if (!n) return JS_NULL;
        JSValue obj = JS_NewObjectClass(ctx, static_cast<int>(nodeClass));
        JS_SetOpaque(obj, n);
        return obj;
    }

    // Name lookup: current frame first (matches FigmaUI's by-name methods),
    // then the whole document.
    Node* findNode(const std::string& name) {
        Node* f = ui.currentFrame();
        Node* n = f ? f->findByName(name) : nullptr;
        return n ? n : ui.document().findByName(name);
    }
};

namespace {

Node* nodeOf(JSContext* ctx, JSValueConst v) {
    auto* im = ScriptHost::Impl::from(ctx);
    return static_cast<Node*>(JS_GetOpaque(v, im->nodeClass));
}

// ---- node object ----

enum NodeProp {
    NP_NAME,
    NP_ID,
    NP_TYPE,
    NP_TEXT,
    NP_CHILD_COUNT,
    NP_PARENT,
    NP_INDEX,
    NP_VISIBLE,
    NP_OPACITY,
    NP_SCROLL_FIXED,
    NP_PRIMARY_SIZING,
    NP_PRIMARY_ALIGN,
};

JSValue nodeGet(JSContext* ctx, JSValueConst thisVal, int /*argc*/, JSValueConst* /*argv*/,
                int magic) {
    Node* n = nodeOf(ctx, thisVal);
    if (!n) return JS_ThrowTypeError(ctx, "not a node");
    switch (magic) {
    case NP_NAME: return JS_NewString(ctx, n->name.c_str());
    case NP_ID: return JS_NewString(ctx, n->id.c_str());
    case NP_TYPE: return JS_NewString(ctx, nodeTypeName(n->type));
    case NP_TEXT: return JS_NewString(ctx, n->characters.c_str());
    case NP_CHILD_COUNT: return JS_NewInt32(ctx, static_cast<int32_t>(n->children.size()));
    case NP_PARENT: return ScriptHost::Impl::from(ctx)->wrapNode(n->parent);
    case NP_INDEX: {
        int32_t idx = -1;
        if (n->parent) {
            for (size_t i = 0; i < n->parent->children.size(); ++i) {
                if (n->parent->children[i].get() == n) {
                    idx = static_cast<int32_t>(i);
                    break;
                }
            }
        }
        return JS_NewInt32(ctx, idx);
    }
    case NP_SCROLL_FIXED: return JS_NewBool(ctx, n->scrollFixed);
    default: return JS_UNDEFINED;
    }
}

JSValue nodeSet(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv,
                int magic) {
    Node* n = nodeOf(ctx, thisVal);
    if (!n) return JS_ThrowTypeError(ctx, "not a node");
    if (argc < 1) return JS_ThrowTypeError(ctx, "value expected");
    auto* im = ScriptHost::Impl::from(ctx);
    switch (magic) {
    case NP_NAME: {
        CStr s(ctx, argv[0]);
        if (!s) return JS_EXCEPTION;
        n->name = s.s;
        break;
    }
    case NP_TEXT: {
        CStr s(ctx, argv[0]);
        if (!s) return JS_EXCEPTION;
        setNodeText(*n, s.s);
        im->ui.markDirty();
        break;
    }
    case NP_VISIBLE:
        n->runtimeVisible = JS_ToBool(ctx, argv[0]) ? 1 : 0;
        im->ui.markDirty();
        break;
    case NP_OPACITY: {
        double v = 0;
        if (JS_ToFloat64(ctx, &v, argv[0])) return JS_EXCEPTION;
        n->runtimeOpacity = static_cast<float>(v);
        im->ui.markDirty();
        break;
    }
    case NP_SCROLL_FIXED:
        n->scrollFixed = JS_ToBool(ctx, argv[0]);
        im->ui.markDirty();
        break;
    case NP_PRIMARY_SIZING: {
        CStr s(ctx, argv[0]);
        if (!s) return JS_EXCEPTION;
        n->autoLayout.primarySizing = std::strcmp(s.s, "hug") == 0
                                          ? AutoLayout::Sizing::Hug
                                          : AutoLayout::Sizing::Fixed;
        im->ui.markDirty();
        break;
    }
    case NP_PRIMARY_ALIGN: {
        CStr s(ctx, argv[0]);
        if (!s) return JS_EXCEPTION;
        n->autoLayout.primaryAlign =
            std::strcmp(s.s, "center") == 0         ? AutoLayout::Align::Center
            : std::strcmp(s.s, "max") == 0          ? AutoLayout::Align::Max
            : std::strcmp(s.s, "spaceBetween") == 0 ? AutoLayout::Align::SpaceBetween
                                                    : AutoLayout::Align::Min;
        im->ui.markDirty();
        break;
    }
    default: break;
    }
    return JS_UNDEFINED;
}

JSValue nodeFind(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    Node* n = nodeOf(ctx, thisVal);
    if (!n) return JS_ThrowTypeError(ctx, "not a node");
    if (argc < 1) return JS_ThrowTypeError(ctx, "name expected");
    CStr s(ctx, argv[0]);
    if (!s) return JS_EXCEPTION;
    return ScriptHost::Impl::from(ctx)->wrapNode(n->findByName(s.s));
}

JSValue nodeChild(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    Node* n = nodeOf(ctx, thisVal);
    if (!n) return JS_ThrowTypeError(ctx, "not a node");
    int32_t i = -1;
    if (argc < 1 || JS_ToInt32(ctx, &i, argv[0])) return JS_EXCEPTION;
    if (i < 0 || static_cast<size_t>(i) >= n->children.size()) return JS_NULL;
    return ScriptHost::Impl::from(ctx)->wrapNode(n->children[i].get());
}

// ---- ui object ----

// Helpers: first argument is a node name.
bool argName(JSContext* ctx, JSValueConst v, std::string& out) {
    CStr s(ctx, v);
    if (!s) return false;
    out = s.s;
    return true;
}

JSValue ui_onClick(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* im = ScriptHost::Impl::from(ctx);
    std::string name;
    if (argc < 2 || !argName(ctx, argv[0], name)) return JS_EXCEPTION;
    if (!JS_IsFunction(ctx, argv[1])) return JS_ThrowTypeError(ctx, "function expected");
    JSValue fn = JS_DupValue(ctx, argv[1]);
    im->retained.push_back(fn);
    im->ui.onClick(name, [im, fn](Node& n) {
        JSValue arg = im->wrapNode(&n);
        im->callVoid(fn, 1, &arg);
    });
    return JS_UNDEFINED;
}

JSValue ui_onHover(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* im = ScriptHost::Impl::from(ctx);
    std::string name;
    if (argc < 2 || !argName(ctx, argv[0], name)) return JS_EXCEPTION;
    if (!JS_IsFunction(ctx, argv[1])) return JS_ThrowTypeError(ctx, "function expected");
    JSValue fn = JS_DupValue(ctx, argv[1]);
    im->retained.push_back(fn);
    im->ui.onHover(name, [im, fn](Node& n, bool entered) {
        JSValue args[2] = {im->wrapNode(&n), JS_NewBool(im->ctx, entered)};
        im->callVoid(fn, 2, args);
    });
    return JS_UNDEFINED;
}

JSValue ui_onUpdate(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* im = ScriptHost::Impl::from(ctx);
    if (argc < 1 || !JS_IsFunction(ctx, argv[0])) {
        return JS_ThrowTypeError(ctx, "function expected");
    }
    im->updateFns.push_back(JS_DupValue(ctx, argv[0]));
    return JS_UNDEFINED;
}

JSValue ui_navigateTo(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* im = ScriptHost::Impl::from(ctx);
    std::string name;
    if (argc < 1 || !argName(ctx, argv[0], name)) return JS_EXCEPTION;
    FigmaUI::Transition t = FigmaUI::Transition::SlideLeft;
    if (argc >= 2 && JS_IsString(argv[1])) {
        CStr s(ctx, argv[1]);
        if (s) t = parseTransition(s.s);
    }
    double dur = 0.3;
    if (argc >= 3 && JS_ToFloat64(ctx, &dur, argv[2])) return JS_EXCEPTION;
    return JS_NewBool(ctx, im->ui.navigateTo(name, t, static_cast<float>(dur)));
}

JSValue ui_navigateBack(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* im = ScriptHost::Impl::from(ctx);
    double dur = 0.3;
    if (argc >= 1 && JS_ToFloat64(ctx, &dur, argv[0])) return JS_EXCEPTION;
    return JS_NewBool(ctx, im->ui.navigateBack(static_cast<float>(dur)));
}

JSValue ui_canGoBack(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    return JS_NewBool(ctx, ScriptHost::Impl::from(ctx)->ui.canGoBack());
}

JSValue ui_selectFrame(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    std::string name;
    if (argc < 1 || !argName(ctx, argv[0], name)) return JS_EXCEPTION;
    return JS_NewBool(ctx, ScriptHost::Impl::from(ctx)->ui.selectFrame(name));
}

JSValue ui_frameNames(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    auto* im = ScriptHost::Impl::from(ctx);
    JSValue arr = JS_NewArray(ctx);
    uint32_t i = 0;
    for (const auto& n : im->ui.frameNames()) {
        JS_SetPropertyUint32(ctx, arr, i++, JS_NewString(ctx, n.c_str()));
    }
    return arr;
}

JSValue ui_currentFrame(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    auto* im = ScriptHost::Impl::from(ctx);
    return im->wrapNode(im->ui.currentFrame());
}

JSValue ui_setResizeMode(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* im = ScriptHost::Impl::from(ctx);
    std::string mode;
    if (argc < 1 || !argName(ctx, argv[0], mode)) return JS_EXCEPTION;
    im->ui.setResizeMode(mode == "reflow" ? FigmaUI::ResizeMode::Reflow
                                          : FigmaUI::ResizeMode::Scale);
    return JS_UNDEFINED;
}

JSValue ui_bindList(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* im = ScriptHost::Impl::from(ctx);
    std::string name;
    int64_t count = 0;
    if (argc < 3 || !argName(ctx, argv[0], name) || JS_ToInt64(ctx, &count, argv[1])) {
        return JS_EXCEPTION;
    }
    if (!JS_IsFunction(ctx, argv[2])) return JS_ThrowTypeError(ctx, "function expected");
    // bindList runs the binder synchronously, so borrowing argv[2] is safe.
    JSValueConst fn = argv[2];
    const bool ok = im->ui.bindList(
        name, static_cast<size_t>(count < 0 ? 0 : count), [im, fn](Node& item, size_t i) {
            JSValue args[2] = {im->wrapNode(&item),
                               JS_NewInt32(im->ctx, static_cast<int32_t>(i))};
            im->callVoid(fn, 2, args);
        });
    return JS_NewBool(ctx, ok);
}

JSValue ui_setText(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* im = ScriptHost::Impl::from(ctx);
    std::string name, text;
    if (argc < 2 || !argName(ctx, argv[0], name) || !argName(ctx, argv[1], text)) {
        return JS_EXCEPTION;
    }
    return JS_NewBool(ctx, im->ui.setText(name, text));
}

JSValue ui_setVisible(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* im = ScriptHost::Impl::from(ctx);
    std::string name;
    if (argc < 2 || !argName(ctx, argv[0], name)) return JS_EXCEPTION;
    return JS_NewBool(ctx, im->ui.setVisible(name, JS_ToBool(ctx, argv[1])));
}

JSValue ui_setOpacity(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* im = ScriptHost::Impl::from(ctx);
    std::string name;
    double v = 0;
    if (argc < 2 || !argName(ctx, argv[0], name) || JS_ToFloat64(ctx, &v, argv[1])) {
        return JS_EXCEPTION;
    }
    return JS_NewBool(ctx, im->ui.setOpacity(name, static_cast<float>(v)));
}

JSValue ui_setVariant(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* im = ScriptHost::Impl::from(ctx);
    std::string name, prop, value;
    if (argc < 3 || !argName(ctx, argv[0], name) || !argName(ctx, argv[1], prop) ||
        !argName(ctx, argv[2], value)) {
        return JS_EXCEPTION;
    }
    return JS_NewBool(ctx, im->ui.setVariant(name, prop, value));
}

JSValue ui_setScroll(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* im = ScriptHost::Impl::from(ctx);
    std::string name;
    double x = 0, y = 0;
    if (argc < 3 || !argName(ctx, argv[0], name) || JS_ToFloat64(ctx, &x, argv[1]) ||
        JS_ToFloat64(ctx, &y, argv[2])) {
        return JS_EXCEPTION;
    }
    return JS_NewBool(ctx,
                      im->ui.setScroll(name, static_cast<float>(x), static_cast<float>(y)));
}

JSValue ui_setEditable(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* im = ScriptHost::Impl::from(ctx);
    std::string name;
    if (argc < 1 || !argName(ctx, argv[0], name)) return JS_EXCEPTION;
    const bool editable = argc < 2 || JS_ToBool(ctx, argv[1]);
    return JS_NewBool(ctx, im->ui.setEditable(name, editable));
}

JSValue ui_focusText(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* im = ScriptHost::Impl::from(ctx);
    std::string name;
    if (argc < 1 || !argName(ctx, argv[0], name)) return JS_EXCEPTION;
    return JS_NewBool(ctx, im->ui.focusText(name));
}

JSValue ui_blur(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    ScriptHost::Impl::from(ctx)->ui.blur();
    return JS_UNDEFINED;
}

JSValue ui_markDirty(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    ScriptHost::Impl::from(ctx)->ui.markDirty();
    return JS_UNDEFINED;
}

JSValue ui_find(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* im = ScriptHost::Impl::from(ctx);
    std::string name;
    if (argc < 1 || !argName(ctx, argv[0], name)) return JS_EXCEPTION;
    return im->wrapNode(im->findNode(name));
}

JSValue ui_findAll(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* im = ScriptHost::Impl::from(ctx);
    std::string name;
    if (argc < 1 || !argName(ctx, argv[0], name)) return JS_EXCEPTION;
    JSValue arr = JS_NewArray(ctx);
    uint32_t i = 0;
    if (Node* root = im->ui.document().root.get()) {
        root->visit([&](Node& n) {
            if (n.name == name) {
                JS_SetPropertyUint32(ctx, arr, i++, im->wrapNode(&n));
            }
            return true;
        });
    }
    return arr;
}

// Synthesized click at the node's on-screen center (uses the hit-test
// transforms, so render at least one frame first). Accepts a node object or
// a node name.
JSValue ui_tap(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* im = ScriptHost::Impl::from(ctx);
    if (argc < 1) return JS_ThrowTypeError(ctx, "node or name expected");
    Node* n = JS_IsObject(argv[0]) ? nodeOf(ctx, argv[0]) : nullptr;
    if (!n) {
        std::string name;
        if (!argName(ctx, argv[0], name)) return JS_EXCEPTION;
        n = im->findNode(name);
    }
    if (!n) return JS_NewBool(ctx, false);
    float cx, cy, vx, vy;
    n->absoluteTransform.apply(n->width * 0.5f, n->height * 0.5f, cx, cy);
    im->ui.renderer().contentTransform().apply(cx, cy, vx, vy);
    im->ui.pointerDown(vx, vy);
    im->ui.pointerUp(vx, vy);
    return JS_NewBool(ctx, true);
}

// localStorage (magic: 0 getItem, 1 setItem, 2 removeItem, 3 clear).
JSValue js_storage(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv, int magic) {
    auto* im = ScriptHost::Impl::from(ctx);
    im->loadStorage();
    std::string key;
    if (magic <= 2 && (argc < 1 || !argName(ctx, argv[0], key))) return JS_EXCEPTION;
    switch (magic) {
    case 0: {  // getItem -> string | null
        const auto it = im->storage.find(key);
        return it == im->storage.end() ? JS_NULL : JS_NewString(ctx, it->second.c_str());
    }
    case 1: {  // setItem
        std::string value;
        if (argc < 2 || !argName(ctx, argv[1], value)) return JS_EXCEPTION;
        im->storage[key] = value;
        im->saveStorage();
        return JS_UNDEFINED;
    }
    case 2:  // removeItem
        if (im->storage.erase(key) > 0) im->saveStorage();
        return JS_UNDEFINED;
    default:  // clear
        if (!im->storage.empty()) {
            im->storage.clear();
            im->saveStorage();
        }
        return JS_UNDEFINED;
    }
}

// setTimeout / setInterval (magic: 0 = once, 1 = repeat) -> timer id.
JSValue js_setTimer(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv, int magic) {
    auto* im = ScriptHost::Impl::from(ctx);
    if (argc < 1 || !JS_IsFunction(ctx, argv[0])) {
        return JS_ThrowTypeError(ctx, "function expected");
    }
    double ms = 0;
    if (argc >= 2 && JS_ToFloat64(ctx, &ms, argv[1])) return JS_EXCEPTION;
    if (ms < 0) ms = 0;
    ScriptHost::Impl::Timer t;
    t.id = im->nextTimerId++;
    t.intervalSec = ms / 1000.0;
    t.due = im->clock + t.intervalSec;
    t.repeat = magic == 1;
    t.fn = JS_DupValue(ctx, argv[0]);
    im->timers.push_back(t);
    return JS_NewInt64(ctx, t.id);
}

JSValue js_clearTimer(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv, int) {
    auto* im = ScriptHost::Impl::from(ctx);
    int64_t id = 0;
    if (argc < 1 || JS_ToInt64(ctx, &id, argv[0])) return JS_UNDEFINED;
    im->clearTimer(id);
    return JS_UNDEFINED;
}

// __fetch(url, method, headersCRLF, body) -> Promise<{status, ok, body}>.
// The standard fetch(url, opts) wrapper is defined in the JS prelude.
JSValue js_fetchNative(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* im = ScriptHost::Impl::from(ctx);
    std::string url, method, headers, body;
    if (argc < 4 || !argName(ctx, argv[0], url) || !argName(ctx, argv[1], method) ||
        !argName(ctx, argv[2], headers) || !argName(ctx, argv[3], body)) {
        return JS_EXCEPTION;
    }
    JSValue funcs[2];
    JSValue promise = JS_NewPromiseCapability(ctx, funcs);
    if (JS_IsException(promise)) return promise;
    const uint64_t id = im->nextFetchId++;
    im->pendingFetch[id] = {funcs[0], funcs[1]};
#ifdef _WIN32
    std::thread(fetchWorker, im->fetchQueue, id, std::move(url), std::move(method),
                std::move(headers), std::move(body))
        .detach();
#else
    // No thread (wasm builds run without pthreads): emscripten's worker just
    // STARTS the async browser request; the stub queues its error
    // immediately. Either way the promise settles in a later update().
    fetchWorker(im->fetchQueue, id, std::move(url), std::move(method),
                std::move(headers), std::move(body));
#endif
    return promise;
}

JSValue js_consoleLog(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    std::string line;
    for (int i = 0; i < argc; ++i) {
        CStr s(ctx, argv[i]);
        if (i) line += ' ';
        line += s.s ? s.s : "(?)";
    }
    std::printf("[js] %s\n", line.c_str());
    std::fflush(stdout);
    return JS_UNDEFINED;
}

}  // namespace

ScriptHost::ScriptHost(FigmaUI& ui) : impl_(std::make_unique<Impl>(ui)) {
    auto& d = *impl_;
    d.rt = JS_NewRuntime();
    d.ctx = JS_NewContext(d.rt);
    JS_SetContextOpaque(d.ctx, &d);
    JSContext* ctx = d.ctx;

    // Node class + prototype.
    JS_NewClassID(d.rt, &d.nodeClass);
    JSClassDef def{};
    def.class_name = "FigmaNode";
    JS_NewClass(d.rt, d.nodeClass, &def);
    JSValue proto = JS_NewObject(ctx);
    auto prop = [&](const char* name, int magic, bool writable) {
        JSAtom atom = JS_NewAtom(ctx, name);
        JSValue get =
            JS_NewCFunctionMagic(ctx, nodeGet, name, 0, JS_CFUNC_generic_magic, magic);
        JSValue set = writable ? JS_NewCFunctionMagic(ctx, nodeSet, name, 1,
                                                      JS_CFUNC_generic_magic, magic)
                               : JS_UNDEFINED;
        JS_DefinePropertyGetSet(ctx, proto, atom, get, set,
                                JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
        JS_FreeAtom(ctx, atom);
    };
    prop("name", NP_NAME, true);
    prop("id", NP_ID, false);
    prop("type", NP_TYPE, false);
    prop("text", NP_TEXT, true);
    prop("childCount", NP_CHILD_COUNT, false);
    prop("parent", NP_PARENT, false);
    prop("index", NP_INDEX, false);
    prop("visible", NP_VISIBLE, true);
    prop("opacity", NP_OPACITY, true);
    prop("scrollFixed", NP_SCROLL_FIXED, true);
    prop("primarySizing", NP_PRIMARY_SIZING, true);
    prop("primaryAlign", NP_PRIMARY_ALIGN, true);
    JS_SetPropertyStr(ctx, proto, "find", JS_NewCFunction(ctx, nodeFind, "find", 1));
    JS_SetPropertyStr(ctx, proto, "child", JS_NewCFunction(ctx, nodeChild, "child", 1));
    JS_SetClassProto(ctx, d.nodeClass, proto);

    // Global `ui` and `console`.
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue uiObj = JS_NewObject(ctx);
    auto fn = [&](const char* name, JSCFunction* f, int len) {
        JS_SetPropertyStr(ctx, uiObj, name, JS_NewCFunction(ctx, f, name, len));
    };
    fn("onClick", ui_onClick, 2);
    fn("onHover", ui_onHover, 2);
    fn("onUpdate", ui_onUpdate, 1);
    fn("navigateTo", ui_navigateTo, 3);
    fn("navigateBack", ui_navigateBack, 1);
    fn("canGoBack", ui_canGoBack, 0);
    fn("selectFrame", ui_selectFrame, 1);
    fn("frameNames", ui_frameNames, 0);
    fn("currentFrame", ui_currentFrame, 0);
    fn("setResizeMode", ui_setResizeMode, 1);
    fn("bindList", ui_bindList, 3);
    fn("setText", ui_setText, 2);
    fn("setVisible", ui_setVisible, 2);
    fn("setOpacity", ui_setOpacity, 2);
    fn("setVariant", ui_setVariant, 3);
    fn("setScroll", ui_setScroll, 3);
    fn("setEditable", ui_setEditable, 2);
    fn("focusText", ui_focusText, 1);
    fn("blur", ui_blur, 0);
    fn("markDirty", ui_markDirty, 0);
    fn("find", ui_find, 1);
    fn("findAll", ui_findAll, 1);
    fn("tap", ui_tap, 1);
    JS_SetPropertyStr(ctx, global, "ui", uiObj);

    JSValue consoleObj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, consoleObj, "log",
                      JS_NewCFunction(ctx, js_consoleLog, "log", 1));
    JS_SetPropertyStr(ctx, consoleObj, "warn",
                      JS_NewCFunction(ctx, js_consoleLog, "warn", 1));
    JS_SetPropertyStr(ctx, consoleObj, "error",
                      JS_NewCFunction(ctx, js_consoleLog, "error", 1));
    JS_SetPropertyStr(ctx, global, "console", consoleObj);

    // Timers + fetch.
    JS_SetPropertyStr(ctx, global, "setTimeout",
                      JS_NewCFunctionMagic(ctx, js_setTimer, "setTimeout", 2,
                                           JS_CFUNC_generic_magic, 0));
    JS_SetPropertyStr(ctx, global, "setInterval",
                      JS_NewCFunctionMagic(ctx, js_setTimer, "setInterval", 2,
                                           JS_CFUNC_generic_magic, 1));
    JS_SetPropertyStr(ctx, global, "clearTimeout",
                      JS_NewCFunctionMagic(ctx, js_clearTimer, "clearTimeout", 1,
                                           JS_CFUNC_generic_magic, 0));
    JS_SetPropertyStr(ctx, global, "clearInterval",
                      JS_NewCFunctionMagic(ctx, js_clearTimer, "clearInterval", 1,
                                           JS_CFUNC_generic_magic, 0));
    JS_SetPropertyStr(ctx, global, "__fetch",
                      JS_NewCFunction(ctx, js_fetchNative, "__fetch", 4));

    JSValue storageObj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, storageObj, "getItem",
                      JS_NewCFunctionMagic(ctx, js_storage, "getItem", 1,
                                           JS_CFUNC_generic_magic, 0));
    JS_SetPropertyStr(ctx, storageObj, "setItem",
                      JS_NewCFunctionMagic(ctx, js_storage, "setItem", 2,
                                           JS_CFUNC_generic_magic, 1));
    JS_SetPropertyStr(ctx, storageObj, "removeItem",
                      JS_NewCFunctionMagic(ctx, js_storage, "removeItem", 1,
                                           JS_CFUNC_generic_magic, 2));
    JS_SetPropertyStr(ctx, storageObj, "clear",
                      JS_NewCFunctionMagic(ctx, js_storage, "clear", 0,
                                           JS_CFUNC_generic_magic, 3));
    JS_SetPropertyStr(ctx, global, "localStorage", storageObj);
    JS_FreeValue(ctx, global);

    // fetch(url, opts) on top of __fetch: flatten headers, wrap the response.
    eval(R"JS(
globalThis.fetch = (url, opts) => {
    opts = opts || {};
    const headers = Object.entries(opts.headers || {})
        .map(([k, v]) => k + ": " + v).join("\r\n");
    return __fetch(String(url), String(opts.method || "GET"), headers,
                   opts.body == null ? "" : String(opts.body))
        .then((r) => ({
            status: r.status,
            ok: r.ok,
            text: () => r.body,
            json: () => JSON.parse(r.body),
        }));
};
)JS",
         "<prelude>");
}

ScriptHost::~ScriptHost() {
    auto& d = *impl_;
    for (JSValue v : d.retained) JS_FreeValue(d.ctx, v);
    for (JSValue v : d.updateFns) JS_FreeValue(d.ctx, v);
    for (auto& t : d.timers) JS_FreeValue(d.ctx, t.fn);
    for (auto& [id, p] : d.pendingFetch) {
        JS_FreeValue(d.ctx, p.resolve);
        JS_FreeValue(d.ctx, p.reject);
    }
    JS_FreeContext(d.ctx);
    JS_FreeRuntime(d.rt);
    // In-flight fetch threads keep the queue alive via shared_ptr and finish
    // harmlessly; their results are never drained.
}

void ScriptHost::setStoragePath(const std::string& path) {
    impl_->storagePath = path;
    impl_->storageLoaded = false;  // re-read from the new location on next access
    impl_->storage.clear();
}

bool ScriptHost::eval(const std::string& source, const std::string& filename) {
    auto& d = *impl_;
    JSValue r = JS_Eval(d.ctx, source.c_str(), source.size(), filename.c_str(),
                        JS_EVAL_TYPE_GLOBAL);
    const bool ok = !JS_IsException(r);
    if (!ok) d.dumpError();
    JS_FreeValue(d.ctx, r);
    return ok;
}

bool ScriptHost::runFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        std::fprintf(stderr, "[script] cannot open %s\n", path.c_str());
        return false;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    return eval(ss.str(), path);
}

void ScriptHost::update(float dtSeconds) {
    auto& d = *impl_;
    d.clock += dtSeconds;
    d.fireDueTimers();
    for (JSValue fn : d.updateFns) {
        JSValue arg = JS_NewFloat64(d.ctx, dtSeconds);
        d.callVoid(fn, 1, &arg);
    }
    d.drainFetchResults();
    JSContext* c = nullptr;
    while (JS_ExecutePendingJob(d.rt, &c) > 0) {
    }
}

}  // namespace figmalib
