// Bully: Scholarship Edition (PC, 1.200). Gamebryo reports its failures through three functions:
// 0x756700  NiRenderer error (int level, int code, const char* fmt, ...): "Failed CreatePixelShader call on %s", ...
// It starts with "sub esp, 804h / mov eax, [0BC9A04h]" (11 bytes), copied into the stub below.
// 0x888620  NiOutputDebugString (const char* fmt, ...), off unless [0xBCB8C4]: "LoadD3DXEffectFiles failed on %s", ...
// It starts with "sub esp, 404h / mov eax, [0BC9A04h]" (11 bytes).
// 0x49A650  a debug printf (const char* fmt, ...) that the retail game emptied to a lone "ret" (15 int3 behind it):
// "NiD3DXEffectShader::PreProcessPipeline - %s - Effect failed to load", ...
// The stubs log the message, then run what they replaced.

#include "GameErrors.h"

#include <windows.h>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace gameerrors {
namespace {

std::function<void(const std::string&)> g_log;
std::atomic<int> g_count[3];

bool SafeFormat(char* buf, size_t size, const char* fmt, va_list va)
{
    __try {
        vsnprintf(buf, size, fmt, va);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void Emit(int ch, const char* channel, const char* fmt, va_list va)
{
    if (!g_log || !fmt) return;
    if (g_count[ch].fetch_add(1) > 200) return;
    char buf[600];
    if (!SafeFormat(buf, sizeof(buf), fmt, va)) snprintf(buf, sizeof(buf), "(unreadable message)");
    size_t n = strlen(buf);
    while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = 0;
    char line[700];
    snprintf(line, sizeof(line), "game %s: %s", channel, buf);
    g_log(line);
}

void __cdecl LogRenderer(const uint32_t* args) { Emit(0, "renderer", reinterpret_cast<const char*>(args[2]), reinterpret_cast<va_list>(const_cast<uint32_t*>(args + 3))); }
void __cdecl LogDebug(const uint32_t* args)    { Emit(1, "debug", reinterpret_cast<const char*>(args[0]), reinterpret_cast<va_list>(const_cast<uint32_t*>(args + 1))); }
void __cdecl LogPrintf(const uint32_t* args)   { Emit(2, "shader", reinterpret_cast<const char*>(args[0]), reinterpret_cast<va_list>(const_cast<uint32_t*>(args + 1))); }

__declspec(naked) void StubRenderer()
{
    __asm {
        pushad
        pushfd
        lea  eax, [esp + 40]
        push eax
        call LogRenderer
        add  esp, 4
        popfd
        popad
        sub  esp, 0x804
        mov  eax, dword ptr ds:[0xBC9A04]
        push 0x75670B
        ret
    }
}

__declspec(naked) void StubDebug()
{
    __asm {
        pushad
        pushfd
        lea  eax, [esp + 40]
        push eax
        call LogDebug
        add  esp, 4
        popfd
        popad
        sub  esp, 0x404
        mov  eax, dword ptr ds:[0xBC9A04]
        push 0x88862B
        ret
    }
}

__declspec(naked) void StubPrintf()
{
    __asm {
        pushad
        pushfd
        lea  eax, [esp + 40]
        push eax
        call LogPrintf
        add  esp, 4
        popfd
        popad
        ret
    }
}

std::atomic<int> g_skipped{0};
void __cdecl NoteSkipped() { if (g_skipped.fetch_add(1) < 5 && g_log) g_log("game errors: a character mesh has no material (its shader failed): skin setup skipped"); }

__declspec(naked) void StubSkinMaterial()
{
    __asm {
        mov  ecx, ebx
        mov  eax, 0x761630
        call eax
        test eax, eax
        jz   none
        push eax
        push 0x20DCE10
        mov  eax, 0x401FF0
        call eax
        add  esp, 8
        test eax, eax
        jz   none
        mov  esi, dword ptr [eax + 0x6C]
        push 0x408120
        ret
    none:
        call NoteSkipped
        push 0x408218
        ret
    }
}

using CreateEffectFn = HRESULT(WINAPI*)(void* dev, const void* data, UINT len, const void* defines, void* include, DWORD flags,
                                       void* pool, void** effect, void** errors);
CreateEffectFn g_realCreateEffect = nullptr;
std::atomic<int> g_effectFails{0}, g_effectRetried{0};
constexpr DWORD kLargeAddressAware = 1u << 17;

HRESULT WINAPI CreateEffectHook(void* dev, const void* data, UINT len, const void* defines, void* include, DWORD flags,
                                void* pool, void** effect, void** errors)
{
    HRESULT hr = g_realCreateEffect(dev, data, len, defines, include, flags, pool, effect, errors);
    if (SUCCEEDED(hr)) return hr;
    const int n = g_effectFails.fetch_add(1);
    HRESULT hr2 = hr;
    if (!(flags & kLargeAddressAware)) {
        if (errors && *errors) { reinterpret_cast<IUnknown*>(*errors)->Release(); *errors = nullptr; }
        hr2 = g_realCreateEffect(dev, data, len, defines, include, flags | kLargeAddressAware, pool, effect, errors);
        if (SUCCEEDED(hr2)) g_effectRetried.fetch_add(1);
    }
    if (n < 12 && g_log) {
        char line[260];
        snprintf(line, sizeof(line), "game errors: D3DXCreateEffect failed 0x%08lX (device %p, data %p, %u bytes, flags 0x%lX); with LARGEADDRESSAWARE: %s 0x%08lX",
                 static_cast<unsigned long>(hr), dev, data, len, static_cast<unsigned long>(flags), SUCCEEDED(hr2) ? "made" : "failed too",
                 static_cast<unsigned long>(hr2));
        g_log(line);
    }
    return hr2;
}

bool Matches(uintptr_t at, const uint8_t* bytes, size_t len) { return memcmp(reinterpret_cast<const void*>(at), bytes, len) == 0; }

bool Jump(uintptr_t at, void* to, size_t len)
{
    DWORD old = 0, tmp = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(at), len, PAGE_EXECUTE_READWRITE, &old)) return false;
    uint8_t* p = reinterpret_cast<uint8_t*>(at);
    p[0] = 0xE9;
    const int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(to) - (at + 5));
    memcpy(p + 1, &rel, 4);
    for (size_t i = 5; i < len; ++i) p[i] = 0x90;
    VirtualProtect(reinterpret_cast<void*>(at), len, old, &tmp);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(at), len);
    return true;
}

}

void Install(std::function<void(const std::string&)> log)
{
    if (reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)) != 0x400000) return;
    g_log = std::move(log);
    static const uint8_t kRenderer[11] = {0x81, 0xEC, 0x04, 0x08, 0x00, 0x00, 0xA1, 0x04, 0x9A, 0xBC, 0x00};
    static const uint8_t kDebug[11]    = {0x81, 0xEC, 0x04, 0x04, 0x00, 0x00, 0xA1, 0x04, 0x9A, 0xBC, 0x00};
    static const uint8_t kPrintf[6]    = {0xC3, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC};
    int n = 0;
    if (Matches(0x756700, kRenderer, 11) && Jump(0x756700, reinterpret_cast<void*>(&StubRenderer), 11)) ++n;
    if (Matches(0x888620, kDebug, 11) && Jump(0x888620, reinterpret_cast<void*>(&StubDebug), 11)) ++n;
    (void)kPrintf;
    static const uint8_t kSkin[18] = {0x8B, 0xCB, 0xE8, 0x21, 0x95, 0x35, 0x00, 0x50, 0x68, 0x10, 0xCE, 0x0D, 0x02, 0xE8, 0xD6, 0x9E, 0xFF, 0xFF};
    const bool guard = Matches(0x408108, kSkin, 18) && Jump(0x408108, reinterpret_cast<void*>(&StubSkinMaterial), 5);
    {
        void** slot = reinterpret_cast<void**>(0x8FC2E8);
        HMODULE dx = GetModuleHandleW(L"d3dx9_38.dll");
        void* real = dx ? reinterpret_cast<void*>(GetProcAddress(dx, "D3DXCreateEffect")) : nullptr;
        DWORD old = 0, tmp = 0;
        if (real && *slot == real && VirtualProtect(slot, 4, PAGE_READWRITE, &old)) {
            g_realCreateEffect = reinterpret_cast<CreateEffectFn>(real);
            *slot = reinterpret_cast<void*>(&CreateEffectHook);
            VirtualProtect(slot, 4, old, &tmp);
            g_log("game errors: D3DXCreateEffect wrapped (failures logged, retried large-address aware)");
        } else {
            g_log("game errors: D3DXCreateEffect NOT wrapped (import not as expected)");
        }
    }
    char line[200];
    snprintf(line, sizeof(line), "game errors: guard for a character mesh without material %s", guard ? "on" : "NOT on (unknown code)");
    g_log(line);
    snprintf(line, sizeof(line), "game errors: %d of 2 Gamebryo message functions now write to this log", n);
    g_log(line);
}

}
