#pragma once
// figmalib — trackpad pinch-zoom gesture source.
//
// macOS delivers two-finger pinch as NSEventMaskMagnify events, which GLFW
// (and therefore raylib) does not surface. This tiny shim installs a Cocoa
// event monitor and accumulates the magnification so the raylib backend can
// drive zoom. No-op on every other platform (the backend provides the stub).

namespace figmalib {

// Install the magnify monitor once. Safe to call every frame (idempotent).
// Must be called after the window/NSApp exists. No-op off macOS.
void pinchGestureInstall();

// Accumulated magnification delta since the last call (e.g. +0.03 per event,
// summed across all events that arrived). Positive = pinch out / zoom in.
// Resets to 0 on read. Returns 0 off macOS.
float pinchGestureConsume();

}  // namespace figmalib
