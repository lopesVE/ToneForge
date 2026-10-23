#pragma once
#include "imgui.h"

class App;

namespace ui {
// hasSymbols: a symbol font (gear icon) was merged into both fonts.
void setFonts(ImFont* regular, ImFont* bold, bool hasSymbols);
void applyTheme();
// Draws the whole interface for one frame.
void draw(App& app);
// Command-line hooks for screenshots/tests:
// --dual --select=C:S:I --show=tuner,looper,audio,midi,interface,help
void applyDebugArgs(App& app, const char* cmdLine);
}  // namespace ui
