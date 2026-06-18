// figmalib — trackpad pinch source: non-macOS stub (see pinch_gesture.h).
// On Apple the symbols come from pinch_macos.mm; this translation unit is
// empty there. Elsewhere there is no magnify gesture, so both calls are no-ops
// (Ctrl/⌘ + wheel still drives zoom).
#include "pinch_gesture.h"

#if !defined(__APPLE__)
namespace figmalib {
void pinchGestureInstall() {}
float pinchGestureConsume() { return 0.0f; }
}  // namespace figmalib
#endif
