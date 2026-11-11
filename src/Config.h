#pragma once
#include "Common.h"

namespace bdx12 {

enum class AdapterPreference { Default, HighPerformance, MinimumPower, Index };

struct Config {
    Backend           backend            = Backend::D3D9On12;
    std::wstring      externalDll;
    AdapterPreference adapterPreference  = AdapterPreference::HighPerformance;
    int               adapterIndex       = 0;
    bool              fallbackToNative   = true;
    bool              debugLayer         = false;
    bool              gpuBasedValidation = false;
    bool              postProcessing     = true;

    bool              dumpShaders        = false;
    bool              overrideShaders    = true;
    std::wstring      shaderDumpDir;
    std::wstring      shaderOverrideDir;

    bool              extensionsEnabled  = true;
    std::wstring      extensionDir;
    std::wstring      extensionFilter;

    LogLevel          logLevel           = LogLevel::Info;

    std::wstring      iniPath;

    void Load();

    int          GetInt(const char* section, const char* key, int def) const;
    float        GetFloat(const char* section, const char* key, float def) const;
    std::wstring GetString(const char* section, const char* key, const wchar_t* def) const;
};

const Config& GetConfig();
Config&       MutableConfig();

}
