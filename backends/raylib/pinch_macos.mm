// figmalib — macOS trackpad pinch source (see pinch_gesture.h).
//
// raylib/GLFW pump the Cocoa run loop via glfwPollEvents(); a local event
// monitor installed here is invoked during that dispatch, so we can observe
// two-finger magnify gestures that GLFW otherwise drops on the floor.
#import <Cocoa/Cocoa.h>

#include "pinch_gesture.h"

namespace {
double g_accumMagnification = 0.0;
id g_monitor = nil;  // process-lifetime monitor; never removed
}  // namespace

namespace figmalib {

void pinchGestureInstall() {
    if (g_monitor) return;
    g_monitor = [NSEvent
        addLocalMonitorForEventsMatchingMask:NSEventMaskMagnify
                                     handler:^NSEvent *(NSEvent *event) {
                                         g_accumMagnification += [event magnification];
                                         return event;  // let it propagate normally
                                     }];
}

float pinchGestureConsume() {
    const float m = static_cast<float>(g_accumMagnification);
    g_accumMagnification = 0.0;
    return m;
}

}  // namespace figmalib
