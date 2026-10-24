// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMMON_INPUT_MANAGER_INTERFACE_H_
#define COMMON_INPUT_MANAGER_INTERFACE_H_

#include <stdint.h>
#include <string>

#include <vector>

namespace arc {

// From android/system/core/include/utils/Timers.h
// Used in the input subsystem to store CLOCK_MONOTONIC timestamps.
typedef int64_t nsecs_t;

// Abstract interface for input routed via the Pepper plugin.
// We are intentionally avoiding using Pepper concepts (from the pp namespace)
// to maintain an abstraction layer between Android and Chrome/Pepper.
class PluginInputHandler {
 public:
  enum MouseButton {
    kMouseButtonLeft = 0,
    kMouseButtonMiddle,
    kMouseButtonRight
  };
  enum TouchType {
    kTouchStart = 0,
    kTouchMove,
    kTouchEnd
  };
  // See Chrome src/ppapi/c/pp_touch_point.h for details.
  struct TouchPoint {
    uint32_t id;
    float pos_x;
    float pos_y;
    float radius_x;
    float radius_y;
    float rotation_angle;
    float pressure;
  };

  virtual ~PluginInputHandler() {}
  virtual void OnMouseButton(nsecs_t now, MouseButton button, bool value) = 0;
  virtual void OnMouseMove(nsecs_t now, int32_t rel_x, int32_t rel_y,
                           int32_t abs_x, int32_t abs_y) = 0;
  virtual void OnKeyboardKey(nsecs_t now, uint32_t keycode,
                             const std::string& chartext, bool value) = 0;
  virtual void OnWheelMove(nsecs_t now, int32_t ticks_v, int32_t ticks_h) = 0;
  virtual void OnTouchEvent(nsecs_t now, TouchType type,
                            const std::vector<TouchPoint>& points) = 0;
};

class PluginFocusHandler {
 public:
  virtual void OnDidChangeFocus(bool has_focus) = 0;
};

class InputManagerInterface {
 public:
  virtual ~InputManagerInterface() {}

  // A quantization factor for the fractional mousewheel deltas.
  static const int kScrollWheelScaleFactor = 0x10000;

  // Sets a handler for input from the plugin
  virtual void SetInputHandler(PluginInputHandler* handler) = 0;

  // Sets handler for focus events from the plugin
  virtual void SetFocusHandler(PluginFocusHandler* handler) = 0;
};

}  // namespace arc
#endif  // COMMON_INPUT_MANAGER_INTERFACE_H_
