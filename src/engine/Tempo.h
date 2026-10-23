// Global tempo shared by tempo-synced effects (set from the UI / tap tempo).
#pragma once
#include <atomic>

extern std::atomic<float> g_tempoBpm;

// Note divisions offered by synced effects, as multiples of a quarter note.
inline const char* const kDivisionNames[] = {"1/2", "1/4.", "1/4", "1/8.", "1/8", "1/8T", "1/16"};
inline constexpr float kDivisionBeats[] = {2.0f, 1.5f, 1.0f, 0.75f, 0.5f, 1.0f / 3.0f, 0.25f};
