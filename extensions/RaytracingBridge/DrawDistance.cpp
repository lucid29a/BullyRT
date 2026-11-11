// Bully: Scholarship Edition (PC, 1.200).
// 0x527440  CBaseModelInfo::SetLodDistance(float): fld [esp+4] / fstp [ecx+24h] / ret 4, then 6 bytes of padding up to
// the next function at 0x527450. Called by the IDE loaders (objs 0x42AF15, tobj 0x42B49B, 0x436401, 0x436550).
// 0xC67738  the model table, up to 0xC735D8; objs models have the vtable 0x917B00.
// The patch, 16 bytes in place of the 10 and the padding:
// fld [esp+4] / fmul dword ptr [factor] / fstp [ecx+24h] / ret 4
// The factor lives in a page of its own that is never freed, so the game's code never points into this DLL.

#include "DrawDistance.h"

#include <windows.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace drawdistance {
namespace {

constexpr uintptr_t kSetter = 0x527440;
const uint8_t kOriginal[16] = {0xD9, 0x44, 0x24, 0x04, 0xD9, 0x59, 0x24, 0xC2, 0x04, 0x00, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC};
constexpr uintptr_t kTable = 0xC67738, kTableEnd = 0xC735D8;
constexpr uint32_t  kObjsVtable = 0x917B00;

constexpr uintptr_t kPoolSizeImm = 0x44D74D;
constexpr uintptr_t kObjectPool  = 0xC0F608;
constexpr uintptr_t kNewCall     = 0x4F0000;
constexpr uintptr_t kPoolNew     = 0x4EB100;

float*   g_factor = nullptr;
uint8_t* g_stub   = nullptr;
bool     g_applied = false;

bool Write(uintptr_t at, const uint8_t* bytes, size_t len)
{
    uint8_t* where = reinterpret_cast<uint8_t*>(at);
    DWORD old = 0;
    if (!VirtualProtect(where, len, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(where, bytes, len);
    DWORD tmp = 0;
    VirtualProtect(where, len, old, &tmp);
    FlushInstructionCache(GetCurrentProcess(), where, len);
    return true;
}

bool Readable(uintptr_t at, size_t len)
{
    MEMORY_BASIC_INFORMATION mi = {};
    return VirtualQuery(reinterpret_cast<const void*>(at), &mi, sizeof(mi)) && mi.State == MEM_COMMIT &&
           at + len <= reinterpret_cast<uintptr_t>(mi.BaseAddress) + mi.RegionSize;
}

}

bool Apply(float factor, const std::function<void(const std::string&)>& log)
{
    if (g_applied || factor == 1.0f) return g_applied;
    if (!(factor > 0.1f && factor <= 20.0f)) { log("draw distance: factor out of range (0.1-20), left as the game has it"); return false; }
    if (reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)) != 0x400000 || !Readable(kSetter, sizeof(kOriginal)) ||
        memcmp(reinterpret_cast<const void*>(kSetter), kOriginal, sizeof(kOriginal)) != 0) {
        log("draw distance: unknown Bully.exe, left as the game has it");
        return false;
    }
    if (!g_factor) g_factor = static_cast<float*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!g_factor) { log("draw distance: no memory for the factor, left as the game has it"); return false; }
    *g_factor = factor;

    const uint32_t addr = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(g_factor));
    uint8_t code[16] = {0xD9, 0x44, 0x24, 0x04,
                        0xD8, 0x0D, 0, 0, 0, 0,
                        0xD9, 0x59, 0x24,
                        0xC2, 0x04, 0x00};
    memcpy(code + 6, &addr, 4);
    if (!Write(kSetter, code, sizeof(code))) { log("draw distance: could not write the game's code, left as it is"); return false; }
    g_applied = true;

    int already = 0, scaled = 0;
    if (Readable(kTable, kTableEnd - kTable)) {
        for (uintptr_t p = kTable; p < kTableEnd; p += 4) {
            const uintptr_t mi = *reinterpret_cast<const uintptr_t*>(p);
            if (!mi) continue;
            ++already;
            if (Readable(mi, 0x28) && *reinterpret_cast<const uint32_t*>(mi) == kObjsVtable) {
                *reinterpret_cast<float*>(mi + 0x24) *= factor;
                ++scaled;
            }
        }
    }
    char line[200];
    snprintf(line, sizeof(line), "draw distance: objects keep their detailed model x%.2f farther (%d models were loaded already, %d of them scaled now)",
             factor, already, scaled);
    log(line);

    const uint8_t callNew[5] = {0xE8, 0xFB, 0xB0, 0xFF, 0xFF};
    if (Readable(kNewCall, 5) && memcmp(reinterpret_cast<const void*>(kNewCall), callNew, 5) == 0) {
        if (!g_stub) g_stub = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        if (g_stub) {
            uint8_t s[] = {0xE8, 0, 0, 0, 0,
                           0x85, 0xC0,
                           0x74, 0x01,
                           0xC3,
                           0x83, 0xC4, 0x04,
                           0x5F, 0x5B, 0x5E,
                           0x33, 0xC0,
                           0x5D,
                           0x81, 0xC4, 0xE8, 0x00, 0x00, 0x00,
                           0xC3};
            const int32_t rel = static_cast<int32_t>(kPoolNew - (reinterpret_cast<uintptr_t>(g_stub) + 5));
            memcpy(s + 1, &rel, 4);
            memcpy(g_stub, s, sizeof(s));
            FlushInstructionCache(GetCurrentProcess(), g_stub, sizeof(s));
            uint8_t call[5] = {0xE8};
            const int32_t relCall = static_cast<int32_t>(reinterpret_cast<uintptr_t>(g_stub) - (kNewCall + 5));
            memcpy(call + 1, &relCall, 4);
            if (Write(kNewCall, call, 5)) log("draw distance: a full object pool leaves the object out instead of crashing");
        }
    }
    return true;
}

void Restore()
{
    if (g_applied && Write(kSetter, kOriginal, sizeof(kOriginal))) g_applied = false;
}

}
