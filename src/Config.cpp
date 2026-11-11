#include "Config.h"

#include <cwchar>
#include <cstdlib>

namespace bdx12 {

namespace {
Config g_config;

std::wstring ToLower(std::wstring s)
{
    for (auto& c : s) c = static_cast<wchar_t>(towlower(c));
    return s;
}

std::wstring Trim(const std::wstring& s)
{
    const size_t b = s.find_first_not_of(L" \t\r\n");
    if (b == std::wstring::npos) return {};
    const size_t e = s.find_last_not_of(L" \t\r\n");
    return s.substr(b, e - b + 1);
}

std::wstring MakeDir(const std::wstring& value)
{
    std::wstring dir = value;
    if (dir.empty()) return GameDirectory();
    const bool absolute = dir.size() > 1 && (dir[1] == L':' || (dir[0] == L'\\' && dir[1] == L'\\'));
    if (!absolute) dir = GameDirectory() + dir;
    if (dir.back() != L'\\' && dir.back() != L'/') dir += L'\\';
    return dir;
}
}

const Config& GetConfig() { return g_config; }
Config&       MutableConfig() { return g_config; }

std::wstring Config::GetString(const char* section, const char* key, const wchar_t* def) const
{
    wchar_t buf[1024] = {};
    GetPrivateProfileStringW(Widen(section).c_str(), Widen(key).c_str(), def, buf, static_cast<DWORD>(std::size(buf)),
                             iniPath.c_str());
    std::wstring v = Trim(buf);
    const size_t sc = v.find_first_of(L";#");
    if (sc != std::wstring::npos) v = Trim(v.substr(0, sc));
    return v;
}

int Config::GetInt(const char* section, const char* key, int def) const
{
    const std::wstring v = GetString(section, key, L"");
    if (v.empty()) return def;
    return static_cast<int>(wcstol(v.c_str(), nullptr, 0));
}

float Config::GetFloat(const char* section, const char* key, float def) const
{
    const std::wstring v = GetString(section, key, L"");
    if (v.empty()) return def;
    return static_cast<float>(wcstod(v.c_str(), nullptr));
}

void Config::Load()
{
    iniPath = GameDirectory() + L"BullyDX12.ini";

    const std::wstring backendStr = ToLower(GetString("Renderer", "Backend", L"D3D9On12"));
    if (backendStr == L"native")        backend = Backend::Native;
    else if (backendStr == L"external") backend = Backend::External;
    else                                backend = Backend::D3D9On12;

    externalDll = GetString("Renderer", "ExternalDll", L"");
    if (!externalDll.empty() && !(externalDll.size() > 1 && externalDll[1] == L':'))
        externalDll = GameDirectory() + externalDll;

    const std::wstring adapterStr = ToLower(GetString("Renderer", "Adapter", L"HighPerformance"));
    if (adapterStr == L"default")             adapterPreference = AdapterPreference::Default;
    else if (adapterStr == L"minimumpower")   adapterPreference = AdapterPreference::MinimumPower;
    else if (adapterStr == L"highperformance") adapterPreference = AdapterPreference::HighPerformance;
    else {
        adapterPreference = AdapterPreference::Index;
        adapterIndex      = static_cast<int>(wcstol(adapterStr.c_str(), nullptr, 10));
    }

    fallbackToNative   = GetInt("Renderer", "FallbackToNative", 1) != 0;
    debugLayer         = GetInt("Renderer", "D3D12DebugLayer", 0) != 0;
    gpuBasedValidation = GetInt("Renderer", "GPUBasedValidation", 0) != 0;
    postProcessing     = GetInt("Renderer", "PostProcessing", 1) != 0;

    dumpShaders        = GetInt("Shaders", "Dump", 0) != 0;
    overrideShaders    = GetInt("Shaders", "Override", 1) != 0;
    shaderDumpDir      = MakeDir(GetString("Shaders", "DumpDirectory", L"BullyDX12\\shaders\\dump"));
    shaderOverrideDir  = MakeDir(GetString("Shaders", "OverrideDirectory", L"BullyDX12\\shaders\\override"));

    extensionsEnabled  = GetInt("Extensions", "Enabled", 1) != 0;
    extensionDir       = MakeDir(GetString("Extensions", "Directory", L"BullyDX12\\extensions"));
    extensionFilter    = GetString("Extensions", "Load", L"*");

    const std::wstring lvl = ToLower(GetString("Log", "Level", L"Info"));
    if (lvl == L"trace")        logLevel = LogLevel::Trace;
    else if (lvl == L"warning") logLevel = LogLevel::Warning;
    else if (lvl == L"error")   logLevel = LogLevel::Error;
    else                        logLevel = LogLevel::Info;
}

}
