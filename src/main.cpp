// ToneForge entry point: Win32 window + OpenGL context + Dear ImGui loop.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <GL/gl.h>

#include <algorithm>
#include <cstring>

#include "app/App.h"
#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_win32.h"
#include "ui/Ui.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace {

HGLRC g_glrc = nullptr;
HDC g_hdc = nullptr;
int g_width = 1400, g_height = 820;
WINDOWPLACEMENT g_placement{sizeof(WINDOWPLACEMENT)};
bool g_placementValid = false;

bool createGl(HWND hwnd) {
    HDC dc = GetDC(hwnd);
    PIXELFORMATDESCRIPTOR pfd{};
    pfd.nSize = sizeof pfd;
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    int pf = ChoosePixelFormat(dc, &pfd);
    if (pf == 0 || !SetPixelFormat(dc, pf, &pfd)) return false;
    g_hdc = dc;
    g_glrc = wglCreateContext(dc);
    return g_glrc && wglMakeCurrent(dc, g_glrc);
}

void enableVsync() {
    using SwapFn = BOOL(WINAPI*)(int);
    if (auto fn = reinterpret_cast<SwapFn>(reinterpret_cast<void*>(wglGetProcAddress("wglSwapIntervalEXT")))) fn(1);
}

LRESULT WINAPI wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam)) return true;
    switch (msg) {
        case WM_SIZE:
            if (wParam != SIZE_MINIMIZED) {
                g_width = LOWORD(lParam);
                g_height = HIWORD(lParam);
            }
            return 0;
        case WM_GETMINMAXINFO: {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
            mmi->ptMinTrackSize.x = 900;
            mmi->ptMinTrackSize.y = 560;
            return 0;
        }
        case WM_SYSCOMMAND:
            if ((wParam & 0xfff0) == SC_KEYMENU) return 0;  // no ALT menu
            break;
        case WM_CLOSE:
            // Remember size/position before the window goes away.
            g_placementValid = GetWindowPlacement(hwnd, &g_placement) != 0;
            break;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

ImFont* loadFont(const char* path, float size, bool merge = false) {
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) return nullptr;
    ImFontConfig cfg;
    cfg.MergeMode = merge;  // merge: add glyphs to the previously loaded font
    return ImGui::GetIO().Fonts->AddFontFromFileTTF(path, size, &cfg);
}

// Initial window rectangle: the saved one if it is still on a monitor,
// otherwise the default size clamped to the primary monitor's work area.
void initialPlacement(const AppSettings& s, float scale, RECT& rect, bool& maximize) {
    MONITORINFO mi{};
    mi.cbSize = sizeof mi;
    GetMonitorInfoW(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
    const RECT wa = mi.rcWork;
    const int waW = wa.right - wa.left, waH = wa.bottom - wa.top;
    int w = int(1400 * scale), h = int(820 * scale);
    maximize = false;
    if (w > waW * 0.95 || h > waH * 0.95) {
        w = int(waW * 0.9);
        h = int(waH * 0.9);
        maximize = true;
    }
    rect.left = wa.left + (waW - w) / 2;
    rect.top = wa.top + (waH - h) / 2;
    rect.right = rect.left + w;
    rect.bottom = rect.top + h;
    if (s.winW >= 600 && s.winH >= 400) {
        RECT saved{s.winX, s.winY, s.winX + s.winW, s.winY + s.winH};
        if (MonitorFromRect(&saved, MONITOR_DEFAULTTONULL)) {
            rect = saved;
            maximize = s.winMaximized;
        }
    }
}

}  // namespace

int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR cmdLine, int) {
    const bool noAudio = cmdLine && std::strstr(cmdLine, "--no-audio");
    ImGui_ImplWin32_EnableDpiAwareness();
    const float scale = ImGui_ImplWin32_GetDpiScaleForMonitor(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY));

    App app;
    app.settings.load();  // only for the window placement here; init() loads it again
    RECT startRect{};
    bool startMaximized = false;
    initialPlacement(app.settings, scale, startRect, startMaximized);

    WNDCLASSEXW wc{sizeof(wc), CS_OWNDC, wndProc, 0, 0, inst, LoadIconW(nullptr, IDI_APPLICATION),
                   LoadCursorW(nullptr, IDC_ARROW), nullptr, nullptr, L"ToneForgeWindow", nullptr};
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"ToneForge — simulador de amplificador", WS_OVERLAPPEDWINDOW,
                              startRect.left, startRect.top, startRect.right - startRect.left,
                              startRect.bottom - startRect.top, nullptr, nullptr, inst, nullptr);
    if (!hwnd || !createGl(hwnd)) {
        MessageBoxW(nullptr, L"Não consegui criar o contexto OpenGL.", L"ToneForge", MB_ICONERROR);
        return 1;
    }
    enableVsync();
    {
        // SetWindowPlacement keeps the normal rectangle even when starting maximized.
        WINDOWPLACEMENT wp{};
        wp.length = sizeof wp;
        GetWindowPlacement(hwnd, &wp);
        wp.rcNormalPosition = startRect;
        wp.showCmd = startMaximized ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL;
        SetWindowPlacement(hwnd, &wp);
    }
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;  // window layout is not persisted
    ui::applyTheme();
    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(scale);
    style.FontScaleDpi = scale;
    style.FontSizeBase = 16.0f;

    // Segoe UI Symbol is merged into both fonts for icons such as the gear.
    const char* symbols = "C:\\Windows\\Fonts\\seguisym.ttf";
    ImFont* regular = loadFont("C:\\Windows\\Fonts\\segoeui.ttf", 16.0f);
    bool hasSymbols = regular && loadFont(symbols, 16.0f, true);
    ImFont* bold = loadFont("C:\\Windows\\Fonts\\segoeuib.ttf", 16.0f);
    if (bold) hasSymbols = loadFont(symbols, 16.0f, true) && hasSymbols;
    if (!regular) regular = io.Fonts->AddFontDefault();
    io.FontDefault = regular;
    ui::setFonts(regular, bold, hasSymbols);

    ImGui_ImplWin32_InitForOpenGL(hwnd);
    ImGui_ImplOpenGL3_Init();

    app.init(noAudio);
    style.FontScaleMain = std::clamp(app.settings.uiScale, 0.75f, 1.75f);
    ui::applyDebugArgs(app, cmdLine);

    bool done = false;
    while (!done) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;
        if (IsIconic(hwnd)) {
            app.frame();
            Sleep(16);
            continue;
        }
        app.frame();
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        ui::draw(app);
        ImGui::Render();
        glViewport(0, 0, g_width, g_height);
        glClearColor(0.09f, 0.095f, 0.11f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        SwapBuffers(g_hdc);
    }

    if (g_placementValid) {
        const RECT& r = g_placement.rcNormalPosition;
        app.settings.winX = r.left;
        app.settings.winY = r.top;
        app.settings.winW = r.right - r.left;
        app.settings.winH = r.bottom - r.top;
        app.settings.winMaximized = g_placement.showCmd == SW_SHOWMAXIMIZED;
    }
    app.shutdown();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    wglMakeCurrent(nullptr, nullptr);
    wglDeleteContext(g_glrc);
    ReleaseDC(hwnd, g_hdc);
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, inst);
    return 0;
}
