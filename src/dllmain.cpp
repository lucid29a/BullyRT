// BullyDX12 - d3d9.dll proxy that runs Bully: Scholarship Edition on Direct3D 12 via D3D9On12.

#include "Common.h"
#include "Backend.h"
#include "Config.h"
#include "D3D12Context.h"
#include "ExtensionManager.h"
#include "Proxy/Direct3D9Proxy.h"
#include "Display.h"

#include <algorithm>
#include <cstring>

#define BULLYDX12_VERSION "1.0.0"

namespace bdx12 {
namespace {
std::once_flag g_initOnce;

void InitializeOnce()
{
    std::call_once(g_initOnce, [] {
        BDX_INFO("BullyDX12 " BULLYDX12_VERSION " - game directory: %s", Narrow(GameDirectory()).c_str());
        const Config& c = GetConfig();
        static const char* kBackends[] = {"D3D9On12", "Native", "External"};
        BDX_INFO("Config: Backend=%s FallbackToNative=%d DebugLayer=%d PostProcessing=%d DumpShaders=%d OverrideShaders=%d",
                 kBackends[static_cast<int>(c.backend)], c.fallbackToNative, c.debugLayer, c.postProcessing,
                 c.dumpShaders, c.overrideShaders);
        Display::Get().LoadConfig();
        BDX_INFO("Display: %s", Display::Get().Borderless() ? "borderless windowed" : "game default");
        if (!SystemD3D9()) BDX_ERROR("System d3d9.dll unavailable - the game will not be able to render");

        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                           reinterpret_cast<LPCWSTR>(&InitializeOnce), &self);
    });
}

std::once_flag g_extOnce;
void LoadExtensionsOnce(Backend backend)
{
    std::call_once(g_extOnce, [backend] {
        ExtensionManager::Get().SetBackend(backend);
        ExtensionManager::Get().LoadAll();
    });
}

FARPROC SystemProc(const char* name)
{
    HMODULE m = SystemD3D9();
    return m ? GetProcAddress(m, name) : nullptr;
}
}
}

using namespace bdx12;

extern "C" IDirect3D9* WINAPI BDX_Direct3DCreate9(UINT sdkVersion)
{
    Startup("Direct3DCreate9 called by the game");
    InitializeOnce();
    BDX_INFO("Direct3DCreate9(SDK %u)", sdkVersion);

    BackendResult r = CreateBackend(sdkVersion, GetConfig());
    Startup(r.d3d9 ? "renderer backend created" : "renderer backend FAILED");
    if (!r.d3d9) return nullptr;

    LoadExtensionsOnce(r.type);
    Startup("Direct3DCreate9 done");
    return new Direct3D9Proxy(r.d3d9, r.preferredAdapter);
}

extern "C" HRESULT WINAPI BDX_Direct3DCreate9Ex(UINT sdkVersion, IDirect3D9Ex** out)
{
    InitializeOnce();
    BDX_WARN("Direct3DCreate9Ex called - forwarded to system D3D9 without BullyDX12 hooks");
    using PFN = HRESULT(WINAPI*)(UINT, IDirect3D9Ex**);
    auto fn = reinterpret_cast<PFN>(SystemProc("Direct3DCreate9Ex"));
    return fn ? fn(sdkVersion, out) : D3DERR_NOTAVAILABLE;
}

extern "C" IDirect3D9* WINAPI BDX_Direct3DCreate9On12(UINT sdkVersion, D3D9ON12_ARGS* args, UINT n)
{
    InitializeOnce();
    auto fn = reinterpret_cast<PFN_Direct3DCreate9On12>(SystemProc("Direct3DCreate9On12"));
    return fn ? fn(sdkVersion, args, n) : nullptr;
}

extern "C" HRESULT WINAPI BDX_Direct3DCreate9On12Ex(UINT sdkVersion, D3D9ON12_ARGS* args, UINT n, IDirect3D9Ex** out)
{
    InitializeOnce();
    auto fn = reinterpret_cast<PFN_Direct3DCreate9On12Ex>(SystemProc("Direct3DCreate9On12Ex"));
    return fn ? fn(sdkVersion, args, n, out) : D3DERR_NOTAVAILABLE;
}

extern "C" int WINAPI BDX_D3DPERF_BeginEvent(D3DCOLOR col, LPCWSTR name)
{
    using PFN = int(WINAPI*)(D3DCOLOR, LPCWSTR);
    auto fn = reinterpret_cast<PFN>(SystemProc("D3DPERF_BeginEvent"));
    return fn ? fn(col, name) : 0;
}
extern "C" int WINAPI BDX_D3DPERF_EndEvent()
{
    using PFN = int(WINAPI*)();
    auto fn = reinterpret_cast<PFN>(SystemProc("D3DPERF_EndEvent"));
    return fn ? fn() : 0;
}
extern "C" void WINAPI BDX_D3DPERF_SetMarker(D3DCOLOR col, LPCWSTR name)
{
    using PFN = void(WINAPI*)(D3DCOLOR, LPCWSTR);
    if (auto fn = reinterpret_cast<PFN>(SystemProc("D3DPERF_SetMarker"))) fn(col, name);
}
extern "C" void WINAPI BDX_D3DPERF_SetRegion(D3DCOLOR col, LPCWSTR name)
{
    using PFN = void(WINAPI*)(D3DCOLOR, LPCWSTR);
    if (auto fn = reinterpret_cast<PFN>(SystemProc("D3DPERF_SetRegion"))) fn(col, name);
}
extern "C" BOOL WINAPI BDX_D3DPERF_QueryRepeatFrame()
{
    using PFN = BOOL(WINAPI*)();
    auto fn = reinterpret_cast<PFN>(SystemProc("D3DPERF_QueryRepeatFrame"));
    return fn ? fn() : FALSE;
}
extern "C" void WINAPI BDX_D3DPERF_SetOptions(DWORD options)
{
    using PFN = void(WINAPI*)(DWORD);
    if (auto fn = reinterpret_cast<PFN>(SystemProc("D3DPERF_SetOptions"))) fn(options);
}
extern "C" DWORD WINAPI BDX_D3DPERF_GetStatus()
{
    using PFN = DWORD(WINAPI*)();
    auto fn = reinterpret_cast<PFN>(SystemProc("D3DPERF_GetStatus"));
    return fn ? fn() : 0;
}

#define BDX_RAW_FORWARD(name)                                                       \
    static FARPROC g_fwd_##name = nullptr;                                          \
    static FARPROC __cdecl Resolve_##name()                                         \
    {                                                                               \
        FARPROC p = SystemProc(#name);                                              \
        if (!p) p = reinterpret_cast<FARPROC>(&MissingExport);                      \
        return p;                                                                   \
    }                                                                               \
    extern "C" __declspec(naked) void BDX_##name()                                  \
    {                                                                               \
        __asm pushad                                                                \
        __asm call Resolve_##name                                                   \
        __asm mov g_fwd_##name, eax                                                 \
        __asm popad                                                                 \
        __asm jmp dword ptr [g_fwd_##name]                                          \
    }

static int __cdecl MissingExport() { return 0; }

BDX_RAW_FORWARD(Direct3DShaderValidatorCreate9)
BDX_RAW_FORWARD(PSGPError)
BDX_RAW_FORWARD(PSGPSampleTexture)
BDX_RAW_FORWARD(DebugSetLevel)
BDX_RAW_FORWARD(DebugSetMute)
BDX_RAW_FORWARD(Direct3D9EnableMaximizedWindowedModeShim)

static void GrowGameHeap()
{
    const int mb = GetConfig().GetInt("Culling", "GameHeapMB", 256);
    if (mb <= 64) return;
    if (reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)) != 0x400000) return;
    uint8_t* at = reinterpret_cast<uint8_t*>(0x5EE65A);
    const uint8_t expect[10] = {0xC7, 0x05, 0x80, 0x39, 0xCA, 0x00, 0x00, 0x00, 0x00, 0x04};
    MEMORY_BASIC_INFORMATION mi = {};
    if (!VirtualQuery(at, &mi, sizeof(mi)) || mi.State != MEM_COMMIT || memcmp(at, expect, sizeof(expect)) != 0) {
        BDX_INFO("Game heap: unknown Bully.exe, left at 64 MB");
        return;
    }
    const uint32_t bytes = static_cast<uint32_t>((std::min)(mb, 1024)) * 1024u * 1024u;
    DWORD old = 0;
    if (!VirtualProtect(at + 6, 4, PAGE_EXECUTE_READWRITE, &old)) return;
    memcpy(at + 6, &bytes, 4);
    DWORD tmp = 0;
    VirtualProtect(at + 6, 4, old, &tmp);
    FlushInstructionCache(GetCurrentProcess(), at + 6, 4);
    BDX_INFO("Game heap: 64 MB -> %u MB (streamed models and the world's pools)", bytes / (1024u * 1024u));
}

static void LogGameExe()
{
    wchar_t path[MAX_PATH * 2] = {};
    GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
    WIN32_FILE_ATTRIBUTE_DATA fa = {};
    const ULONGLONG size = GetFileAttributesExW(path, GetFileExInfoStandard, &fa) ? (static_cast<ULONGLONG>(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow : 0;
    const auto* base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
    DWORD stamp = 0;
    if (base) {
        const LONG pe = *reinterpret_cast<const LONG*>(base + 0x3C);
        stamp = *reinterpret_cast<const DWORD*>(base + pe + 8);
    }
    OSVERSIONINFOEXW os = {sizeof(os)};
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOEXW*);
    if (auto fn = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"))) fn(&os);
    BDX_INFO("Game exe: %ls, %llu bytes, link time 0x%08lX, loaded at %p (the mod knows the Steam 1.200 exe, at 0x00400000); Windows %lu.%lu build %lu",
             path, size, static_cast<unsigned long>(stamp), base, os.dwMajorVersion, os.dwMinorVersion, os.dwBuildNumber);
}

static LPTOP_LEVEL_EXCEPTION_FILTER g_prevCrashFilter = nullptr;
static LONG WINAPI CrashFilter(EXCEPTION_POINTERS* ep)
{
    if (ep && ep->ExceptionRecord) {
        const EXCEPTION_RECORD* er = ep->ExceptionRecord;
        HMODULE mod = nullptr;
        char name[MAX_PATH] = "?";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               static_cast<LPCSTR>(er->ExceptionAddress), &mod) && mod)
            GetModuleFileNameA(mod, name, MAX_PATH);
        const char* shortName = strrchr(name, '\\') ? strrchr(name, '\\') + 1 : name;
        const uintptr_t off = mod ? reinterpret_cast<uintptr_t>(er->ExceptionAddress) - reinterpret_cast<uintptr_t>(mod) : 0;
        BDX_ERROR("CRASH: exception 0x%08lX at %p = %s+0x%IX (access %Iu at %p)", static_cast<unsigned long>(er->ExceptionCode), er->ExceptionAddress,
                  shortName, off, er->NumberParameters > 0 ? er->ExceptionInformation[0] : 0,
                  er->NumberParameters > 1 ? reinterpret_cast<void*>(er->ExceptionInformation[1]) : nullptr);
    }
    return g_prevCrashFilter ? g_prevCrashFilter(ep) : EXCEPTION_CONTINUE_SEARCH;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved)
{
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(module);
        Startup("d3d9.dll loaded (DllMain)");
        MutableConfig().Load();
        Startup("BullyDX12.ini read");
        LogInit(GameDirectory() + L"BullyDX12.log", GetConfig().logLevel);
        Startup("log opened");
        LogGameExe();
        g_prevCrashFilter = SetUnhandledExceptionFilter(&CrashFilter);
        if (GetConfig().GetInt("Display", "DpiAware", 1) != 0) {
            const BOOL ok = SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) ||
                            SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE);
            BDX_INFO("Display: process DPI awareness %s", ok ? "enabled (per monitor)" : "unchanged (already set)");
        }
        Display::Get().LoadConfig();
        Display::Get().InstallModeChangeHooks();
        Startup("display hooks installed");
        GrowGameHeap();
        Startup("DllMain done");
        break;
    case DLL_PROCESS_DETACH:
        (void)reserved;
        BDX_INFO("BullyDX12 detached (process exit)");
        LogShutdown();
        break;
    }
    return TRUE;
}

extern "C" BOOL WINAPI _DllMainCRTStartup(HINSTANCE, DWORD, LPVOID);
extern "C" BOOL WINAPI BDX_EntryPoint(HINSTANCE module, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH) Startup("d3d9.dll entry point reached (before the C runtime)");
    return _DllMainCRTStartup(module, reason, reserved);
}
