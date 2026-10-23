#include "util/Paths.h"

#include <windows.h>
#include <shlobj.h>

#include <cstdio>
#include <cstdlib>

std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

std::string wideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

static std::string knownFolder(REFKNOWNFOLDERID id, const std::wstring& sub) {
    std::wstring path;
    // TONEFORGE_DATA redirects every data folder (portable mode / testing).
    if (const wchar_t* over = _wgetenv(L"TONEFORGE_DATA"); over && *over) {
        path = over;
        CreateDirectoryW(path.c_str(), nullptr);
    } else {
        PWSTR base = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &base))) path = base;
        CoTaskMemFree(base);
    }
    if (path.empty()) path = L".";
    size_t start = 0;
    std::wstring rest = sub;
    while (start <= rest.size()) {
        size_t sep = rest.find(L'\\', start);
        std::wstring part = rest.substr(start, sep == std::wstring::npos ? std::wstring::npos : sep - start);
        if (!part.empty()) {
            path += L"\\" + part;
            CreateDirectoryW(path.c_str(), nullptr);
        }
        if (sep == std::wstring::npos) break;
        start = sep + 1;
    }
    return wideToUtf8(path);
}

std::string appDataDir() { return knownFolder(FOLDERID_RoamingAppData, L"ToneForge"); }

std::string documentsDir(const std::string& sub) {
    return knownFolder(FOLDERID_Documents, L"ToneForge" + (sub.empty() ? L"" : L"\\" + utf8ToWide(sub)));
}

bool readTextFile(const std::string& pathUtf8, std::string& out) {
    FILE* f = _wfopen(utf8ToWide(pathUtf8).c_str(), L"rb");
    if (!f) return false;
    out.clear();
    char buf[8192];
    size_t got;
    while ((got = fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, got);
    fclose(f);
    return true;
}

bool writeTextFile(const std::string& pathUtf8, const std::string& text) {
    std::wstring tmp = utf8ToWide(pathUtf8) + L".tmp";
    FILE* f = _wfopen(tmp.c_str(), L"wb");
    if (!f) return false;
    bool ok = fwrite(text.data(), 1, text.size(), f) == text.size();
    ok = (fclose(f) == 0) && ok;
    if (!ok) { DeleteFileW(tmp.c_str()); return false; }
    return MoveFileExW(tmp.c_str(), utf8ToWide(pathUtf8).c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}
