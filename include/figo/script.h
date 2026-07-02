#pragma once
// figo — JS scripting host (QuickJS).
//
// Turns an app into <design.fig> + <logic.js>: the script gets a global `ui`
// object bound to a FigmaUI plus `console.log`. The host app only loads the
// two files and runs the frame loop.
//
// JS API (all names resolve within the current frame first, then document):
//   ui.onClick(name, fn(node))         ui.onHover(name, fn(node, entered))
//   ui.onUpdate(fn(dtSeconds))         ui.markDirty()
//   ui.navigateTo(name, transition?, durationSec?)   // "slideLeft" | "slideRight"
//   ui.navigateBack(durationSec?)                    // | "slideUp" | "slideDown"
//   ui.canGoBack() -> bool                           // | "dissolve" | "none"
//   ui.transitionProgress() -> number   // eased [0,1) mid-transition, 1 when
//                                       // idle — gate shots on >= 1
//   ui.selectFrame(name)               ui.frameNames() -> [string]
//   ui.currentFrame() -> node          ui.setResizeMode("reflow" | "scale")
//   ui.bindList(name, count, fn(itemNode, index))
//   ui.setText(name, s)  ui.setVisible(name, b)  ui.setOpacity(name, v)
//   ui.setVariant(name, property, value)         ui.setScroll(name, x, y)
//   ui.setEditable(name, editable?)    ui.focusText(name)    ui.blur()
//   ui.find(name) -> node|null         ui.findAll(name) -> [node]
//   ui.tap(nameOrNode) -> bool         // synthesized click at the node center
// By-name lookups (ui.find/setText/setVisible/...) search the current frame
// first, then the whole document. Structural mutations (ui.bindList /
// ui.setVariant) invalidate live node handles; don't call them from inside
// an onClick/onHover handler's node argument scope and re-find afterwards.
// Node objects (valid only while the underlying document node lives — don't
// hold them across bindList/setVariant/navigation):
//   node.name (get/set)   node.id   node.type        // "Text", "Frame", ...
//   node.text (get/set)   node.visible (get/set)     node.opacity (get/set)
//   node.scrollFixed (get/set)   node.childCount     node.child(i) -> node
//   node.parent -> node|null           node.index    // position in parent
//   node.find(name) -> node|null       node.width / node.height  (read-only,
//                                      layout size in frame-local px)
//   node.primarySizing = "hug"|"fixed"   node.primaryAlign = "min"|"center"|
//                                        "max"|"spaceBetween"   (auto-layout,
//                                        both also readable)
// Also available:
//   setTimeout(fn, ms) / setInterval(fn, ms) -> id, clearTimeout/clearInterval
//     (driven by update(dt) — they tick in app time, pausing with the host)
//   fetch(url, {method, headers, body}?) -> Promise<{status, ok, text(), json()}>
//     (background thread; the promise settles on the next update(dt))
//   localStorage.getItem/setItem/removeItem/clear — string key/value store
//     persisted as JSON at the host-provided path (see setStoragePath);
//     without a path it works in memory only

#include <memory>
#include <string>

namespace figo {

class FigmaUI;

class ScriptHost {
public:
    explicit ScriptHost(FigmaUI& ui);
    ~ScriptHost();  // keep the host alive as long as the FigmaUI uses its handlers

    ScriptHost(const ScriptHost&) = delete;
    ScriptHost& operator=(const ScriptHost&) = delete;

    // Run a script file / source string. JS errors print to stderr -> false.
    bool runFile(const std::string& path);
    bool eval(const std::string& source, const std::string& filename = "<eval>");

    // Where localStorage persists (a small JSON file, created on first
    // setItem). Set it before runFile; figoplay uses "<script>.storage.json".
    void setStoragePath(const std::string& path);

    // Call once per host frame: fires ui.onUpdate handlers and drains the
    // JS job queue (promise reactions).
    void update(float dtSeconds);

    struct Impl;  // internal (public so the C binding functions can reach it)

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace figo
