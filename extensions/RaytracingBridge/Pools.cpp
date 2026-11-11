#include "Pools.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace pools {
namespace {

constexpr uintptr_t kWorld = 0x20C7C48;

struct Site {
    uintptr_t at;
    uint8_t   op[3];
    uint8_t   opLen;
    uint32_t  value;
    uint32_t  bytes;
};

struct PoolDef {
    const char* name;
    uintptr_t   global;
    uint32_t    capacity;
    Site        sites[3];
    int         siteCount;
};

const PoolDef kPools[] = {
    {"objects", 0xC0F608, 220, {{0x44D74A, {0xC7, 0x46, 0x08}, 3, 220, 1}}, 1},
    {"entities", kWorld + 0x3C, 490,
     {{0x6D40BC, {0xC7, 0x46, 0x08}, 3, 490, 1}, {0x6D3F09, {0xB8}, 1, 490, 1}, {0x6D3F4D, {0x3D}, 1, 490 * 2, 2}}, 3},
    {"world+40", kWorld + 0x40, 1550, {{0x6D3FA4, {0xBF}, 1, 1550, 1}, {0x6D3FED, {0x3D}, 1, 1550 * 2, 2}}, 2},
    {"world+44", kWorld + 0x44, 800,
     {{0x6D3F56, {0xB8}, 1, 800, 1}, {0x6D3F9D, {0x3D}, 1, 800 * 2, 2}, {0x6D4113, {0xBF}, 1, 800, 1}}, 3},
    {"world+48", kWorld + 0x48, 2350,
     {{0x6D415E, {0xC7, 0x46, 0x08}, 3, 2350, 1}, {0x6D3FF6, {0xB8}, 1, 2350, 1}, {0x6D403D, {0x3D}, 1, 2350 * 2, 2}}, 3},
    {"world+4C", kWorld + 0x4C, 200,
     {{0x6D44B9, {0xC7, 0x46, 0x08}, 3, 200, 1}, {0x6D4046, {0xB8}, 1, 200, 1}, {0x6D408D, {0x3D}, 1, 200 * 2, 2}}, 3},
    {"world+04", kWorld + 0x04, 350, {{0x6D41C8, {0xC7, 0x46, 0x08}, 3, 350, 1}}, 1},
    {"world+08", kWorld + 0x08, 200, {{0x6D41FF, {0xC7, 0x46, 0x08}, 3, 200, 1}}, 1},
    {"world+10", kWorld + 0x10, 250, {{0x6D4269, {0xC7, 0x46, 0x08}, 3, 250, 1}}, 1},
    {"world+14", kWorld + 0x14, 100, {{0x6D42A0, {0xC7, 0x46, 0x08}, 3, 100, 1}}, 1},
    {"world+18", kWorld + 0x18, 80, {{0x6D42D7, {0xC7, 0x46, 0x08}, 3, 80, 1}}, 1},
    {"nodes", 0xC0F5EC, 2000, {{0x44D38A, {0xC7, 0x46, 0x08}, 3, 2000, 1}}, 1},
};
constexpr int kPoolCount = sizeof(kPools) / sizeof(kPools[0]);
const PoolDef kWorld0 = {"world+00", kWorld + 0x00, 800, {}, 0};

uint32_t g_capacity[kPoolCount + 1] = {};
uint32_t g_peak[kPoolCount + 1] = {};
bool     g_applied = false;

bool Readable(uintptr_t at, size_t len)
{
    MEMORY_BASIC_INFORMATION mi = {};
    return VirtualQuery(reinterpret_cast<const void*>(at), &mi, sizeof(mi)) && mi.State == MEM_COMMIT &&
           !(mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) && at + len <= reinterpret_cast<uintptr_t>(mi.BaseAddress) + mi.RegionSize;
}

bool Write(uintptr_t at, const void* bytes, size_t len)
{
    void* where = reinterpret_cast<void*>(at);
    DWORD old = 0;
    if (!VirtualProtect(where, len, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(where, bytes, len);
    DWORD tmp = 0;
    VirtualProtect(where, len, old, &tmp);
    FlushInstructionCache(GetCurrentProcess(), where, len);
    return true;
}

bool SiteKnown(const Site& s)
{
    uint8_t expect[8] = {};
    memcpy(expect, s.op, s.opLen);
    memcpy(expect + s.opLen, &s.value, 4);
    return Readable(s.at, s.opLen + 4u) && memcmp(reinterpret_cast<const void*>(s.at), expect, s.opLen + 4u) == 0;
}

bool Made(uintptr_t global) { return Readable(global, 4) && *reinterpret_cast<const uint32_t*>(global) != 0; }

}

void Apply(int scale, const std::function<void(const std::string&)>& log)
{
    for (int i = 0; i < kPoolCount; ++i) g_capacity[i] = kPools[i].capacity;
    g_capacity[kPoolCount] = kWorld0.capacity;
    scale = std::clamp(scale, 1, 16);
    if (g_applied || scale == 1) return;
    if (reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)) != 0x400000) { log("pools: unknown Bully.exe, the game keeps its pools"); return; }

    for (const PoolDef& p : kPools) {
        for (int s = 0; s < p.siteCount; ++s) {
            if (!SiteKnown(p.sites[s])) {
                char line[160];
                snprintf(line, sizeof(line), "pools: unknown code at 0x%06X (%s), the game keeps its pools", static_cast<unsigned>(p.sites[s].at), p.name);
                log(line);
                return;
            }
        }
        if (Made(p.global)) { log(std::string("pools: the game made its pools already (") + p.name + "), they keep their size"); return; }
    }

    std::string line = "pools: x" + std::to_string(scale) + ":";
    for (int i = 0; i < kPoolCount; ++i) {
        const PoolDef& p = kPools[i];
        const uint32_t cap = std::min<uint32_t>(p.capacity * static_cast<uint32_t>(scale), 60000u);
        for (int s = 0; s < p.siteCount; ++s) {
            const uint32_t v = cap * p.sites[s].bytes;
            if (!Write(p.sites[s].at + p.sites[s].opLen, &v, 4)) { log(std::string("pools: WRITE FAILED at ") + p.name); return; }
        }
        g_capacity[i] = cap;
        char part[64];
        snprintf(part, sizeof(part), " %s %u->%u", p.name, p.capacity, cap);
        line += part;
    }
    g_capacity[kPoolCount] = g_capacity[3];
    line += " (world+00 with world+44)";
    g_applied = true;
    log(line);
}

void Report(const std::function<void(const std::string&)>& log)
{
    std::string line = "pools in use:", warn;
    for (int i = 0; i <= kPoolCount; ++i) {
        const PoolDef& p = i < kPoolCount ? kPools[i] : kWorld0;
        if (!Readable(p.global, 4)) continue;
        const uintptr_t pool = *reinterpret_cast<const uint32_t*>(p.global);
        if (!pool || !Readable(pool, 0x1C)) continue;
        const uintptr_t flags = *reinterpret_cast<const uint32_t*>(pool + 4);
        const uint32_t  cap   = *reinterpret_cast<const uint32_t*>(pool + 8);
        if (!flags || !cap || cap > 100000 || !Readable(flags, cap)) continue;
        uint32_t used = 0;
        const uint8_t* f = reinterpret_cast<const uint8_t*>(flags);
        for (uint32_t k = 0; k < cap; ++k) used += (f[k] & 0x80) ? 0u : 1u;
        g_peak[i] = std::max(g_peak[i], used);
        char part[80];
        snprintf(part, sizeof(part), " %s %u/%u (peak %u)", p.name, used, cap, g_peak[i]);
        line += part;
        if (g_peak[i] * 100 > cap * 85) warn += std::string(" ") + p.name;
    }
    log(line);
    if (!warn.empty()) log("pools: NEAR FULL (peak over 85%):" + warn + " - raise [Culling] PoolScale");
}

}
