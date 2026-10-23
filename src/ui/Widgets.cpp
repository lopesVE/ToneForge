#include "ui/Widgets.h"

#include <algorithm>
#include <cmath>

#include "imgui_internal.h"

namespace ui {

ImU32 categoryColor(Category c) {
    switch (c) {
        case Category::Dynamics: return IM_COL32(46, 170, 160, 255);
        case Category::Drive: return IM_COL32(230, 120, 40, 255);
        case Category::Filter: return IM_COL32(220, 190, 50, 255);
        case Category::Modulation: return IM_COL32(150, 90, 210, 255);
        case Category::Delay: return IM_COL32(60, 130, 220, 255);
        case Category::Reverb: return IM_COL32(70, 180, 220, 255);
        case Category::Pitch: return IM_COL32(220, 80, 150, 255);
        case Category::EQ: return IM_COL32(90, 180, 80, 255);
        case Category::Utility: return IM_COL32(140, 140, 150, 255);
        case Category::Amp: return IM_COL32(200, 160, 70, 255);
        case Category::Cab: return IM_COL32(150, 105, 70, 255);
    }
    return IM_COL32(128, 128, 128, 255);
}

ImU32 mixColor(ImU32 a, ImU32 b, float t) {
    ImVec4 ca = ImGui::ColorConvertU32ToFloat4(a), cb = ImGui::ColorConvertU32ToFloat4(b);
    return ImGui::ColorConvertFloat4ToU32(ImVec4(ca.x + (cb.x - ca.x) * t, ca.y + (cb.y - ca.y) * t,
                                                 ca.z + (cb.z - ca.z) * t, ca.w + (cb.w - ca.w) * t));
}

bool Knob(const char* id, const char* label, float* norm, float defNorm, const std::string& valueText, float radius,
          ImU32 color) {
    ImGuiIO& io = ImGui::GetIO();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float lineH = ImGui::GetTextLineHeight();
    const float width = std::max(radius * 2.0f, ImGui::CalcTextSize(label).x) + 8.0f;
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const ImVec2 size(std::max(width, radius * 2.6f), radius * 2.0f + lineH * 2.0f + 8.0f);

    ImGui::InvisibleButton(id, size);
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
    bool changed = false;
    if (active && io.MouseDelta.y != 0.0f) {
        float speed = io.KeyShift ? 0.0008f : 0.005f;
        *norm = std::clamp(*norm - io.MouseDelta.y * speed, 0.0f, 1.0f);
        changed = true;
    }
    if (hovered && io.MouseWheel != 0.0f) {
        *norm = std::clamp(*norm + io.MouseWheel * (io.KeyShift ? 0.005f : 0.03f), 0.0f, 1.0f);
        changed = true;
    }
    if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        *norm = defNorm;
        changed = true;
    }

    const ImVec2 center(pos.x + size.x * 0.5f, pos.y + lineH + 4.0f + radius);
    const float a0 = IM_PI * 0.75f, a1 = IM_PI * 2.25f;
    const float av = a0 + (a1 - a0) * *norm;
    const ImU32 textCol = ImGui::GetColorU32(ImGuiCol_Text);

    // Label above.
    ImVec2 ls = ImGui::CalcTextSize(label);
    dl->AddText(ImVec2(center.x - ls.x * 0.5f, pos.y), textCol, label);
    // Body.
    dl->AddCircleFilled(center, radius, IM_COL32(28, 30, 36, 255), 40);
    dl->AddCircleFilled(center, radius * 0.72f, hovered || active ? IM_COL32(70, 74, 86, 255) : IM_COL32(56, 60, 70, 255), 40);
    // Track and value arc.
    dl->PathArcTo(center, radius * 0.88f, a0, a1, 40);
    dl->PathStroke(IM_COL32(60, 62, 70, 255), 0, radius * 0.16f);
    dl->PathArcTo(center, radius * 0.88f, a0, av, 40);
    dl->PathStroke(color, 0, radius * 0.16f);
    // Pointer.
    ImVec2 p1(center.x + std::cos(av) * radius * 0.25f, center.y + std::sin(av) * radius * 0.25f);
    ImVec2 p2(center.x + std::cos(av) * radius * 0.68f, center.y + std::sin(av) * radius * 0.68f);
    dl->AddLine(p1, p2, IM_COL32(235, 235, 240, 255), std::max(2.0f, radius * 0.1f));
    // Value below.
    ImVec2 vs = ImGui::CalcTextSize(valueText.c_str());
    dl->AddText(ImVec2(center.x - vs.x * 0.5f, center.y + radius + 4.0f),
                active ? color : ImGui::GetColorU32(ImGuiCol_TextDisabled), valueText.c_str());
    if (hovered && !active) ImGui::SetTooltip("%s: %s\nArraste (Shift = fino), roda do mouse.\nDuplo clique = padrão. Botão direito = MIDI.",
                                              label, valueText.c_str());
    return changed;
}

bool ToggleButton(const char* label, bool* v, const ImVec2& size, ImU32 onColor) {
    ImGui::PushStyleColor(ImGuiCol_Button, *v ? onColor : ImGui::GetColorU32(ImGuiCol_FrameBg));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, *v ? mixColor(onColor, IM_COL32_WHITE, 0.2f)
                                                     : ImGui::GetColorU32(ImGuiCol_FrameBgHovered));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, mixColor(onColor, IM_COL32_BLACK, 0.2f));
    ImGui::PushStyleColor(ImGuiCol_Text, *v ? IM_COL32(15, 15, 18, 255) : ImGui::GetColorU32(ImGuiCol_Text));
    bool pressed = ImGui::Button(label, size);
    ImGui::PopStyleColor(4);
    if (pressed) *v = !*v;
    return pressed;
}

void Meter(const char* id, float level, const ImVec2& size) {
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, size);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), IM_COL32(20, 22, 26, 255), 2.0f);
    float db = 20.0f * std::log10(std::max(level, 1e-6f));
    float t = std::clamp((db + 60.0f) / 60.0f, 0.0f, 1.0f);
    float top = pos.y + size.y * (1.0f - t);
    ImU32 col = db > -1.0f ? IM_COL32(230, 60, 50, 255) : db > -9.0f ? IM_COL32(230, 200, 60, 255) : IM_COL32(70, 200, 110, 255);
    dl->AddRectFilled(ImVec2(pos.x + 1, top), ImVec2(pos.x + size.x - 1, pos.y + size.y - 1), col, 1.0f);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%.1f dBFS", db);
}

bool Led(const char* id, bool on, ImU32 color, float radius) {
    ImVec2 pos = ImGui::GetCursorScreenPos();
    bool pressed = ImGui::InvisibleButton(id, ImVec2(radius * 2, radius * 2));
    ImVec2 c(pos.x + radius, pos.y + radius);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (on) dl->AddCircleFilled(c, radius * 1.6f, (color & 0x00FFFFFF) | 0x40000000, 20);
    dl->AddCircleFilled(c, radius, on ? color : IM_COL32(50, 30, 30, 255), 20);
    dl->AddCircle(c, radius, IM_COL32(0, 0, 0, 160), 20, 1.0f);
    return pressed;
}

void CenteredText(const char* text, ImU32 color) {
    float w = ImGui::CalcTextSize(text).x;
    float avail = ImGui::GetContentRegionAvail().x;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, (avail - w) * 0.5f));
    if (color) ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(color), "%s", text);
    else ImGui::TextUnformatted(text);
}

}  // namespace ui
