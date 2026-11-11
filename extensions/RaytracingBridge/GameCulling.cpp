// Bully: Scholarship Edition (PC, 1.200). The world is drawn by 0x515F50 (twice per frame, same camera); its culler
// MdBullyCullingProcess (vtable 0x91758C) walks the scene in 0x5158A0 and tests every object with:
// 0x515660  IsVisible(planes, object in esi): app-culled flag (+0xA8 bit 0), day/night flags (+0xB0, chosen by
// [0xC675F0]), then the bound sphere (+0x1C centre, +0x28 radius, + 1.0) against six planes.
// 0x515770  Process(object), the NiCullingProcess path: the same six planes with a mask of planes still to test.
// The patches keep the flags and drop only the planes:
// 0x515694  after the flags pass: "mov al, 1 / pop ecx / ret" in place of "fld [esi+28h] / mov ecx, [esp+8] / ..."
// 0x515780  the "jne" into the plane loop becomes two nops: every object goes straight to OnVisible.

#include "GameCulling.h"

#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace gameculling {
namespace {

struct Patch {
    uintptr_t check;
    uint8_t   expect[9];
    size_t    expectLen;
    uintptr_t write;
    uint8_t   with[4];
    size_t    withLen;
    uint8_t   saved[4];
    bool      applied;
};

Patch g_patches[] = {
    {0x515694, {0xD9, 0x46, 0x28, 0x8B, 0x4C, 0x24, 0x08, 0xDC, 0x05}, 9, 0x515694, {0xB0, 0x01, 0x59, 0xC3}, 4, {}, false},
    {0x51577A, {0x85, 0xC0, 0x89, 0x44, 0x24, 0x04, 0x75, 0x14}, 8, 0x515780, {0x90, 0x90}, 2, {}, false},
};
bool g_active = false;

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

}

bool DisableFrustum(const std::function<void(const std::string&)>& log)
{
    if (g_active) return true;
    if (reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)) != 0x400000) {
        log("game culling: Bully.exe not at its usual base, the game keeps its own culling");
        return false;
    }
    for (const Patch& p : g_patches) {
        MEMORY_BASIC_INFORMATION mi = {};
        const void* at = reinterpret_cast<const void*>(p.check);
        if (!VirtualQuery(at, &mi, sizeof(mi)) || mi.State != MEM_COMMIT || memcmp(at, p.expect, p.expectLen) != 0) {
            char line[160];
            snprintf(line, sizeof(line), "game culling: unknown code at 0x%06X, the game keeps its own culling", static_cast<unsigned>(p.check));
            log(line);
            return false;
        }
    }
    for (Patch& p : g_patches) {
        memcpy(p.saved, reinterpret_cast<const void*>(p.write), p.withLen);
        if (!Write(p.write, p.with, p.withLen)) {
            Restore();
            log("game culling: could not write the game's code, it keeps its own culling");
            return false;
        }
        p.applied = true;
    }
    g_active = true;
    log("game culling: the game's frustum test is off; every loaded object is handed on, RaytracingBridge culls the raster");
    return true;
}

void Restore()
{
    for (Patch& p : g_patches) {
        if (p.applied && Write(p.write, p.saved, p.withLen)) p.applied = false;
    }
    g_active = false;
}

bool Active() { return g_active; }

}
