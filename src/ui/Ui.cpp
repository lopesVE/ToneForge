#include "ui/Ui.h"

#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "app/App.h"
#include "engine/Tempo.h"
#include "modules/Cab.h"
#include "ui/Widgets.h"
#include "util/Paths.h"

namespace ui {
namespace {

ImFont* g_regular = nullptr;
ImFont* g_bold = nullptr;
bool g_symbols = false;  // Segoe UI Symbol merged in (gear icon etc.)

constexpr int kNone = -2, kGate = -1;
struct Selection {
    int chain = 0, section = kNone, slot = 0;
};

struct UiState {
    Selection sel;
    bool showLooper = false, showSettings = false, showHelp = false;
    int settingsTab = 0;      // 0 = Áudio, 1 = MIDI, 2 = Interface
    bool selectTab = false;   // force settingsTab on the next frame
    float topWidths[8] = {};  // top bar group widths from the previous frame
    bool openSaveAs = false, openDelete = false;
    char saveName[128] = {};
    float inLevel = 0, outL = 0, outR = 0;
    AudioConfig audioEdit;
    bool audioEditInit = false;
    int addChain = 0, addSection = 0;
    bool firstRunHint = false;
} g;

const ImU32 kAccent = IM_COL32(240, 160, 50, 255);
const ImU32 kDim = IM_COL32(150, 150, 160, 255);

float em() { return ImGui::GetFontSize() / 16.0f; }

// First-use placement for floating windows, relative to the main viewport.
// fx/fy pick the anchor corner (0 = left/top, 1 = right/bottom).
void placeWindow(float w, float h, float fx, float fy, float marginY = 0) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float e = em(), pad = 24 * e;
    ImVec2 pos(vp->WorkPos.x + pad + (vp->WorkSize.x - w * e - 2 * pad) * fx,
               vp->WorkPos.y + pad + marginY * e + (vp->WorkSize.y - h * e - 2 * pad - marginY * e) * fy);
    ImGui::SetNextWindowPos(pos, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(w * e, h * e), ImGuiCond_FirstUseEver);
}

void boldText(const char* text, float scale = 1.0f) {
    ImGui::PushFont(g_bold, ImGui::GetStyle().FontSizeBase * scale);
    ImGui::TextUnformatted(text);
    ImGui::PopFont();
}

Module* selectedModule(App& app) {
    const Selection& s = g.sel;
    if (s.section == kGate) return app.rig.gate();
    if (s.section < 0 || s.section > 3) return nullptr;
    return app.rig.chain(s.chain).get(Section(s.section), s.slot);
}

MidiTarget targetForSelection(int paramIdx, Module* m, bool bypass) {
    MidiTarget t;
    t.kind = bypass ? MidiTarget::Bypass : MidiTarget::Param;
    t.chain = g.sel.chain;
    t.section = g.sel.section == kGate ? -1 : g.sel.section;
    t.slot = g.sel.slot;
    if (!bypass && m) t.paramId = m->desc(paramIdx).id;
    return t;
}

MidiTarget globalTarget(int id) {
    MidiTarget t;
    t.kind = MidiTarget::Global;
    t.global = id;
    return t;
}

// Right-click menu for MIDI learn on the last item.
void midiContextMenu(App& app, const MidiTarget& t, const char* popupId = "midi") {
    if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) ImGui::OpenPopup(popupId);
    if (ImGui::BeginPopup(popupId)) {
        int cc = app.mappedCc(t);
        if (cc >= 0) ImGui::TextDisabled("Mapeado no MIDI CC %d", cc);
        else ImGui::TextDisabled("Sem mapeamento MIDI");
        if (ImGui::MenuItem("Aprender MIDI (mova um controle)")) app.startLearn(t);
        if (cc >= 0 && ImGui::MenuItem("Remover mapeamento")) app.clearMapping(t);
        ImGui::EndPopup();
    }
}

std::string describeTarget(App& app, const MidiTarget& t) {
    if (t.kind == MidiTarget::Global) return globalTargetName(t.global);
    std::string where = t.section < 0 ? "Gate" : std::string(t.chain ? "B" : "A") + " · " + sectionName(Section(t.section)) +
                                                     (t.section == 0 || t.section == 3 ? " #" + std::to_string(t.slot + 1) : "");
    Module* m = app.resolve(t);
    if (m) where += " (" + m->info().name + ")";
    if (t.kind == MidiTarget::Bypass) return where + " · Liga/Desliga";
    std::string pname = t.paramId;
    if (m) {
        int idx = m->info().paramIndex(t.paramId);
        if (idx >= 0) pname = m->desc(idx).name;
    }
    return where + " · " + pname;
}

std::string openAudioFileDialog() {
    wchar_t file[MAX_PATH] = {};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = GetActiveWindow();
    ofn.lpstrFilter = L"Impulse responses (*.wav;*.aif;*.aiff;*.flac)\0*.wav;*.aif;*.aiff;*.flac\0Todos os arquivos\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L"Escolha um arquivo de IR de gabinete";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return {};
    return wideToUtf8(file);
}

// ------------------------------------------------------------- Top bar ---

void openSettings(int tab) {
    g.showSettings = true;
    g.settingsTab = tab;
    g.selectTab = true;
}

// Lays out widget groups left to right and wraps to a new line when the next
// group would not fit. Widths are measured on the previous frame.
struct Flow {
    float* widths;
    float spacing;
    int idx = 0;
    void begin(bool alignRight = false) {
        if (idx > 0) {
            const float limit = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
            if (ImGui::GetItemRectMax().x + spacing + widths[idx] <= limit) {
                if (alignRight) ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - widths[idx]);
                else ImGui::SameLine(0, spacing);
            }
        }
        ImGui::BeginGroup();
    }
    void end() {
        ImGui::EndGroup();
        widths[idx++] = ImGui::GetItemRectSize().x;
    }
};

void requestSave(App& app) {
    if (app.currentIsFactory() || app.currentPreset < 0) {
        g.openSaveAs = true;
        std::snprintf(g.saveName, sizeof g.saveName, "%s", app.currentName.c_str());
    } else {
        app.saveCurrent();
    }
}

void drawTopBar(App& app) {
    const float e = em();
    Flow flow{g.topWidths, 20 * e};

    // Logo, settings and help: always the first thing on screen.
    flow.begin();
    ImGui::AlignTextToFramePadding();
    ImGui::PushFont(g_bold, ImGui::GetStyle().FontSizeBase * 1.35f);
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kAccent), "ToneForge");
    ImGui::PopFont();
    ImGui::SameLine(0, 14 * e);
    ToggleButton(g_symbols ? "⚙ Configurações###settings" : "Configurações###settings", &g.showSettings, ImVec2(0, 0), kAccent);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Áudio, MIDI e interface (Ctrl+,)");
    ImGui::SameLine();
    ToggleButton("?", &g.showHelp, ImVec2(0, 0), IM_COL32(200, 200, 210, 255));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Ajuda e atalhos");
    flow.end();

    // Preset selector.
    flow.begin();
    if (ImGui::ArrowButton("##prev", ImGuiDir_Left)) app.stepPreset(-1);
    ImGui::SameLine();
    std::string preview = app.currentName + (app.dirty() ? " *" : "");
    ImGui::SetNextItemWidth(230 * e);
    if (ImGui::BeginCombo("##preset", preview.c_str(), ImGuiComboFlags_HeightLargest)) {
        bool userHeader = false;
        ImGui::TextDisabled("Fábrica");
        for (int i = 0; i < int(app.presets.size()); ++i) {
            const auto& p = app.presets[size_t(i)];
            if (!p.factory && !userHeader) {
                ImGui::Separator();
                ImGui::TextDisabled("Meus presets");
                userHeader = true;
            }
            if (ImGui::Selectable((p.name + "##" + std::to_string(i)).c_str(), i == app.currentPreset)) app.loadPreset(i);
        }
        if (!userHeader) {
            ImGui::Separator();
            ImGui::TextDisabled("Meus presets: nenhum ainda (use Salvar como)");
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::ArrowButton("##next", ImGuiDir_Right)) app.stepPreset(1);
    ImGui::SameLine();
    if (ImGui::Button("Salvar")) requestSave(app);
    ImGui::SameLine();
    if (ImGui::Button("Salvar como...")) {
        g.openSaveAs = true;
        std::snprintf(g.saveName, sizeof g.saveName, "%s", app.currentName.c_str());
    }
    ImGui::SameLine();
    if (ImGui::Button("Novo")) {
        app.newPreset();
        g.sel = Selection{};
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(app.currentIsFactory() || app.currentPreset < 0);
    if (ImGui::Button("Excluir")) g.openDelete = true;
    ImGui::EndDisabled();
    flow.end();

    // Tempo.
    flow.begin();
    float bpm = g_tempoBpm.load();
    ImGui::SetNextItemWidth(80 * e);
    if (ImGui::DragFloat("##bpm", &bpm, 0.2f, 30, 300, "%.1f BPM")) g_tempoBpm.store(bpm);
    ImGui::SameLine();
    if (ImGui::Button("TAP")) app.tapTempo();
    midiContextMenu(app, globalTarget(MidiTarget::TapTempo), "midi_tap");
    flow.end();

    // Tools.
    flow.begin();
    bool tuner = app.tunerOpen();
    if (ToggleButton("Afinador", &tuner, ImVec2(0, 0), IM_COL32(90, 200, 120, 255))) app.setTunerOpen(tuner);
    midiContextMenu(app, globalTarget(MidiTarget::Tuner), "midi_tuner");
    ImGui::SameLine();
    ToggleButton("Looper", &g.showLooper, ImVec2(0, 0), IM_COL32(90, 160, 230, 255));
    ImGui::SameLine();
    bool rec = app.recording();
    char recLabel[48];
    if (rec) std::snprintf(recLabel, sizeof recLabel, "● REC %02d:%02d###rec", int(app.recordedSeconds()) / 60, int(app.recordedSeconds()) % 60);
    else std::snprintf(recLabel, sizeof recLabel, "● Gravar###rec");
    if (ToggleButton(recLabel, &rec, ImVec2(0, 0), IM_COL32(220, 60, 60, 255))) app.toggleRecording();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Grava a saída em WAV na pasta Documentos\\ToneForge\\Gravações");
    flow.end();

    // Meters, right-aligned when there is room.
    g.inLevel = std::max(app.rig.inPeak.exchange(0), g.inLevel * 0.9f);
    g.outL = std::max(app.rig.outPeakL.exchange(0), g.outL * 0.9f);
    g.outR = std::max(app.rig.outPeakR.exchange(0), g.outR * 0.9f);
    const float mh = ImGui::GetFrameHeight(), mw = 7 * e;
    flow.begin(true);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("IN");
    ImGui::SameLine();
    Meter("##min", g.inLevel, ImVec2(mw, mh));
    ImGui::SameLine(0, 10 * e);
    ImGui::TextDisabled("OUT");
    ImGui::SameLine();
    Meter("##moutl", g.outL, ImVec2(mw, mh));
    ImGui::SameLine(0, 2);
    Meter("##moutr", g.outR, ImVec2(mw, mh));
    flow.end();
}

// --------------------------------------------------------------- Chain ---

std::string blockTitle(Module& m) {
    if (m.info().category == Category::Amp) return m.desc(0).format(m.get(0));
    if (m.info().category == Category::Cab) {
        auto& cab = static_cast<Cab&>(m);
        if (cab.isIrModel()) return cab.irName().empty() ? "IR (nenhuma)" : cab.irName();
        return m.desc(0).format(m.get(0));
    }
    return m.info().name;
}

std::string blockSubtitle(Module& m) {
    if (m.info().category == Category::Amp) return "Amplificador";
    if (m.info().category == Category::Cab) return "Mic " + m.desc(Cab::kMic).format(m.get(Cab::kMic));
    return categoryName(m.info().category);
}

struct DragPayload {
    int chain, section, index;
};

// Draws one block; returns true when the body was clicked.
bool block(const char* id, const std::string& title, const std::string& subtitle, ImU32 color, bool bypassed,
           bool selected, bool* ledClicked, bool empty = false) {
    const float e = em();
    const ImVec2 size(124 * e, 70 * e);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, size);
    const bool hovered = ImGui::IsItemHovered();
    bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 end(pos.x + size.x, pos.y + size.y);
    const float r = 8 * e;
    const ImVec2 led(end.x - 12 * e, pos.y + 12 * e);
    if (empty) {
        dl->AddRect(pos, end, hovered ? IM_COL32(200, 200, 200, 200) : IM_COL32(110, 110, 120, 160), r, 0, 1.5f);
    } else {
        ImU32 fill = bypassed ? mixColor(color, IM_COL32(25, 26, 30, 255), 0.82f) : mixColor(color, IM_COL32(25, 26, 30, 255), 0.45f);
        dl->AddRectFilled(pos, end, fill, r);
        dl->AddRectFilled(pos, ImVec2(end.x, pos.y + 5 * e), bypassed ? mixColor(color, IM_COL32_BLACK, 0.6f) : color, r,
                          ImDrawFlags_RoundCornersTop);
        ImU32 border = selected ? IM_COL32(255, 255, 255, 255) : hovered ? mixColor(color, IM_COL32_WHITE, 0.3f) : color;
        dl->AddRect(pos, end, border, r, 0, selected ? 2.5f : 1.2f);
        // LED.
        bool ledHover = hovered && std::hypot(ImGui::GetIO().MousePos.x - led.x, ImGui::GetIO().MousePos.y - led.y) < 9 * e;
        dl->AddCircleFilled(led, 5 * e, bypassed ? IM_COL32(60, 30, 30, 255) : IM_COL32(255, 70, 50, 255), 16);
        if (!bypassed) dl->AddCircleFilled(led, 9 * e, IM_COL32(255, 70, 50, 50), 16);
        if (ledHover) dl->AddCircle(led, 7 * e, IM_COL32_WHITE, 16, 1.0f);
        if (clicked && ledHover) {
            *ledClicked = true;
            clicked = false;
        }
    }
    ImGui::PushClipRect(ImVec2(pos.x + 6 * e, pos.y), ImVec2(end.x - (empty ? 6 : 22) * e, end.y), true);
    ImFont* f = g_bold ? g_bold : ImGui::GetFont();
    const float fs = ImGui::GetFontSize();
    dl->AddText(f, fs, ImVec2(pos.x + 8 * e, pos.y + 14 * e), bypassed ? kDim : IM_COL32(245, 245, 245, 255), title.c_str(),
                nullptr, size.x - 26 * e);
    ImGui::PopClipRect();
    dl->AddText(ImVec2(pos.x + 8 * e, end.y - fs - 6 * e), kDim, subtitle.c_str());
    return clicked;
}

void arrow() {
    const float e = em();
    ImGui::SameLine(0, 0);
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(16 * e, 70 * e));
    float y = p.y + 35 * e;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddLine(ImVec2(p.x + 2 * e, y), ImVec2(p.x + 14 * e, y), IM_COL32(120, 120, 130, 255), 2);
    dl->AddTriangleFilled(ImVec2(p.x + 14 * e, y - 4 * e), ImVec2(p.x + 14 * e, y + 4 * e), ImVec2(p.x + 18 * e, y),
                          IM_COL32(120, 120, 130, 255));
    ImGui::SameLine(0, 0);
}

void addMenuItems(App& app, int chain, Section s, int index, bool replace) {
    for (int c = 0; c <= int(Category::Utility); ++c) {
        Category cat = Category(c);
        if (!ImGui::BeginMenu(categoryName(cat))) continue;
        for (auto& t : moduleTypes()) {
            if (t.info->category != cat) continue;
            if (ImGui::MenuItem(t.info->name.c_str())) {
                if (replace) {
                    app.rig.remove(chain, s, index);
                    app.rig.insert(chain, s, index, t.create());
                } else {
                    auto* l = app.rig.chain(chain).list(s);
                    index = l ? int(l->size()) : 0;
                    if (!app.rig.insert(chain, s, index, t.create())) app.notify("Limite de blocos nesta seção.");
                }
                g.sel = Selection{chain, int(s), index};
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", t.info->description.c_str());
        }
        ImGui::EndMenu();
    }
}

void listBlocks(App& app, int ci, Section s) {
    Chain& ch = app.rig.chain(ci);
    auto& list = *ch.list(s);
    for (int i = 0; i < int(list.size()); ++i) {
        Module& m = *list[size_t(i)];
        ImGui::PushID(i);
        bool led = false;
        bool sel = g.sel.chain == ci && g.sel.section == int(s) && g.sel.slot == i;
        if (block("##blk", blockTitle(m), blockSubtitle(m), categoryColor(m.info().category), m.bypassed.load(), sel, &led))
            g.sel = Selection{ci, int(s), i};
        if (led) m.bypassed.store(!m.bypassed.load());
        if (ImGui::BeginDragDropSource()) {
            DragPayload p{ci, int(s), i};
            ImGui::SetDragDropPayload("TF_BLOCK", &p, sizeof p);
            ImGui::Text("Mover %s", m.info().name.c_str());
            ImGui::EndDragDropSource();
        }
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("TF_BLOCK")) {
                DragPayload p;
                std::memcpy(&p, pl->Data, sizeof p);
                if (p.chain == ci && p.section == int(s)) {
                    app.rig.move(ci, s, p.index, i);
                    g.sel = Selection{ci, int(s), i};
                }
            }
            ImGui::EndDragDropTarget();
        }
        if (ImGui::BeginPopupContextItem("ctx")) {
            if (ImGui::MenuItem(m.bypassed.load() ? "Ligar" : "Desligar (bypass)")) m.bypassed.store(!m.bypassed.load());
            if (ImGui::MenuItem("Mover para a esquerda", nullptr, false, i > 0)) app.rig.move(ci, s, i, i - 1);
            if (ImGui::MenuItem("Mover para a direita", nullptr, false, i + 1 < int(list.size()))) app.rig.move(ci, s, i, i + 1);
            if (ImGui::BeginMenu("Trocar por")) {
                addMenuItems(app, ci, s, i, true);
                ImGui::EndMenu();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Remover")) {
                app.rig.remove(ci, s, i);
                g.sel = Selection{};
                ImGui::EndPopup();
                ImGui::PopID();
                return;  // the list changed under us
            }
            ImGui::EndPopup();
        }
        ImGui::PopID();
        arrow();
    }
    // Add button.
    ImGui::PushID(int(s) + 100);
    const float e = em();
    int limit = s == Section::Stomp ? Rig::kMaxStomps : Rig::kMaxRack;
    ImGui::BeginDisabled(int(list.size()) >= limit);
    if (ImGui::Button("+", ImVec2(30 * e, 70 * e))) ImGui::OpenPopup("add");
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip(s == Section::Stomp ? "Adicionar pedal (antes do amp)" : "Adicionar efeito de rack (depois do gabinete)");
    if (ImGui::BeginPopup("add")) {
        ImGui::TextDisabled(s == Section::Stomp ? "Adicionar pedal" : "Adicionar ao rack");
        addMenuItems(app, ci, s, 0, false);
        ImGui::EndPopup();
    }
    ImGui::PopID();
    arrow();
}

void slotBlock(App& app, int ci, Section s) {
    Chain& ch = app.rig.chain(ci);
    ImGui::PushID(int(s) + 200);
    Module* m = ch.slot(s)->get();
    bool sel = g.sel.chain == ci && g.sel.section == int(s);
    bool led = false;
    if (!m) {
        if (block("##slot", s == Section::Amp ? "+ Amplificador" : "+ Gabinete", "vazio", 0, false, false, &led, true)) {
            app.rig.setSlot(ci, s, createModule(s == Section::Amp ? "amp" : "cab"));
            g.sel = Selection{ci, int(s), 0};
        }
    } else {
        if (block("##slot", blockTitle(*m), blockSubtitle(*m), categoryColor(m->info().category), m->bypassed.load(), sel, &led))
            g.sel = Selection{ci, int(s), 0};
        if (led) m->bypassed.store(!m->bypassed.load());
        if (ImGui::BeginPopupContextItem("ctx")) {
            if (ImGui::MenuItem(m->bypassed.load() ? "Ligar" : "Desligar (bypass)")) m->bypassed.store(!m->bypassed.load());
            if (ImGui::MenuItem("Remover")) {
                app.rig.setSlot(ci, s, nullptr);
                g.sel = Selection{};
            }
            ImGui::EndPopup();
        }
    }
    ImGui::PopID();
    arrow();
}

void chainRow(App& app, int ci) {
    const float e = em();
    Chain& ch = app.rig.chain(ci);
    ImGui::PushID(ci);
    ImGui::BeginGroup();
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(22 * e, 70 * e));
    ImGui::GetWindowDrawList()->AddText(g_bold, ImGui::GetFontSize() * 1.4f, ImVec2(p.x + 3 * e, p.y + 22 * e), kAccent,
                                        ci == 0 ? "A" : "B");
    ImGui::SameLine(0, 4 * e);
    listBlocks(app, ci, Section::Stomp);
    slotBlock(app, ci, Section::Amp);
    slotBlock(app, ci, Section::Cab);
    listBlocks(app, ci, Section::Rack);

    // Chain level and pan.
    ImGui::SameLine(0, 10 * e);
    float lvl = ch.levelDb.load(), pan = ch.pan.load();
    float ln = (lvl + 40) / 52, pn = (pan + 1) / 2;
    char buf[32];
    std::snprintf(buf, sizeof buf, "%+.1f dB", lvl);
    if (Knob("##lvl", "Nível", &ln, 40.0f / 52, buf, 15 * e, kAccent)) ch.levelDb.store(ln * 52 - 40);
    ImGui::SameLine();
    std::snprintf(buf, sizeof buf, pan == 0 ? "C" : pan < 0 ? "L%.0f" : "R%.0f", std::fabs(pan) * 100);
    if (Knob("##pan", "Pan", &pn, 0.5f, buf, 15 * e, kAccent)) ch.pan.store(std::round((pn * 2 - 1) * 100) / 100);
    ImGui::EndGroup();
    ImGui::PopID();
}

void drawChains(App& app) {
    const float e = em();
    // Input column: input gain + gate.
    ImGui::BeginGroup();
    float in = app.rig.inputDb.load();
    float inN = (in + 24) / 48;
    char buf[32];
    std::snprintf(buf, sizeof buf, "%+.1f dB", in);
    if (Knob("##input", "Entrada", &inN, 0.5f, buf, 17 * e, IM_COL32(120, 200, 140, 255))) app.rig.inputDb.store(inN * 48 - 24);
    midiContextMenu(app, globalTarget(MidiTarget::Input), "midi_in");
    ImGui::EndGroup();
    ImGui::SameLine(0, 8 * e);
    {
        Module* gate = app.rig.gate();
        bool led = false;
        if (gate) {
            if (block("##gate", "Noise Gate", "Entrada", categoryColor(Category::Dynamics), gate->bypassed.load(),
                      g.sel.section == kGate, &led))
                g.sel = Selection{0, kGate, 0};
            if (led) gate->bypassed.store(!gate->bypassed.load());
        }
    }
    arrow();

    // Chains.
    ImGui::BeginGroup();
    chainRow(app, 0);
    bool dual = app.rig.dual.load();
    if (dual) chainRow(app, 1);
    ImGui::EndGroup();

    // Output column.
    ImGui::SameLine(0, 10 * e);
    ImGui::BeginGroup();
    float master = app.rig.masterDb.load();
    float mn = (master + 60) / 72;
    std::snprintf(buf, sizeof buf, "%+.1f dB", master);
    if (Knob("##master", "Master", &mn, 60.0f / 72, buf, 17 * e, kAccent)) app.rig.masterDb.store(mn * 72 - 60);
    midiContextMenu(app, globalTarget(MidiTarget::Master), "midi_master");
    ImGui::EndGroup();

    // Dual controls.
    if (ToggleButton(dual ? "Cadeia dupla: ligada" : "Cadeia dupla: desligada", &dual, ImVec2(0, 0), IM_COL32(240, 160, 50, 255)))
        app.rig.dual.store(dual);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Duas cadeias completas em paralelo (A + B), como dois amps microfonados.");
    midiContextMenu(app, globalTarget(MidiTarget::DualToggle), "midi_dual");
    if (dual) {
        ImGui::SameLine();
        if (ImGui::Button("Copiar A → B")) app.rig.copyChain(0, 1);
        ImGui::SameLine();
        if (ImGui::Button("Copiar B → A")) app.rig.copyChain(1, 0);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Arraste blocos para reordenar · clique no LED para ligar/desligar · botão direito para mais opções");
}

// -------------------------------------------------------------- Editor ---

void drawParam(App& app, Module& m, int i, float& x, float avail) {
    const float e = em();
    const ParamDesc& d = m.desc(i);
    const MidiTarget t = targetForSelection(i, &m, false);
    const int cc = app.mappedCc(t);
    float w = d.isChoice() ? 170 * e : d.toggle ? 100 * e : 78 * e;
    if (x > 0 && x + w > avail) x = 0;
    else if (x > 0) ImGui::SameLine(0, 12 * e);
    x += w + 12 * e;
    ImGui::PushID(i);
    ImU32 color = categoryColor(m.info().category);
    if (d.isChoice()) {
        ImGui::BeginGroup();
        ImGui::TextUnformatted(d.name.c_str());
        ImGui::SetNextItemWidth(w);
        int cur = m.getInt(i);
        if (ImGui::BeginCombo("##c", d.choices[size_t(std::clamp(cur, 0, int(d.choices.size()) - 1))].c_str(),
                              ImGuiComboFlags_HeightLarge)) {
            for (int k = 0; k < int(d.choices.size()); ++k)
                if (ImGui::Selectable(d.choices[size_t(k)].c_str(), k == cur)) m.set(i, float(k));
            ImGui::EndCombo();
        }
        if (cc >= 0) ImGui::TextDisabled("CC %d", cc);
        ImGui::EndGroup();
    } else if (d.toggle) {
        ImGui::BeginGroup();
        ImGui::Dummy(ImVec2(0, ImGui::GetTextLineHeight()));
        bool v = m.getBool(i);
        if (ToggleButton(d.name.c_str(), &v, ImVec2(w, 0), color)) m.set(i, v ? 1.0f : 0.0f);
        if (cc >= 0) ImGui::TextDisabled("CC %d", cc);
        ImGui::EndGroup();
    } else {
        float n = m.getNorm(i);
        std::string val = d.format(m.get(i));
        if (cc >= 0) val += " · CC" + std::to_string(cc);
        if (Knob("##k", d.name.c_str(), &n, d.toNorm(d.def), val, 22 * e, color)) m.setNorm(i, n);
    }
    midiContextMenu(app, t);
    ImGui::PopID();
}

void drawEditor(App& app) {
    const float e = em();
    Module* m = selectedModule(app);
    if (!m) {
        g.sel = Selection{};
        ImGui::Dummy(ImVec2(0, 10 * e));
        CenteredText("Clique num bloco da cadeia para editar seus controles.", kDim);
        CenteredText("Use os botões + para adicionar pedais (antes do amp) e efeitos de rack (depois do gabinete).", kDim);
        CenteredText("Botão direito em qualquer knob para mapear um controlador MIDI.", kDim);
        return;
    }
    // Header.
    ImGui::PushStyleColor(ImGuiCol_Text, categoryColor(m->info().category));
    boldText(m->info().name.c_str(), 1.3f);
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::TextDisabled("  %s", g.sel.section == kGate ? "Entrada" :
                        (std::string(g.sel.chain ? "Cadeia B · " : "Cadeia A · ") + sectionName(Section(g.sel.section))).c_str());
    ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - 210 * e);
    bool on = !m->bypassed.load();
    if (ToggleButton(on ? "Ligado" : "Desligado", &on, ImVec2(100 * e, 0), IM_COL32(255, 90, 60, 255))) m->bypassed.store(!on);
    midiContextMenu(app, targetForSelection(0, m, true), "midi_bypass");
    if (g.sel.section != kGate) {
        ImGui::SameLine();
        if (ImGui::Button("Remover", ImVec2(90 * e, 0))) {
            if (g.sel.section == int(Section::Amp) || g.sel.section == int(Section::Cab))
                app.rig.setSlot(g.sel.chain, Section(g.sel.section), nullptr);
            else
                app.rig.remove(g.sel.chain, Section(g.sel.section), g.sel.slot);
            g.sel = Selection{};
            return;
        }
    }
    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    ImGui::TextWrapped("%s", m->info().description.c_str());
    ImGui::PopStyleColor();

    // Cab: IR loader.
    if (m->info().category == Category::Cab) {
        auto* cab = static_cast<Cab*>(m);
        if (ImGui::Button("Carregar IR (.wav)...")) {
            std::string path = openAudioFileDialog();
            if (!path.empty()) {
                std::string err;
                if (cab->loadIr(path, err)) {
                    cab->set(Cab::kModel, float(Cab::numBuiltinModels()));
                    app.notify("IR carregada: " + cab->irName());
                } else {
                    app.notify(err, 5.0f);
                }
            }
        }
        ImGui::SameLine();
        if (!cab->irName().empty()) ImGui::TextDisabled("IR atual: %s", cab->irName().c_str());
        else ImGui::TextDisabled("Sem IR carregada: os modelos internos simulam alto-falante + microfone.");
    }
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, 4 * e));

    const float avail = ImGui::GetContentRegionAvail().x;
    float x = 0;
    for (int i = 0; i < m->numParams(); ++i) drawParam(app, *m, i, x, avail);
}

// ------------------------------------------------------------ Windows ---

void drawTuner(App& app) {
    if (!app.tunerOpen()) return;
    const float e = em();
    placeWindow(420, 250, 1.0f, 0.0f, 290);
    bool open = true;
    if (ImGui::Begin("Afinador", &open, ImGuiWindowFlags_NoCollapse)) {
        const PitchResult& p = app.pitch;
        const NoteInfo& n = app.note;
        const bool has = p.freq > 0;
        const bool inTune = has && std::fabs(n.cents) < 3.0f;
        ImU32 col = !has ? kDim : inTune ? IM_COL32(80, 230, 120, 255) : std::fabs(n.cents) < 15 ? IM_COL32(240, 200, 60, 255)
                                                                                                  : IM_COL32(240, 90, 60, 255);
        ImGui::PushFont(g_bold, ImGui::GetStyle().FontSizeBase * 4.0f);
        std::string big = has ? n.name : "--";
        CenteredText(big.c_str(), col);
        ImGui::PopFont();
        char info[64];
        if (has) std::snprintf(info, sizeof info, "%s%d   %.2f Hz   %+.1f cents", n.name.c_str(), n.octave, p.freq, n.cents);
        else std::snprintf(info, sizeof info, "Toque uma corda");
        CenteredText(info, kDim);

        // Cents scale.
        ImVec2 pos = ImGui::GetCursorScreenPos();
        float w = ImGui::GetContentRegionAvail().x, h = 46 * e;
        ImGui::Dummy(ImVec2(w, h));
        ImDrawList* dl = ImGui::GetWindowDrawList();
        float cx = pos.x + w * 0.5f, y = pos.y + h * 0.5f;
        dl->AddRectFilled(ImVec2(cx - w * 0.03f, pos.y + 4), ImVec2(cx + w * 0.03f, pos.y + h - 4), IM_COL32(40, 90, 50, 255), 3);
        for (int c = -50; c <= 50; c += 10) {
            float x = cx + c / 50.0f * w * 0.45f;
            float th = c == 0 ? h * 0.45f : h * 0.25f;
            dl->AddLine(ImVec2(x, y - th), ImVec2(x, y + th), IM_COL32(120, 120, 130, 255), c == 0 ? 2.0f : 1.0f);
        }
        if (has) {
            float x = cx + std::clamp(n.cents, -50.0f, 50.0f) / 50.0f * w * 0.45f;
            dl->AddTriangleFilled(ImVec2(x - 8 * e, pos.y), ImVec2(x + 8 * e, pos.y), ImVec2(x, y), col);
            dl->AddLine(ImVec2(x, pos.y), ImVec2(x, pos.y + h), col, 3.0f);
        }
        ImGui::Dummy(ImVec2(0, 6 * e));
        ImGui::SetNextItemWidth(120 * e);
        ImGui::DragFloat("Referência A4", &app.settings.a4, 0.1f, 425, 455, "%.1f Hz");
        ImGui::SameLine();
        bool mute = app.settings.tunerMutes;
        if (ImGui::Checkbox("Silenciar saída", &mute)) {
            app.settings.tunerMutes = mute;
            app.rig.muted.store(mute);
        }
    }
    ImGui::End();
    if (!open) app.setTunerOpen(false);
}

void drawLooper(App& app) {
    if (!g.showLooper) return;
    const float e = em();
    placeWindow(460, 230, 1.0f, 1.0f);
    if (ImGui::Begin("Looper", &g.showLooper, ImGuiWindowFlags_NoCollapse)) {
        Looper& lp = app.rig.looper;
        Looper::State st = lp.state();
        static const char* names[] = {"Vazio", "Gravando", "Tocando", "Overdub", "Parado"};
        static const ImU32 cols[] = {IM_COL32(150, 150, 160, 255), IM_COL32(240, 70, 60, 255), IM_COL32(80, 220, 120, 255),
                                     IM_COL32(240, 160, 50, 255), IM_COL32(120, 160, 230, 255)};
        ImGui::PushFont(g_bold, ImGui::GetStyle().FontSizeBase * 1.6f);
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(cols[int(st)]), "%s", names[int(st)]);
        ImGui::PopFont();
        char overlay[64];
        if (st == Looper::State::Recording) std::snprintf(overlay, sizeof overlay, "%.1f s", lp.recordedSeconds());
        else std::snprintf(overlay, sizeof overlay, "%.1f / %.1f s", lp.progress() * lp.lengthSeconds(), lp.lengthSeconds());
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, cols[int(st)]);
        ImGui::ProgressBar(st == Looper::State::Recording ? 1.0f : lp.progress(), ImVec2(-1, 18 * e), overlay);
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0, 4 * e));
        const ImVec2 bs(100 * e, 48 * e);
        const char* recLabel = st == Looper::State::Empty ? "REC" : st == Looper::State::Recording ? "FECHAR" :
                               st == Looper::State::Overdubbing ? "PARAR DUB" : "DUB";
        ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(150, 40, 35, 255));
        if (ImGui::Button(recLabel, bs)) lp.command(Looper::kRecDub);
        ImGui::PopStyleColor();
        midiContextMenu(app, globalTarget(MidiTarget::LooperRec), "midi_lrec");
        ImGui::SameLine();
        if (ImGui::Button(st == Looper::State::Playing || st == Looper::State::Overdubbing ? "STOP" : "PLAY", bs))
            lp.command(Looper::kPlayStop);
        midiContextMenu(app, globalTarget(MidiTarget::LooperPlay), "midi_lplay");
        ImGui::SameLine();
        ImGui::BeginDisabled(!lp.hasUndo());
        if (ImGui::Button(lp.undone() ? "REFAZER" : "DESFAZER", bs)) lp.command(Looper::kUndo);
        ImGui::EndDisabled();
        midiContextMenu(app, globalTarget(MidiTarget::LooperUndo), "midi_lundo");
        ImGui::SameLine();
        if (ImGui::Button("LIMPAR", bs)) lp.command(Looper::kClear);
        float lv = lp.levelDb.load();
        ImGui::SetNextItemWidth(200 * e);
        if (ImGui::SliderFloat("Volume do loop", &lv, -30, 6, "%.1f dB")) lp.levelDb.store(lv);
        ImGui::TextDisabled("Até 60 s em estéreo. O looper grava a saída final (com efeitos).");
        ImGui::TextDisabled("Botão direito nos botões para mapear um pedal MIDI.");
    }
    ImGui::End();
}

// ----------------------------------------------------------- Settings ---

bool sameAudioConfig(const AudioConfig& a, const AudioConfig& b) {
    return a.backend == b.backend && a.inputName == b.inputName && a.outputName == b.outputName &&
           a.sampleRate == b.sampleRate && a.bufferSize == b.bufferSize && a.inputChannel == b.inputChannel &&
           a.exclusive == b.exclusive;
}

void openFolder(const std::string& path) {
    ShellExecuteW(nullptr, L"open", utf8ToWide(path).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void drawAudioTab(App& app) {
    const float e = em();
    if (!g.audioEditInit) {
        g.audioEdit = app.settings.audio;
        g.audioEditInit = true;
    }
    AudioConfig& c = g.audioEdit;
    if (g.firstRunHint) {
        ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
        ImGui::TextWrapped("Bem-vindo! Escolha a entrada onde a guitarra está ligada e a saída, depois clique em Aplicar. "
                           "Use fones ou monitores: com o microfone do notebook e caixas de som pode dar microfonia.");
        ImGui::PopStyleColor();
        ImGui::Separator();
    }
    const float w = 320 * e;
    auto combo = [&](const char* label, std::string& value, const std::vector<std::string>& items) {
        ImGui::SetNextItemWidth(w);
        if (ImGui::BeginCombo(label, value.empty() ? "(padrão do sistema)" : value.c_str())) {
            if (ImGui::Selectable("(padrão do sistema)", value.empty())) value.clear();
            for (auto& it : items)
                if (ImGui::Selectable(it.c_str(), it == value)) value = it;
            ImGui::EndCombo();
        }
    };
    const auto& backends = AudioEngine::backendNames();
    ImGui::SetNextItemWidth(w);
    if (ImGui::BeginCombo("Driver", backends[size_t(c.backend)].c_str())) {
        for (int i = 0; i < int(backends.size()); ++i)
            if (ImGui::Selectable(backends[size_t(i)].c_str(), i == c.backend) && i != c.backend) {
                // Listing another driver's devices needs the current stream closed.
                c.backend = i;
                c.inputName.clear();
                c.outputName.clear();
                app.audio.stop();
                app.audio.refreshDevices(i);
            }
        ImGui::EndCombo();
    }
    combo("Entrada", c.inputName, app.audio.inputDevices());
    combo("Saída", c.outputName, app.audio.outputDevices());
    static const char* chans[] = {"Entrada 1 (esquerda)", "Entrada 2 (direita)", "Mix 1 + 2"};
    ImGui::SetNextItemWidth(w);
    ImGui::Combo("Canal da guitarra", &c.inputChannel, chans, 3);
    static const int rates[] = {44100, 48000, 88200, 96000};
    ImGui::SetNextItemWidth(w);
    if (ImGui::BeginCombo("Taxa de amostragem", (std::to_string(c.sampleRate) + " Hz").c_str())) {
        for (int r : rates)
            if (ImGui::Selectable((std::to_string(r) + " Hz").c_str(), r == c.sampleRate)) c.sampleRate = r;
        ImGui::EndCombo();
    }
    static const int sizes[] = {32, 64, 96, 128, 192, 256, 512, 1024};
    ImGui::SetNextItemWidth(w);
    if (ImGui::BeginCombo("Buffer", (std::to_string(c.bufferSize) + " amostras").c_str())) {
        for (int s : sizes) {
            char lbl[48];
            std::snprintf(lbl, sizeof lbl, "%d amostras (%.1f ms)", s, 1000.0 * s / c.sampleRate);
            if (ImGui::Selectable(lbl, s == c.bufferSize)) c.bufferSize = s;
        }
        ImGui::EndCombo();
    }
    ImGui::BeginDisabled(c.backend != 0);
    ImGui::Checkbox("WASAPI exclusivo (menor latência; outros apps ficam sem som)", &c.exclusive);
    ImGui::EndDisabled();

    ImGui::Dummy(ImVec2(0, 6 * e));
    const bool pending = !sameAudioConfig(c, app.settings.audio) || !app.audio.status().running;
    if (pending) {
        ImGui::PushStyleColor(ImGuiCol_Button, kAccent);
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(20, 20, 24, 255));
    }
    const bool apply = ImGui::Button("Aplicar", ImVec2(120 * e, 0));
    if (pending) ImGui::PopStyleColor(2);
    if (apply) {
        app.applyAudio(c);
        g.audioEdit = app.settings.audio;
        g.firstRunHint = false;
    }
    ImGui::SameLine();
    if (ImGui::Button("Desfazer mudanças")) g.audioEdit = app.settings.audio;
    ImGui::SameLine();
    if (ImGui::Button("Atualizar lista de dispositivos")) {
        app.audio.stop();
        app.audio.refreshDevices(c.backend);
        app.applyAudio(app.settings.audio);
    }
    if (!sameAudioConfig(c, app.settings.audio)) {
        ImGui::SameLine();
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kAccent), "mudanças ainda não aplicadas");
    }

    ImGui::Separator();
    const AudioStatus& s = app.audio.status();
    if (s.running) {
        ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1), "Rodando");
        ImGui::Text("Entrada: %s", s.inputName.c_str());
        ImGui::Text("Saída: %s", s.outputName.c_str());
        ImGui::Text("%d Hz · %d amostras · latência estimada %.1f ms", s.sampleRate, s.bufferSize, s.latencyMs);
    } else {
        ImGui::TextColored(ImVec4(1, 0.4f, 0.3f, 1), "Parado");
        if (!s.error.empty()) ImGui::TextWrapped("%s", s.error.c_str());
    }
    ImGui::Dummy(ImVec2(0, 4 * e));
    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    ImGui::TextWrapped("Dica: com interface de áudio, use WASAPI exclusivo com buffer de 64 a 128 amostras. "
                       "No modo compartilhado muitos drivers USB ficam presos em 480 amostras (~30 ms). "
                       "ASIO ainda não é suportado.");
    ImGui::PopStyleColor();
}

void drawMidiTab(App& app) {
    const float e = em();
    auto devs = MidiInput::devices();
    ImGui::SetNextItemWidth(320 * e);
    const std::string& cur = app.settings.midiDevice;
    if (ImGui::BeginCombo("Dispositivo MIDI", cur.empty() ? "(nenhum)" : cur.c_str())) {
        if (ImGui::Selectable("(nenhum)", cur.empty())) { app.openMidi(""); app.settings.save(); }
        for (auto& d : devs)
            if (ImGui::Selectable(d.c_str(), d == cur)) { app.openMidi(d); app.settings.save(); }
        ImGui::EndCombo();
    }
    if (devs.empty()) ImGui::TextDisabled("Nenhum dispositivo MIDI encontrado. Conecte o controlador e reabra esta aba.");
    if (ImGui::Checkbox("Program Change troca o preset (PC 0 = primeiro da lista)", &app.settings.programChangeSelectsPreset))
        app.settings.save();
    ImGui::TextDisabled("Última mensagem: %s", app.lastMidi.empty() ? "-" : app.lastMidi.c_str());
    if (app.learning) {
        ImGui::TextColored(ImVec4(1, 0.7f, 0.2f, 1), "Aprendendo: %s — mova um controle...", describeTarget(app, app.learnTarget).c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Cancelar")) app.cancelLearn();
    }
    ImGui::Separator();
    ImGui::TextUnformatted("Funções globais");
    for (int gid = MidiTarget::Master; gid <= MidiTarget::TapTempo; ++gid) {
        ImGui::PushID(gid);
        MidiTarget t = globalTarget(gid);
        int cc = app.mappedCc(t);
        if (ImGui::SmallButton("Aprender")) app.startLearn(t);
        ImGui::SameLine();
        if (cc >= 0) {
            if (ImGui::SmallButton("x")) app.clearMapping(t);
            ImGui::SameLine();
            ImGui::Text("%s  ·  CC %d", globalTargetName(gid), cc);
        } else {
            ImGui::TextDisabled("%s", globalTargetName(gid));
        }
        ImGui::PopID();
    }
    ImGui::Separator();
    ImGui::TextUnformatted("Mapeamentos");
    if (ImGui::BeginTable("map", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY,
                          ImVec2(0, std::max(120 * e, ImGui::GetContentRegionAvail().y - 30 * e)))) {
        ImGui::TableSetupColumn("CC", ImGuiTableColumnFlags_WidthFixed, 50 * e);
        ImGui::TableSetupColumn("Destino");
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 30 * e);
        ImGui::TableHeadersRow();
        int removeIdx = -1;
        for (int i = 0; i < int(app.settings.midiMap.size()); ++i) {
            const MidiMapping& mm = app.settings.midiMap[size_t(i)];
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%d", mm.cc);
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(describeTarget(app, mm.target).c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::PushID(i);
            if (ImGui::SmallButton("x")) removeIdx = i;
            ImGui::PopID();
        }
        ImGui::EndTable();
        if (removeIdx >= 0) {
            app.settings.midiMap.erase(app.settings.midiMap.begin() + removeIdx);
            app.settings.save();
        }
    }
    ImGui::TextDisabled("Para mapear um knob: botão direito nele > Aprender MIDI. Mapeamentos valem para a posição do bloco.");
}

void setUiScale(App& app, float scale) {
    app.settings.uiScale = std::clamp(std::round(scale * 20.0f) / 20.0f, 0.75f, 1.75f);
    ImGui::GetStyle().FontScaleMain = app.settings.uiScale;
}

void drawInterfaceTab(App& app) {
    const float e = em();
    boldText("Tamanho");
    float pct = app.settings.uiScale * 100.0f;
    ImGui::SetNextItemWidth(260 * e);
    if (ImGui::SliderFloat("Tamanho da interface", &pct, 75, 175, "%.0f%%")) setUiScale(app, pct / 100.0f);
    if (ImGui::IsItemDeactivatedAfterEdit()) app.settings.save();
    ImGui::SameLine();
    if (ImGui::Button("100%")) {
        setUiScale(app, 1.0f);
        app.settings.save();
    }
    ImGui::TextDisabled("Atalhos: Ctrl + = aumenta, Ctrl + - diminui, Ctrl + 0 volta ao normal.");

    ImGui::Separator();
    boldText("Afinador");
    ImGui::SetNextItemWidth(140 * e);
    ImGui::DragFloat("Referência A4", &app.settings.a4, 0.1f, 425, 455, "%.1f Hz");
    if (ImGui::IsItemDeactivatedAfterEdit()) app.settings.save();
    bool mute = app.settings.tunerMutes;
    if (ImGui::Checkbox("Silenciar a saída enquanto o afinador está aberto", &mute)) {
        app.settings.tunerMutes = mute;
        if (app.tunerOpen()) app.rig.muted.store(mute);
        app.settings.save();
    }

    ImGui::Separator();
    boldText("Pastas");
    if (ImGui::Button("Abrir pasta de presets")) openFolder(app.presetDir());
    ImGui::SameLine();
    if (ImGui::Button("Abrir pasta de gravações")) openFolder(documentsDir("Gravações"));
    ImGui::SameLine();
    if (ImGui::Button("Abrir pasta de configurações")) openFolder(appDataDir());

    ImGui::Separator();
    boldText("Sobre");
    ImGui::TextDisabled("ToneForge 0.2 · DSP próprio · miniaudio 0.11.25 · Dear ImGui 1.92.9b");
}

void drawSettings(App& app) {
    if (!g.showSettings) {
        g.audioEditInit = false;  // reload the saved config next time it opens
        return;
    }
    placeWindow(680, 560, 0.5f, 0.5f);
    if (ImGui::Begin("Configurações", &g.showSettings, ImGuiWindowFlags_NoCollapse)) {
        if (ImGui::BeginTabBar("settings_tabs")) {
            static const char* names[] = {"Áudio", "MIDI", "Interface"};
            for (int t = 0; t < 3; ++t) {
                ImGuiTabItemFlags flags = g.selectTab && g.settingsTab == t ? ImGuiTabItemFlags_SetSelected : 0;
                if (!ImGui::BeginTabItem(names[t], nullptr, flags)) continue;
                if (!g.selectTab) g.settingsTab = t;
                ImGui::Dummy(ImVec2(0, 4 * em()));
                if (t == 0) drawAudioTab(app);
                else if (t == 1) drawMidiTab(app);
                else drawInterfaceTab(app);
                ImGui::EndTabItem();
            }
            g.selectTab = false;
            ImGui::EndTabBar();
        }
    }
    ImGui::End();
}

void drawHelp() {
    if (!g.showHelp) return;
    placeWindow(560, 440, 0.5f, 0.5f);
    if (ImGui::Begin("Ajuda", &g.showHelp, ImGuiWindowFlags_NoCollapse)) {
        boldText("ToneForge", 1.3f);
        ImGui::TextWrapped("Simulador de amplificador de guitarra e baixo: pedais, 8 amplificadores, gabinetes com "
                           "microfone (ou suas IRs), rack de efeitos, cadeia dupla em paralelo, afinador, looper, gravação "
                           "em WAV, presets e controle MIDI.");
        ImGui::Separator();
        boldText("Atalhos");
        ImGui::BulletText("Ctrl+,: configurações (áudio, MIDI, interface)");
        ImGui::BulletText("Ctrl+S: salvar preset   ·   Page Up / Page Down: preset anterior/próximo");
        ImGui::BulletText("Ctrl + = / Ctrl + -: aumenta ou diminui a interface   ·   Ctrl+0: tamanho normal");
        ImGui::BulletText("T: afinador   ·   L: looper   ·   Espaço: Rec/Dub do looper");
        ImGui::BulletText("B: liga/desliga o bloco selecionado   ·   Delete: remove o bloco selecionado");
        ImGui::Separator();
        boldText("Cadeia de sinal");
        ImGui::TextWrapped("Entrada → Noise Gate → Pedais → Amp → Gabinete → Rack → Master → Looper → Saída. "
                           "Com a cadeia dupla ligada, o sinal passa por A e B em paralelo e é somado (use Nível e Pan "
                           "de cada cadeia para abrir em estéreo).");
        ImGui::Separator();
        boldText("Arquivos");
        ImGui::BulletText("Presets: Documentos\\ToneForge\\Presets (JSON)");
        ImGui::BulletText("Gravações: Documentos\\ToneForge\\Gravações (WAV)");
        ImGui::BulletText("Configurações: %%APPDATA%%\\ToneForge\\settings.json");
    }
    ImGui::End();
}

void drawModals(App& app) {
    const float e = em();
    if (g.openSaveAs) { ImGui::OpenPopup("Salvar preset"); g.openSaveAs = false; }
    if (ImGui::BeginPopupModal("Salvar preset", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Nome do preset:");
        ImGui::SetNextItemWidth(300 * e);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        bool enter = ImGui::InputText("##name", g.saveName, sizeof g.saveName, ImGuiInputTextFlags_EnterReturnsTrue);
        if (ImGui::Button("Salvar", ImVec2(120 * e, 0)) || enter) {
            app.savePresetAs(g.saveName);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancelar", ImVec2(120 * e, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (g.openDelete) { ImGui::OpenPopup("Excluir preset"); g.openDelete = false; }
    if (ImGui::BeginPopupModal("Excluir preset", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Excluir o preset \"%s\"? Isso apaga o arquivo.", app.currentName.c_str());
        if (ImGui::Button("Excluir", ImVec2(120 * e, 0))) {
            app.deleteCurrent();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancelar", ImVec2(120 * e, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

void drawStatusBar(App& app) {
    const AudioStatus& s = app.audio.status();
    char audioText[200];
    if (s.running)
        std::snprintf(audioText, sizeof audioText, "%s · %d Hz · %d amostras · ~%.1f ms",
                      AudioEngine::backendNames()[size_t(app.settings.audio.backend)].c_str(), s.sampleRate, s.bufferSize,
                      s.latencyMs);
    else
        std::snprintf(audioText, sizeof audioText, "Áudio parado — clique aqui para configurar");
    ImGui::PushStyleColor(ImGuiCol_TextLink, s.running ? ImVec4(0.6f, 0.6f, 0.65f, 1) : ImVec4(1, 0.4f, 0.3f, 1));
    if (ImGui::TextLink(audioText)) openSettings(0);
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Abrir as configurações de áudio (Ctrl+,)");
    ImGui::SameLine(0, 24);
    float cpu = app.rig.cpuLoad.load() * 100;
    ImGui::TextColored(cpu > 70 ? ImVec4(1, 0.4f, 0.3f, 1) : ImVec4(0.6f, 0.6f, 0.65f, 1), "CPU %.0f%%", cpu);
    ImGui::SameLine(0, 24);
    std::string midiText = "MIDI: " + (app.settings.midiDevice.empty() ? std::string("nenhum") : app.settings.midiDevice);
    ImGui::PushStyleColor(ImGuiCol_TextLink, ImVec4(0.6f, 0.6f, 0.65f, 1));
    if (ImGui::TextLink(midiText.c_str())) openSettings(1);
    ImGui::PopStyleColor();
    if (app.rig.clipped.exchange(false)) {
        ImGui::SameLine(0, 24);
        ImGui::TextColored(ImVec4(1, 0.3f, 0.2f, 1), "CLIP! baixe o Master");
    }
    if (app.now() < app.toastUntil) {
        ImGui::SameLine(0, 24);
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kAccent), "%s", app.toast.c_str());
    }
}

void handleShortcuts(App& app) {
    ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput) return;
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) requestSave(app);
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Comma, false)) {
        if (g.showSettings) g.showSettings = false;
        else openSettings(g.settingsTab);
    }
    if (io.KeyCtrl && (ImGui::IsKeyPressed(ImGuiKey_Equal, true) || ImGui::IsKeyPressed(ImGuiKey_KeypadAdd, true))) {
        setUiScale(app, app.settings.uiScale + 0.05f);
        app.settings.save();
    }
    if (io.KeyCtrl && (ImGui::IsKeyPressed(ImGuiKey_Minus, true) || ImGui::IsKeyPressed(ImGuiKey_KeypadSubtract, true))) {
        setUiScale(app, app.settings.uiScale - 0.05f);
        app.settings.save();
    }
    if (io.KeyCtrl && (ImGui::IsKeyPressed(ImGuiKey_0, false) || ImGui::IsKeyPressed(ImGuiKey_Keypad0, false))) {
        setUiScale(app, 1.0f);
        app.settings.save();
    }
    if (ImGui::IsKeyPressed(ImGuiKey_PageDown, false)) app.stepPreset(1);
    if (ImGui::IsKeyPressed(ImGuiKey_PageUp, false)) app.stepPreset(-1);
    if (io.KeyCtrl || ImGui::IsAnyItemActive()) return;
    if (ImGui::IsKeyPressed(ImGuiKey_T, false)) app.setTunerOpen(!app.tunerOpen());
    if (ImGui::IsKeyPressed(ImGuiKey_L, false)) g.showLooper = !g.showLooper;
    if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) app.rig.looper.command(Looper::kRecDub);
    Module* m = selectedModule(app);
    if (m && ImGui::IsKeyPressed(ImGuiKey_B, false)) m->bypassed.store(!m->bypassed.load());
    if (m && g.sel.section >= 0 && ImGui::IsKeyPressed(ImGuiKey_Delete, false)) {
        if (g.sel.section == int(Section::Amp) || g.sel.section == int(Section::Cab))
            app.rig.setSlot(g.sel.chain, Section(g.sel.section), nullptr);
        else
            app.rig.remove(g.sel.chain, Section(g.sel.section), g.sel.slot);
        g.sel = Selection{};
    }
}

}  // namespace

void applyDebugArgs(App& app, const char* cmd) {
    if (!cmd) return;
    std::string s = cmd;
    if (s.find("--dual") != std::string::npos) app.rig.dual.store(true);
    size_t p = s.find("--select=");
    if (p != std::string::npos) {
        int c = 0, sec = 0, slot = 0;
        if (std::sscanf(s.c_str() + p + 9, "%d:%d:%d", &c, &sec, &slot) == 3) g.sel = Selection{c, sec, slot};
    }
    p = s.find("--show=");
    if (p != std::string::npos) {
        std::string w = s.substr(p + 7, s.find(' ', p) == std::string::npos ? std::string::npos : s.find(' ', p) - p - 7);
        if (w.find("tuner") != std::string::npos) app.setTunerOpen(true);
        g.showLooper = w.find("looper") != std::string::npos;
        if (w.find("audio") != std::string::npos) openSettings(0);
        if (w.find("midi") != std::string::npos) openSettings(1);
        if (w.find("interface") != std::string::npos) openSettings(2);
        g.showHelp = w.find("help") != std::string::npos;
    }
}

void setFonts(ImFont* regular, ImFont* bold, bool hasSymbols) {
    g_regular = regular;
    g_bold = bold ? bold : regular;
    g_symbols = hasSymbols;
}

void applyTheme() {
    ImGui::StyleColorsDark();
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 8;
    s.ChildRounding = 8;
    s.FrameRounding = 6;
    s.PopupRounding = 6;
    s.GrabRounding = 6;
    s.TabRounding = 6;
    s.FramePadding = ImVec2(10, 6);
    s.ItemSpacing = ImVec2(8, 7);
    s.WindowPadding = ImVec2(12, 10);
    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg] = ImVec4(0.09f, 0.095f, 0.11f, 1);
    c[ImGuiCol_ChildBg] = ImVec4(0.115f, 0.12f, 0.14f, 1);
    c[ImGuiCol_PopupBg] = ImVec4(0.12f, 0.125f, 0.145f, 0.98f);
    c[ImGuiCol_Border] = ImVec4(0.25f, 0.26f, 0.3f, 1);
    c[ImGuiCol_FrameBg] = ImVec4(0.18f, 0.19f, 0.22f, 1);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.24f, 0.25f, 0.29f, 1);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.28f, 0.29f, 0.34f, 1);
    c[ImGuiCol_Button] = ImVec4(0.2f, 0.21f, 0.25f, 1);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.3f, 0.31f, 0.36f, 1);
    c[ImGuiCol_ButtonActive] = ImVec4(0.94f, 0.63f, 0.2f, 1);
    c[ImGuiCol_Header] = ImVec4(0.94f, 0.63f, 0.2f, 0.35f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.94f, 0.63f, 0.2f, 0.55f);
    c[ImGuiCol_HeaderActive] = ImVec4(0.94f, 0.63f, 0.2f, 0.8f);
    c[ImGuiCol_CheckMark] = ImVec4(0.94f, 0.63f, 0.2f, 1);
    c[ImGuiCol_SliderGrab] = ImVec4(0.94f, 0.63f, 0.2f, 1);
    c[ImGuiCol_SliderGrabActive] = ImVec4(1.0f, 0.75f, 0.35f, 1);
    c[ImGuiCol_TitleBg] = ImVec4(0.12f, 0.125f, 0.145f, 1);
    c[ImGuiCol_TitleBgActive] = ImVec4(0.2f, 0.16f, 0.1f, 1);
    c[ImGuiCol_Separator] = ImVec4(0.25f, 0.26f, 0.3f, 1);
    c[ImGuiCol_PlotHistogram] = ImVec4(0.94f, 0.63f, 0.2f, 1);
    c[ImGuiCol_DragDropTarget] = ImVec4(1.0f, 0.75f, 0.35f, 1);
}

void draw(App& app) {
    if (app.firstRun) {
        openSettings(0);
        app.firstRun = false;
        g.firstRunHint = true;
    }
    handleShortcuts(app);
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("##main", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);
    const float e = em();
    drawTopBar(app);
    ImGui::Dummy(ImVec2(0, 2 * e));
    const float chainsH = (app.rig.dual.load() ? 2 : 1) * 104 * e + 52 * e;
    ImGui::BeginChild("chains", ImVec2(0, chainsH), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
    drawChains(app);
    ImGui::EndChild();
    ImGui::BeginChild("editor", ImVec2(0, -ImGui::GetFrameHeightWithSpacing()), ImGuiChildFlags_Borders);
    drawEditor(app);
    ImGui::EndChild();
    drawStatusBar(app);
    ImGui::End();

    drawTuner(app);
    drawLooper(app);
    drawSettings(app);
    drawHelp();
    drawModals(app);
}

}  // namespace ui
