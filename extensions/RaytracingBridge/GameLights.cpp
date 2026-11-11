#include "GameLights.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace gamelights {
namespace {

constexpr uintptr_t kRegisterCorona = 0x511130, kAddLight = 0x522DB0;
const uint8_t kCoronaStart[8] = {0x83, 0xEC, 0x18, 0xA1, 0xF4, 0xCC, 0xC3, 0x00};
const uint8_t kLightStart[6]  = {0x8B, 0x0D, 0x68, 0x26, 0xC9, 0x00};
constexpr uint32_t kFromObjectLights = 0x512D0B, kFromSun = 0x512693, kFromWrapper = 0x51253B, kWrapperFromMoon = 0x512758;

struct Corona { uint32_t id; float pos[3]; uint8_t rgb[3], intensity; float size, range, pool; uint8_t object; };
struct Point  { float pos[3], radius, rgb[3], dir[3]; bool beam; };
bool FromVehicle(uint32_t ret) { return (ret >= 0x4B2540 && ret < 0x4B5400) || (ret >= 0x4C21D0 && ret < 0x4C4000); }

constexpr int kMaxCoronas = 1024, kMaxPoints = 512;
Corona   g_cor[2][kMaxCoronas];
Point    g_pt[2][kMaxPoints];
uint32_t g_corCount[2] = {}, g_ptCount[2] = {};
int      g_fill = 0;
uint32_t g_seen[4096];

uint8_t* g_stubs = nullptr;
bool     g_active = false;
float    g_power = 3.0f, g_reach = 1.5f, g_carPower = 1.5f;
std::vector<Lamp> g_lamps;
Stats    g_stats;

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

bool Readable(uintptr_t at, size_t len)
{
    MEMORY_BASIC_INFORMATION mi = {};
    return VirtualQuery(reinterpret_cast<const void*>(at), &mi, sizeof(mi)) && mi.State == MEM_COMMIT &&
           !(mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) && at + len <= reinterpret_cast<uintptr_t>(mi.BaseAddress) + mi.RegionSize;
}

float F(uint32_t v) { float f; memcpy(&f, &v, 4); return f; }
bool  Finite(const float* v, int n) { for (int i = 0; i < n; ++i) if (!std::isfinite(v[i]) || std::fabs(v[i]) > 1e5f) return false; return true; }

bool FirstThisFrame(uint32_t id)
{
    uint32_t h = (id * 2654435761u) >> 20;
    for (int probe = 0; probe < 4096; ++probe, h = (h + 1) & 4095) {
        if (g_seen[h] == id) return false;
        if (!g_seen[h]) { g_seen[h] = id; return true; }
    }
    return false;
}

void __cdecl OnCorona(const uint32_t* regs)
{
    __try {
        const uint32_t from = regs[8];
        const uint32_t* a = regs + 9;
        const uint32_t id = a[0];
        if (id <= 2 || from == kFromSun) return;
        if (from == kFromWrapper) return;
        const uint8_t r = static_cast<uint8_t>(a[1]), g = static_cast<uint8_t>(a[2]), b = static_cast<uint8_t>(a[3]), in = static_cast<uint8_t>(a[4]);
        if (!(r | g | b) || !in) return;
        const float* p = reinterpret_cast<const float*>(a[5]);
        if (!p) return;
        Corona c = {};
        c.id = id;
        memcpy(c.pos, p, sizeof(c.pos));
        if (!Finite(c.pos, 3)) return;
        c.rgb[0] = r; c.rgb[1] = g; c.rgb[2] = b; c.intensity = in;
        c.size = F(a[6]);
        if (from == kFromObjectLights && regs[1]) {
            const uint8_t* fx = reinterpret_cast<const uint8_t*>(regs[1]);
            memcpy(&c.range, fx + 0x20, 4);
            memcpy(&c.pool, fx + 0x28, 4);
            c.object = 1;
        }
        if (!std::isfinite(c.size)) c.size = 1.0f;
        if (!std::isfinite(c.range) || c.range < 0.0f) c.range = 0.0f;
        if (!std::isfinite(c.pool) || c.pool < 0.0f) c.pool = 0.0f;
        uint32_t& n = g_corCount[g_fill];
        if (n < kMaxCoronas && FirstThisFrame(id)) g_cor[g_fill][n++] = c;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void __cdecl OnAddLight(const uint32_t* regs)
{
    __try {
        const uint32_t* a = regs + 9;
        const uint8_t type = static_cast<uint8_t>(a[0]);
        const bool vehicle = FromVehicle(regs[8]);
        if (type == 0 && vehicle) return;
        if (type != 0 && !(type == 1 && vehicle)) return;
        Point p = {};
        for (int i = 0; i < 3; ++i) { p.pos[i] = F(a[1 + i]); p.dir[i] = F(a[4 + i]); p.rgb[i] = F(a[8 + i]); }
        p.radius = F(a[7]);
        if (!Finite(p.pos, 3) || !Finite(p.rgb, 3) || !Finite(p.dir, 3) || !std::isfinite(p.radius) || p.radius <= 0.0f) return;
        if (type == 1) {
            const float len = std::sqrt(p.dir[0] * p.dir[0] + p.dir[1] * p.dir[1] + p.dir[2] * p.dir[2]);
            if (len < 1e-3f) return;
            for (float& d : p.dir) d /= len;
            p.beam = true;
        }
        if (p.rgb[0] + p.rgb[1] + p.rgb[2] <= 1e-4f) return;
        uint32_t& n = g_ptCount[g_fill];
        if (n < kMaxPoints) g_pt[g_fill][n++] = p;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

uint8_t* MakeStub(uint8_t* at, void (__cdecl* handler)(const uint32_t*), const uint8_t* original, size_t len, uintptr_t back)
{
    uint8_t* s = at;
    *s++ = 0x60;
    *s++ = 0x54;
    *s++ = 0xE8;
    const int32_t relCall = static_cast<int32_t>(reinterpret_cast<uintptr_t>(handler) - (reinterpret_cast<uintptr_t>(s) + 4));
    memcpy(s, &relCall, 4); s += 4;
    *s++ = 0x83; *s++ = 0xC4; *s++ = 0x04;
    *s++ = 0x61;
    memcpy(s, original, len); s += len;
    *s++ = 0xE9;
    const int32_t relBack = static_cast<int32_t>(back - (reinterpret_cast<uintptr_t>(s) + 4));
    memcpy(s, &relBack, 4); s += 4;
    return s;
}

bool JumpTo(uintptr_t from, const uint8_t* to, size_t len)
{
    uint8_t code[8];
    memset(code, 0x90, sizeof(code));
    code[0] = 0xE9;
    const int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(to) - (from + 5));
    memcpy(code + 1, &rel, 4);
    return Write(from, code, len);
}

uint32_t KeyOf(const float* p)
{
    uint32_t h = 2166136261u;
    for (int i = 0; i < 3; ++i) { h ^= static_cast<uint32_t>(static_cast<int32_t>(std::floor(p[i] * 2.0f))); h *= 16777619u; }
    return h | 1u;
}

}

bool Install(const std::function<void(const std::string&)>& log)
{
    if (g_active) return true;
    if (reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)) != 0x400000 || !Readable(kRegisterCorona, 8) || !Readable(kAddLight, 6) ||
        memcmp(reinterpret_cast<const void*>(kRegisterCorona), kCoronaStart, 8) != 0 || memcmp(reinterpret_cast<const void*>(kAddLight), kLightStart, 6) != 0) {
        log("game lights: unknown Bully.exe, the game's lights are not read (the rays use the lights its shaders report)");
        return false;
    }
    if (!g_stubs) g_stubs = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!g_stubs) { log("game lights: no memory for the hooks"); return false; }
    memset(g_seen, 0, sizeof(g_seen));
    uint8_t* coronaStub = g_stubs;
    uint8_t* lightStub  = g_stubs + 64;
    MakeStub(coronaStub, &OnCorona, kCoronaStart, 8, kRegisterCorona + 8);
    MakeStub(lightStub, &OnAddLight, kLightStart, 6, kAddLight + 6);
    FlushInstructionCache(GetCurrentProcess(), g_stubs, 128);
    if (!JumpTo(kRegisterCorona, coronaStub, 8)) { log("game lights: could not write the game's code"); return false; }
    if (!JumpTo(kAddLight, lightStub, 6)) { Write(kRegisterCorona, kCoronaStart, 8); log("game lights: could not write the game's code"); return false; }
    g_active = true;
    log("game lights: reading every light the game turns on (its coronas at 0x511130 and its point lights at 0x522DB0)");
    return true;
}

bool SetLampHours(float onHour, float offHour, const std::function<void(const std::string&)>& log)
{
    constexpr uintptr_t kOff = 0xA645C4, kOn = 0xA645D4;
    const float gameOff[4] = {7.0f, 7.0f, 7.0f, 7.0f}, gameOn[4] = {20.0f, 20.0f, 19.0f, 21.0f};
    static bool s_ours = false;
    if (reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)) != 0x400000 || !Readable(kOff, 32) ||
        (!s_ours && (memcmp(reinterpret_cast<const void*>(kOff), gameOff, 16) != 0 || memcmp(reinterpret_cast<const void*>(kOn), gameOn, 16) != 0))) {
        log("game lights: the game's lamp hours are not the known ones, left as they are");
        return false;
    }
    onHour = std::clamp(onHour, 12.0f, 24.0f);
    offHour = std::clamp(offHour, 0.0f, 12.0f);
    const float t = onHour + 1.0f;
    const float on[4] = {t, t, t, t}, off[4] = {offHour, offHour, offHour, offHour};
    if (!Write(kOn, on, 16) || !Write(kOff, off, 16)) { log("game lights: could not write the lamp hours"); return false; }
    s_ours = true;
    char line[160];
    snprintf(line, sizeof(line), "game lights: the game lights its night lamps from %.0f:00 to %.0f:00 in every season (it had 19/18/20 to 7)", onHour, offHour);
    log(line);
    return true;
}

void Restore()
{
    if (!g_active) return;
    Write(kRegisterCorona, kCoronaStart, 8);
    Write(kAddLight, kLightStart, 6);
    g_active = false;
}

namespace { bool g_test = false; }

bool Active() { return g_active || g_test; }

bool InjectForTest(const char* spec)
{
    if (!spec || !*spec || g_active) return false;
    g_lamps.clear();
    for (const char* p = spec; *p;) {
        Lamp l = {};
        int used = 0;
        if (sscanf_s(p, " %f %f %f %f %f %f %f%n", &l.pos[0], &l.pos[1], &l.pos[2], &l.col[0], &l.col[1], &l.col[2], &l.reach, &used) != 7) break;
        l.key = KeyOf(l.pos);
        g_lamps.push_back(l);
        p += used;
        while (*p == ' ' || *p == ';') ++p;
    }
    g_test = !g_lamps.empty();
    g_stats.lamps = static_cast<uint32_t>(g_lamps.size());
    return g_test;
}

void Configure(float power, float reach, float carPower)
{
    g_power = std::clamp(power, 0.0f, 100.0f);
    g_reach = std::clamp(reach, 0.1f, 10.0f);
    g_carPower = std::clamp(carPower, 0.0f, 100.0f);
}

void EndFrame()
{
    if (!g_active) return;
    const int done = g_fill;
    g_fill ^= 1;
    g_corCount[g_fill] = g_ptCount[g_fill] = 0;
    memset(g_seen, 0, sizeof(g_seen));

    g_lamps.clear();
    Stats st;
    const Corona* cor = g_cor[done];
    const Point* pt = g_pt[done];
    st.coronas = g_corCount[done];
    st.pointLights = g_ptCount[done];
    for (uint32_t i = 0; i < g_corCount[done]; ++i) {
        const Corona& c = cor[i];
        Lamp l = {};
        memcpy(l.pos, c.pos, sizeof(l.pos));
        const float k = c.intensity / 255.0f;
        const float peak = std::max({static_cast<float>(c.rgb[0]), static_cast<float>(c.rgb[1]), static_cast<float>(c.rgb[2]), 1.0f}) * 2.0f / 255.0f;
        const float scale = peak > 1.0f ? 1.0f / peak : 1.0f;
        for (int j = 0; j < 3; ++j) l.col[j] = c.rgb[j] * 2.0f / 255.0f * scale * k;
        l.reach = c.object ? std::max({c.range, c.pool, c.size * 3.0f}) : std::max(c.size * 4.0f, 4.0f);
        l.key = c.id;
        l.kind = c.object ? 0 : 1;
        if (c.object) ++st.objectCoronas;
        g_lamps.push_back(l);
    }
    const size_t withCorona = g_lamps.size();
    for (uint32_t i = 0; i < g_ptCount[done]; ++i) {
        const Point& p = pt[i];
        if (p.beam) {
            Lamp l = {};
            memcpy(l.pos, p.pos, sizeof(l.pos));
            memcpy(l.dir, p.dir, sizeof(l.dir));
            const float peak = std::max({p.rgb[0], p.rgb[1], p.rgb[2], 1.0f});
            for (int j = 0; j < 3; ++j) l.col[j] = std::max(p.rgb[j], 0.0f) / peak * (g_carPower / std::max(g_power, 1e-3f));
            l.reach = p.radius;
            l.cosOuter = 0.82f;
            l.cosInner = 0.97f;
            l.key = KeyOf(p.pos) ^ 0x5A5A5A5Au;
            l.kind = 2;
            g_lamps.push_back(l);
            continue;
        }
        Lamp* same = nullptr;
        for (size_t k = 0; k < withCorona && !same; ++k) {
            const float dx = g_lamps[k].pos[0] - p.pos[0], dy = g_lamps[k].pos[1] - p.pos[1], dz = g_lamps[k].pos[2] - p.pos[2];
            if (dx * dx + dy * dy + dz * dz < 1.0f) same = &g_lamps[k];
        }
        if (same) { same->reach = std::max(same->reach, p.radius); continue; }
        Lamp l = {};
        memcpy(l.pos, p.pos, sizeof(l.pos));
        const float peak = std::max({p.rgb[0], p.rgb[1], p.rgb[2], 1.0f});
        for (int j = 0; j < 3; ++j) l.col[j] = std::max(p.rgb[j], 0.0f) / peak;
        l.reach = p.radius;
        l.key = KeyOf(p.pos);
        l.kind = 2;
        g_lamps.push_back(l);
    }
    for (Lamp& l : g_lamps) {
        l.reach = std::clamp(l.reach * g_reach, 2.0f, 40.0f);
        for (float& c : l.col) c *= g_power;
    }
    st.lamps = static_cast<uint32_t>(g_lamps.size());
    g_stats = st;
}

const std::vector<Lamp>& Lamps() { return g_lamps; }
Stats LastStats() { return g_stats; }

}
