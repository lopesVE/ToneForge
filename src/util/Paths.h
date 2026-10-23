// Windows path and string helpers.
#pragma once
#include <string>

std::wstring utf8ToWide(const std::string& s);
std::string wideToUtf8(const std::wstring& s);
// %APPDATA%\ToneForge (created on demand), UTF-8.
std::string appDataDir();
// Documents\ToneForge\<sub> (created on demand), UTF-8.
std::string documentsDir(const std::string& sub);
bool readTextFile(const std::string& pathUtf8, std::string& out);
bool writeTextFile(const std::string& pathUtf8, const std::string& text);
