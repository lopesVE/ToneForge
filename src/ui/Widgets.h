// Custom ImGui widgets: knobs, LEDs, toggle buttons and level meters.
#pragma once
#include <string>

#include "engine/Module.h"
#include "imgui.h"

namespace ui {

ImU32 categoryColor(Category c);
ImU32 mixColor(ImU32 a, ImU32 b, float t);

// Rotary knob. `norm` is 0..1. Drag vertically (Shift = fine), mouse wheel,
// double-click resets to `defNorm`. Returns true while the value changes.
bool Knob(const char* id, const char* label, float* norm, float defNorm, const std::string& valueText, float radius,
          ImU32 color);

// Pill-shaped on/off button.
bool ToggleButton(const char* label, bool* v, const ImVec2& size, ImU32 onColor);

// Vertical peak meter; `level` is linear peak (0..1+).
void Meter(const char* id, float level, const ImVec2& size);

// Small LED; returns true when clicked.
bool Led(const char* id, bool on, ImU32 color, float radius);

// Text centered in the current content region.
void CenteredText(const char* text, ImU32 color = 0);

}  // namespace ui
