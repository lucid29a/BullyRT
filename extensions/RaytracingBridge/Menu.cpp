#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "Menu.h"
#include "RtCycle.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "imgui.h"
#include "backends/imgui_impl_dx9.h"
#include "backends/imgui_impl_win32.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace menu {
namespace {

std::wstring g_ini, g_config;
Hooks        g_hooks;
bool         g_ready = false, g_open = false, g_keyDown = false;
HWND         g_wnd = nullptr;
WNDPROC      g_prevProc = nullptr;

using GetStateFn = HRESULT(__stdcall*)(void*, DWORD, void*);
using GetDataFn  = HRESULT(__stdcall*)(void*, DWORD, void*, DWORD*, DWORD);
GetStateFn g_getState[2] = {};
GetDataFn  g_getData[2] = {};
void**     g_vtbl[2] = {};
volatile LONG g_dx = 0, g_dy = 0, g_wheel = 0;
volatile LONG g_buttons = 0;
volatile LONG g_buffered = 0;
float      g_cursor[2] = {-1, -1};

void TakeMouseState(DWORD cb, void* data)
{
    if (!data || (cb != 16 && cb != 20)) return;
    const LONG* l = static_cast<const LONG*>(data);
    const BYTE* b = static_cast<const BYTE*>(data) + 12;
    if (!g_buffered) { InterlockedExchangeAdd(&g_dx, l[0]); InterlockedExchangeAdd(&g_dy, l[1]); InterlockedExchangeAdd(&g_wheel, l[2]); }
    InterlockedExchange(&g_buttons, ((b[0] & 0x80) ? 1 : 0) | ((b[1] & 0x80) ? 2 : 0) | ((b[2] & 0x80) ? 4 : 0));
}

template <int K> HRESULT __stdcall HookGetState(void* self, DWORD cb, void* data)
{
    const HRESULT hr = g_getState[K](self, cb, data);
    if (g_open && SUCCEEDED(hr) && data) { TakeMouseState(cb, data); memset(data, 0, cb); }
    return hr;
}

template <int K> HRESULT __stdcall HookGetData(void* self, DWORD cb, void* rgdod, DWORD* inOut, DWORD flags)
{
    const HRESULT hr = g_getData[K](self, cb, rgdod, inOut, flags);
    if (!g_open || FAILED(hr) || !inOut) return hr;
    if (rgdod && cb >= 16) {
        for (DWORD i = 0; i < *inOut; ++i) {
            const DWORD* e = reinterpret_cast<const DWORD*>(static_cast<const BYTE*>(rgdod) + i * cb);
            const DWORD ofs = e[0], v = e[1];
            if (ofs <= 8) InterlockedExchange(&g_buffered, 1);
            if (ofs == 0) InterlockedExchangeAdd(&g_dx, static_cast<LONG>(v));
            else if (ofs == 4) InterlockedExchangeAdd(&g_dy, static_cast<LONG>(v));
            else if (ofs == 8) InterlockedExchangeAdd(&g_wheel, static_cast<LONG>(v));
            else if (ofs >= 12 && ofs <= 14) {
                const LONG bit = 1 << (ofs == 12 ? 0 : ofs == 13 ? 1 : 2);
                LONG cur = g_buttons;
                InterlockedExchange(&g_buttons, (v & 0x80) ? (cur | bit) : (cur & ~bit));
            }
        }
    }
    *inOut = 0;
    return hr;
}

void PatchSlot(void** vtbl, int slot, void* fn, void** original)
{
    DWORD old = 0;
    if (!VirtualProtect(&vtbl[slot], sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) return;
    *original = vtbl[slot];
    vtbl[slot] = fn;
    VirtualProtect(&vtbl[slot], sizeof(void*), old, &old);
}

void HookDirectInput()
{
    static bool tried = false;
    if (tried) return;
    tried = true;
    HMODULE di = LoadLibraryW(L"dinput8.dll");
    if (!di) return;
    using CreateFn = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, void**, IUnknown*);
    const auto create = reinterpret_cast<CreateFn>(GetProcAddress(di, "DirectInput8Create"));
    if (!create) return;
    static const GUID kIID8A = {0xBF798030, 0x483A, 0x4DA2, {0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00}};
    static const GUID kIID8W = {0xBF798031, 0x483A, 0x4DA2, {0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00}};
    static const GUID kSysMouse = {0x6F1D2B60, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
    const GUID* iids[2] = {&kIID8A, &kIID8W};
    for (int k = 0; k < 2; ++k) {
        void* dinput = nullptr;
        if (FAILED(create(GetModuleHandleW(nullptr), 0x0800, *iids[k], &dinput, nullptr)) || !dinput) continue;
        using CreateDevFn = HRESULT(__stdcall*)(void*, REFGUID, void**, IUnknown*);
        void** dvt = *static_cast<void***>(dinput);
        void* dev = nullptr;
        if (SUCCEEDED(reinterpret_cast<CreateDevFn>(dvt[3])(dinput, kSysMouse, &dev, nullptr)) && dev) {
            void** vt = *static_cast<void***>(dev);
            g_vtbl[k] = vt;
            if (k == 0) { PatchSlot(vt, 9, reinterpret_cast<void*>(&HookGetState<0>), reinterpret_cast<void**>(&g_getState[0]));
                          PatchSlot(vt, 10, reinterpret_cast<void*>(&HookGetData<0>), reinterpret_cast<void**>(&g_getData[0])); }
            else        { PatchSlot(vt, 9, reinterpret_cast<void*>(&HookGetState<1>), reinterpret_cast<void**>(&g_getState[1]));
                          PatchSlot(vt, 10, reinterpret_cast<void*>(&HookGetData<1>), reinterpret_cast<void**>(&g_getData[1])); }
            reinterpret_cast<ULONG(__stdcall*)(void*)>(vt[2])(dev);
        }
        reinterpret_cast<ULONG(__stdcall*)(void*)>(dvt[2])(dinput);
    }
}

void UnhookDirectInput()
{
    for (int k = 0; k < 2; ++k) {
        if (!g_vtbl[k]) continue;
        void* dummy = nullptr;
        if (g_getState[k]) PatchSlot(g_vtbl[k], 9, reinterpret_cast<void*>(g_getState[k]), &dummy);
        if (g_getData[k]) PatchSlot(g_vtbl[k], 10, reinterpret_cast<void*>(g_getData[k]), &dummy);
        g_vtbl[k] = nullptr;
    }
}

void FeedMouse()
{
    ImGuiIO& io = ImGui::GetIO();
    const float w = io.DisplaySize.x, h = io.DisplaySize.y;
    if (g_cursor[0] < 0) { g_cursor[0] = w * 0.5f; g_cursor[1] = h * 0.5f; }
    const LONG dx = InterlockedExchange(&g_dx, 0), dy = InterlockedExchange(&g_dy, 0), wheel = InterlockedExchange(&g_wheel, 0);
    g_cursor[0] = std::clamp(g_cursor[0] + dx, 0.0f, w - 1.0f);
    g_cursor[1] = std::clamp(g_cursor[1] + dy, 0.0f, h - 1.0f);
    io.AddMousePosEvent(g_cursor[0], g_cursor[1]);
    const LONG b = g_buttons | ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) ? 1 : 0) | ((GetAsyncKeyState(VK_RBUTTON) & 0x8000) ? 2 : 0);
    io.AddMouseButtonEvent(0, (b & 1) != 0);
    io.AddMouseButtonEvent(1, (b & 2) != 0);
    io.AddMouseButtonEvent(2, (b & 4) != 0);
    if (wheel) io.AddMouseWheelEvent(0.0f, static_cast<float>(wheel) / 120.0f);
}

float IniF(const char* sec, const char* key, float def)
{
    char buf[64] = {};
    wchar_t ws[64], wk[64], wb[64] = {};
    MultiByteToWideChar(CP_UTF8, 0, sec, -1, ws, 64); MultiByteToWideChar(CP_UTF8, 0, key, -1, wk, 64);
    if (!GetPrivateProfileStringW(ws, wk, L"", wb, 64, g_ini.c_str()) || !wb[0]) return def;
    WideCharToMultiByte(CP_UTF8, 0, wb, -1, buf, 64, nullptr, nullptr);
    return static_cast<float>(atof(buf));
}
void IniSet(const char* sec, const char* key, float v, bool integer)
{
    wchar_t ws[64], wk[64], wv[64];
    MultiByteToWideChar(CP_UTF8, 0, sec, -1, ws, 64); MultiByteToWideChar(CP_UTF8, 0, key, -1, wk, 64);
    if (integer) swprintf_s(wv, L"%d", static_cast<int>(std::lround(v)));
    else swprintf_s(wv, L"%g", v);
    WritePrivateProfileStringW(ws, wk, wv, g_ini.c_str());
}

enum Kind { kFloat, kInt, kBool, kCombo };
struct Item { const char* sec; const char* key; const char* label; Kind kind; float lo, hi, def; const char* combo; float v; };

std::vector<Item> g_rt = {
    {"RTOptions", "RayTracing", "Ray tracing", kBool, 0, 1, 1},
    {"RTOptions", "Lighting", "Lighting", kCombo, 0, 3, 2, "Off\0Medium\0Ultra\0Psycho\0"},
    {"RTOptions", "Reflections", "Reflections", kBool, 0, 1, 1},
    {"RTOptions", "SunShadows", "Sun shadows", kBool, 0, 1, 1},
    {"RTOptions", "LocalShadows", "Lamp shadows", kBool, 0, 1, 1},
    {"RTOptions", "Volumetric", "Volumetrics", kBool, 0, 1, 1},
    {"RTOptions", "Denoiser", "Denoiser", kCombo, 0, 1, 0, "NRD\0DLSS RR\0"},
    {"RaytracingBridge", "RtScale", "Ray resolution", kCombo, 1, 3, 2, "Full\0Half\0Third\0"},
};
std::vector<Item> g_light = {
    {"Light", "FixturePower", "Fixtures", kFloat, 0, 4, 1},
    {"Light", "FixtureReach", "Fixture reach", kFloat, 5, 40, 15},
    {"Light", "GameLampPower", "Game lamps", kFloat, 0, 10, 3},
    {"Light", "GameLampReach", "Game lamp reach", kFloat, 0.5f, 4, 1.5f},
    {"Light", "CarLightPower", "Headlights", kFloat, 0, 6, 1.5f},
    {"Light", "GameLightPower", "Area lights", kFloat, 0, 4, 1},
    {"Light", "GameLightNearLamp", "Area near fixture", kFloat, 0, 1, 0.2f},
    {"Light", "LampsOnHour", "Lamps on", kInt, 12, 23, 18},
    {"Light", "LampsOffHour", "Lamps off", kInt, 0, 12, 7},
    {"Light", "WindowPower", "Window light", kFloat, 0, 4, 0},
};
std::vector<Item> g_image = {
    {"Light", "Brightness", "Brightness", kFloat, 0.2f, 2, 0.65f},
    {"Exposure", "Min", "Exposure min", kFloat, 0.1f, 4, 0.5f},
    {"Exposure", "Max", "Exposure max", kFloat, 1, 32, 8},
    {"RaytracingBridge", "GiStrength", "Bounce", kFloat, 0, 4, 1.5f},
    {"Light", "BounceColor", "Bounce color", kFloat, 0, 3, 1.5f},
    {"RaytracingBridge", "ReflectionStrength", "Reflections", kFloat, 0, 2, 1},
    {"Light", "VolumetricStrength", "Fog indoors", kFloat, 0, 10, 5},
    {"Light", "VolumetricOutdoor", "Fog outdoors", kFloat, 0, 10, 2.5f},
};
std::vector<Item> g_sky = {
    {"Light", "SunAzimuthDegrees", "Sun azimuth", kFloat, 0, 360, 35},
    {"Light", "SunTiltDegrees", "Sun tilt", kFloat, 0, 80, 35},
    {"RaytracingBridge", "SunIntensity", "Sun", kFloat, 0, 3, 1},
    {"RaytracingBridge", "SkyIntensity", "Sky", kFloat, 0, 3, 1},
    {"Light", "MoonIntensity", "Moon", kFloat, 0, 4, 1},
    {"Light", "CloudCoverage", "Clouds", kFloat, 0, 1, 0.45f},
    {"Light", "CloudBottom", "Cloud base", kFloat, 200, 3000, 900},
    {"Light", "CloudTop", "Cloud top", kFloat, 300, 5000, 1700},
};

void LoadItems(std::vector<Item>& items) { for (Item& i : items) i.v = IniF(i.sec, i.key, i.def); }

bool DrawItems(std::vector<Item>& items)
{
    bool applied = false;
    for (Item& i : items) {
        ImGui::PushID(i.key);
        bool commit = false;
        switch (i.kind) {
        case kBool: { bool b = i.v != 0; if (ImGui::Checkbox(i.label, &b)) { i.v = b ? 1.f : 0.f; commit = true; } break; }
        case kCombo: {
            int c = static_cast<int>(i.v - i.lo);
            if (ImGui::Combo(i.label, &c, i.combo)) { i.v = i.lo + c; commit = true; }
            break;
        }
        case kInt: {
            int n = static_cast<int>(i.v);
            ImGui::SliderInt(i.label, &n, static_cast<int>(i.lo), static_cast<int>(i.hi));
            i.v = static_cast<float>(n);
            commit = ImGui::IsItemDeactivatedAfterEdit();
            break;
        }
        default:
            ImGui::SliderFloat(i.label, &i.v, i.lo, i.hi, "%.2f");
            commit = ImGui::IsItemDeactivatedAfterEdit();
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) { i.v = i.def; commit = true; }
        }
        if (commit) { IniSet(i.sec, i.key, i.v, i.kind != kFloat); applied = true; }
        ImGui::PopID();
    }
    return applied;
}

struct TcLine { std::string text; std::vector<std::string> tok; bool data = false; };
struct TcFile { std::wstring path; std::vector<TcLine> lines; std::vector<int> rows[6]; bool loaded = false, dirty = false; };
TcFile g_tc[3];
const char* const kSeasons = "Fall\0Winter\0Summer\0";
const wchar_t* const kTcFiles[3] = {L"timecycF.dat", L"timecycW.dat", L"timecycS.dat"};
const char* const kWeathers = "Sunny\0Cloudy\0Rainy / snow\0Foggy\0Extra sunny\0Hurricane\0";
const char* const kWeatherName[6] = {"Sunny", "Cloudy", "Rainy / snow", "Foggy", "Extra sunny", "Hurricane"};
const char* const kSeasonName[3] = {"Fall", "Winter", "Summer"};
const int kKeys[] = {0, 4, 6, 7, 9, 12, 16, 18, 19, 20, 21};
const char* const kKeyName[] = {"12 AM", "4 AM", "6 AM", "7 AM", "9 AM", "12 PM", "4 PM", "6 PM", "7 PM", "8 PM", "9 PM"};
constexpr int kKeyCount = static_cast<int>(sizeof(kKeys) / sizeof(kKeys[0]));
int g_season = 0, g_weather = 0, g_key = 5;

struct Field { const char* name; int at, n; };
const Field kFields[] = {
    {"Ambient props", 0, 3}, {"Ambient world", 3, 3}, {"Ambient peds", 6, 3}, {"Sun", 9, 3}, {"Back light", 12, 3},
    {"Back light int", 15, 1}, {"Sky top", 16, 3}, {"Sky bottom", 19, 3}, {"Sun core", 22, 3}, {"Sun corona", 25, 3},
    {"Sun size", 28, 1}, {"Sprite size", 29, 1}, {"Sprite bright", 30, 1}, {"Shadows", 31, 1}, {"Light shadow", 32, 1},
    {"Pole shadow", 33, 1}, {"Far clip", 34, 1}, {"Fog start", 35, 1}, {"Night factor", 36, 1},
};

struct ColMap { int col; char type; int off; float scale; };
const ColMap kBox[] = {
    {0, 'b', 0x00, 1}, {1, 'b', 0x01, 1}, {2, 'b', 0x02, 1}, {3, 'b', 0x03, 1}, {4, 'b', 0x04, 1}, {5, 'b', 0x05, 1},
    {6, 'b', 0x06, 1}, {7, 'b', 0x07, 1}, {8, 'b', 0x08, 1}, {9, 'b', 0x09, 1}, {10, 'b', 0x0A, 1}, {11, 'b', 0x0B, 1},
    {12, 'b', 0x0C, 1}, {13, 'b', 0x0D, 1}, {14, 'b', 0x0E, 1}, {15, 'f', 0x10, 1},
    {16, 'b', 0x2C, 1}, {17, 'b', 0x2D, 1}, {18, 'b', 0x2E, 1}, {19, 'b', 0x2F, 1}, {20, 'b', 0x30, 1}, {21, 'b', 0x31, 1},
    {22, 'b', 0x35, 1}, {23, 'b', 0x36, 1}, {24, 'b', 0x37, 1}, {25, 'b', 0x38, 1}, {26, 'b', 0x39, 1}, {27, 'b', 0x3A, 1},
    {28, 'c', 0x3B, 10}, {29, 'c', 0x3C, 10}, {30, 'c', 0x3D, 10}, {31, 'b', 0x3E, 1}, {32, 'f', 0x40, 1}, {33, 'f', 0x44, 1},
    {34, 's', 0x48, 1}, {35, 's', 0x4A, 1}, {36, 'f', 0x14, 1}, {37, 'f', 0x18, 1}, {38, 'f', 0x1C, 1}, {39, 'f', 0x20, 1},
    {40, 'f', 0x24, 1}, {41, 'f', 0x28, 1}, {42, 'b', 0x4C, 1}, {43, 'b', 0x4D, 1}, {55, 'f', 0x50, 1},
    {56, 'b', 0x54, 1}, {57, 'b', 0x55, 1}, {58, 'b', 0x56, 1}, {59, 'b', 0x57, 1}, {60, 'b', 0x58, 1}, {61, 'b', 0x59, 1},
    {62, 'b', 0x5A, 1}, {63, 'b', 0x5B, 1}, {64, 'b', 0x5C, 1}, {65, 'b', 0x5D, 1}, {66, 'b', 0x5E, 1},
    {67, 'f', 0x60, 1}, {68, 'f', 0x64, 1}, {69, 'f', 0x68, 1}, {70, 'f', 0x6C, 1}, {71, 'f', 0x70, 1}, {72, 'f', 0x74, 1},
    {73, 's', 0x78, 1},
};

bool GameIsBully() { return reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)) == 0x400000; }

void LoadTc(int s)
{
    TcFile& f = g_tc[s];
    f = TcFile{};
    f.path = g_config + kTcFiles[s];
    FILE* fp = _wfopen(f.path.c_str(), L"rb");
    if (!fp) return;
    std::string all;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) all.append(buf, n);
    fclose(fp);
    int weather = -1;
    size_t a = 0;
    while (a < all.size()) {
        size_t b = all.find('\n', a);
        if (b == std::string::npos) b = all.size();
        TcLine l;
        l.text = all.substr(a, b - a);
        a = b + 1;
        const char* p = l.text.c_str();
        while (*p == ' ' || *p == '\t') ++p;
        if (!strncmp(p, "//////", 6)) ++weather;
        else if (weather >= 0 && weather < 6 && (isdigit(static_cast<unsigned char>(*p)) || *p == '-' || *p == '.')) {
            for (const char* q = p; *q && *q != '\r';) {
                while (*q == ' ' || *q == '\t') ++q;
                const char* e = q;
                while (*e && *e != ' ' && *e != '\t' && *e != '\r') ++e;
                if (e > q) l.tok.emplace_back(q, e);
                q = e;
            }
            l.data = true;
            if (f.rows[weather].size() < 24) f.rows[weather].push_back(static_cast<int>(f.lines.size()));
        }
        f.lines.push_back(std::move(l));
    }
    f.loaded = true;
}

bool SaveTc(int s)
{
    TcFile& f = g_tc[s];
    const std::wstring bak = f.path + L".original";
    if (GetFileAttributesW(bak.c_str()) == INVALID_FILE_ATTRIBUTES) CopyFileW(f.path.c_str(), bak.c_str(), TRUE);
    FILE* fp = _wfopen(f.path.c_str(), L"wb");
    if (!fp) return false;
    for (size_t i = 0; i < f.lines.size(); ++i) {
        const TcLine& l = f.lines[i];
        if (l.data) {
            std::string out;
            for (size_t k = 0; k < l.tok.size(); ++k) out += (k ? "\t" : "") + l.tok[k];
            fputs(out.c_str(), fp);
        } else {
            fputs(l.text.c_str(), fp);
        }
        if (i + 1 < f.lines.size()) fputc('\n', fp);
    }
    fclose(fp);
    f.dirty = false;
    return true;
}

float TokF(const std::string& t) { return static_cast<float>(atof(t.c_str())); }
std::string FmtLike(const std::string& was, float v)
{
    char b[32];
    if (was.find('.') == std::string::npos) snprintf(b, sizeof(b), "%d", static_cast<int>(std::lround(v)));
    else snprintf(b, sizeof(b), "%.3f", v);
    return b;
}

void WriteBox(int season, int weather, int hour, const std::vector<std::string>& tok)
{
    if (!GameIsBully() || season < 0 || season > 2 || weather < 0 || weather > 5 || hour < 0 || hour > 23) return;
    uint8_t* box = reinterpret_cast<uint8_t*>(0xBA6C70) + (hour * 21 + weather + 6 * season) * 0x7C;
    for (const ColMap& m : kBox) {
        if (m.col >= static_cast<int>(tok.size())) continue;
        const float v = TokF(tok[m.col]) * m.scale;
        const long r = std::lround(v);
        switch (m.type) {
        case 'b': box[m.off] = static_cast<uint8_t>(std::clamp(r, 0L, 255L)); break;
        case 'c': box[m.off] = static_cast<uint8_t>(static_cast<int8_t>(std::clamp(r, -128L, 127L))); break;
        case 's': { const int16_t x = static_cast<int16_t>(std::clamp(r, -32768L, 32767L)); memcpy(box + m.off, &x, 2); break; }
        default: memcpy(box + m.off, &v, 4);
        }
    }
}

void FollowKey(int season, int weather, int keyIndex)
{
    TcFile& f = g_tc[season];
    const bool rows = f.loaded && f.rows[weather].size() == 24;
    for (int side = 0; side < 2; ++side) {
        const int ia = side == 0 ? (keyIndex + kKeyCount - 1) % kKeyCount : keyIndex;
        const int ib = side == 0 ? keyIndex : (keyIndex + 1) % kKeyCount;
        const int ha = kKeys[ia];
        int hb = kKeys[ib];
        if (hb <= ha) hb += 24;
        for (int h = ha + 1; h < hb; ++h) {
            const float t = static_cast<float>(h - ha) / static_cast<float>(hb - ha);
            const int hh = h % 24;
            const rtcycle::Key ka = rtcycle::At(season, weather, ha);
            const rtcycle::Key kb = rtcycle::At(season, weather, hb % 24);
            rtcycle::Key& kh = rtcycle::At(season, weather, hh);
            for (int c = 0; c < rtcycle::kFieldCount; ++c) kh.v[c] = ka.v[c] + (kb.v[c] - ka.v[c]) * t;
            if (rows) {
                const std::vector<std::string> ta = f.lines[f.rows[weather][ha]].tok;
                const std::vector<std::string> tb = f.lines[f.rows[weather][hb % 24]].tok;
                TcLine& lh = f.lines[f.rows[weather][hh]];
                for (size_t c = 0; c < lh.tok.size() && c < ta.size() && c < tb.size(); ++c)
                    lh.tok[c] = FmtLike(lh.tok[c], TokF(ta[c]) + (TokF(tb[c]) - TokF(ta[c])) * t);
                WriteBox(season, weather, hh, lh.tok);
            }
        }
    }
    if (rows) WriteBox(season, weather, kKeys[keyIndex], f.lines[f.rows[weather][kKeys[keyIndex]]].tok);
}

bool g_rcDirty = false;

void DrawTimeCycle()
{
    const rtcycle::Now& n = rtcycle::Current();
    ImGui::Text("Now: %s, %s", kSeasonName[std::clamp(n.season, 0, 2)], kWeatherName[std::clamp(n.from, 0, 5)]);
    if (n.from != n.to) { ImGui::SameLine(); ImGui::Text("-> %s %.0f%%", kWeatherName[std::clamp(n.to, 0, 5)], n.blend * 100.0f); }
    ImGui::SameLine();
    ImGui::Text("%02d:%02d", static_cast<int>(n.hours) % 24, static_cast<int>((n.hours - std::floor(n.hours)) * 60.0f));
    ImGui::SameLine();
    if (ImGui::SmallButton("Go")) {
        g_season = std::clamp(n.season, 0, 2);
        g_weather = std::clamp(n.from, 0, 5);
        g_key = 0;
        for (int i = 0; i < kKeyCount; ++i) if (kKeys[i] <= static_cast<int>(n.hours) % 24) g_key = i;
    }
    ImGui::Combo("Season", &g_season, kSeasons);
    ImGui::Combo("Weather", &g_weather, kWeathers);
    for (int i = 0; i < kKeyCount; ++i) {
        if (i % 6) ImGui::SameLine();
        const bool sel = i == g_key;
        if (sel) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (ImGui::Button(kKeyName[i], ImVec2(52, 0))) g_key = i;
        if (sel) ImGui::PopStyleColor();
    }
    const int hour = kKeys[g_key];
    if (!g_tc[g_season].loaded) LoadTc(g_season);
    TcFile& f = g_tc[g_season];
    bool changed = false;

    if (ImGui::CollapsingHeader("Game", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (!f.loaded || hour >= static_cast<int>(f.rows[g_weather].size())) {
            ImGui::TextDisabled("timecyc not found");
        } else {
            TcLine& l = f.lines[f.rows[g_weather][hour]];
            for (const Field& fd : kFields) {
                if (fd.at + fd.n > static_cast<int>(l.tok.size())) continue;
                ImGui::PushID(fd.at);
                if (fd.n == 3) {
                    float c[3] = {TokF(l.tok[fd.at]) / 255.f, TokF(l.tok[fd.at + 1]) / 255.f, TokF(l.tok[fd.at + 2]) / 255.f};
                    if (ImGui::ColorEdit3(fd.name, c, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_Uint8)) {
                        for (int k = 0; k < 3; ++k) l.tok[fd.at + k] = FmtLike(l.tok[fd.at + k], std::clamp(c[k], 0.f, 1.f) * 255.f);
                        changed = true;
                    }
                } else {
                    float v = TokF(l.tok[fd.at]);
                    if (ImGui::DragFloat(fd.name, &v, std::fabs(v) > 50.f ? 1.f : 0.01f)) { l.tok[fd.at] = FmtLike(l.tok[fd.at], v); changed = true; }
                }
                ImGui::PopID();
            }
            if (ImGui::TreeNode("Other columns")) {
                for (int k = 37; k < static_cast<int>(l.tok.size()); ++k) {
                    ImGui::PushID(k);
                    float v = TokF(l.tok[k]);
                    char name[16];
                    snprintf(name, sizeof(name), "col %d", k);
                    if (ImGui::DragFloat(name, &v, std::fabs(v) > 50.f ? 1.f : 0.01f)) { l.tok[k] = FmtLike(l.tok[k], v); changed = true; }
                    ImGui::PopID();
                }
                ImGui::TreePop();
            }
        }
    }
    if (ImGui::CollapsingHeader("Ray tracing", ImGuiTreeNodeFlags_DefaultOpen)) {
        rtcycle::Key& k = rtcycle::At(g_season, g_weather, hour);
        for (int c = 0; c < rtcycle::kTintR; ++c) {
            ImGui::PushID(100 + c);
            if (ImGui::SliderFloat(rtcycle::FieldName(c), &k.v[c], 0.0f, c == rtcycle::kFog ? 5.0f : 3.0f, "%.2f")) changed = true;
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) { k.v[c] = 1.0f; changed = true; }
            ImGui::PopID();
        }
        float tint[3] = {k.v[rtcycle::kTintR] * 0.5f, k.v[rtcycle::kTintG] * 0.5f, k.v[rtcycle::kTintB] * 0.5f};
        if (ImGui::ColorEdit3("Sun tint", tint, ImGuiColorEditFlags_NoInputs)) {
            for (int c = 0; c < 3; ++c) k.v[rtcycle::kTintR + c] = tint[c] * 2.0f;
            changed = true;
        }
    }
    if (changed) {
        FollowKey(g_season, g_weather, g_key);
        f.dirty = true;
        g_rcDirty = true;
    }
    ImGui::Separator();
    if (ImGui::Button("Copy to all keys")) {
        if (f.loaded && hour < static_cast<int>(f.rows[g_weather].size())) {
            const std::vector<std::string> tok = f.lines[f.rows[g_weather][hour]].tok;
            for (int r : f.rows[g_weather]) f.lines[r].tok = tok;
            for (int h = 0; h < 24; ++h) WriteBox(g_season, g_weather, h, tok);
            f.dirty = true;
        }
        const rtcycle::Key src = rtcycle::At(g_season, g_weather, hour);
        for (int h = 0; h < 24; ++h) rtcycle::At(g_season, g_weather, h) = src;
        g_rcDirty = true;
    }
    ImGui::SameLine();
    const bool anyDirty = g_tc[0].dirty || g_tc[1].dirty || g_tc[2].dirty || g_rcDirty;
    ImGui::BeginDisabled(!anyDirty);
    if (ImGui::Button("Save")) {
        bool ok = true;
        for (int s = 0; s < 3; ++s) if (g_tc[s].dirty) ok = SaveTc(s) && ok;
        if (g_rcDirty && rtcycle::Save()) g_rcDirty = false;
        if (ok && g_hooks.timeCycleSaved) g_hooks.timeCycleSaved();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!f.dirty);
    if (ImGui::Button("Revert")) {
        LoadTc(g_season);
        if (g_tc[g_season].loaded)
            for (int w = 0; w < 6; ++w)
                for (int h = 0; h < static_cast<int>(g_tc[g_season].rows[w].size()); ++h)
                    WriteBox(g_season, w, h, g_tc[g_season].lines[g_tc[g_season].rows[w][h]].tok);
    }
    ImGui::EndDisabled();
    ImGui::TextDisabled("Applied live. Save keeps it.");
}

std::vector<Item> g_time = {
    {"Light", "FollowGameClock", "RT follows game clock", kBool, 0, 1, 1},
    {"Light", "TimeOfDay", "RT time", kFloat, 0, 24, 13},
    {"Light", "ClockPaused", "RT clock paused", kBool, 0, 1, 1},
    {"Light", "DayLengthMinutes", "RT day length (min)", kFloat, 1, 120, 24},
};
int  g_seasonSaved[16];
bool g_seasonKept[16] = {};
int  g_forceWeather = 0;

void SetSeason(int season)
{
    if (!GameIsBully()) return;
    const uint8_t chapter = *reinterpret_cast<volatile uint8_t*>(0xA147E0);
    if (chapter >= 12) return;
    int32_t* table = reinterpret_cast<int32_t*>(0xA147EC);
    if (!g_seasonKept[chapter]) { g_seasonSaved[chapter] = table[chapter]; g_seasonKept[chapter] = true; }
    if (table[chapter] == season) return;
    table[chapter] = season;
    using SetWinterPeds = void(__thiscall*)(void*, int);
    reinterpret_cast<SetWinterPeds>(0x454540)(reinterpret_cast<void*>(0xA147E0), season == 2 ? 1 : 0);
    reinterpret_cast<void(__cdecl*)()>(0x5348D0)();
}

bool DrawTime()
{
    bool applied = false;
    if (GameIsBully()) {
        volatile uint8_t& hours = *reinterpret_cast<volatile uint8_t*>(0xBCEB4B);
        volatile uint8_t& minutes = *reinterpret_cast<volatile uint8_t*>(0xBCEB4A);
        volatile int32_t& days = *reinterpret_cast<volatile int32_t*>(0xBCEB4C);
        volatile uint8_t& bigJump = *reinterpret_cast<volatile uint8_t*>(0xBCEB3C);
        volatile uint8_t& stopped = *reinterpret_cast<volatile uint8_t*>(0xBCEB3F);
        ImGui::Text("Day %d   %02d:%02d", static_cast<int>(days), static_cast<int>(hours), static_cast<int>(minutes));
        int h = hours, m = minutes;
        if (ImGui::SliderInt("Hour", &h, 0, 23)) { hours = static_cast<uint8_t>(h); bigJump = 1; }
        if (ImGui::SliderInt("Minute", &m, 0, 59)) { minutes = static_cast<uint8_t>(m); bigJump = 1; }
        if (ImGui::Button("-1 day")) days = days - 1;
        ImGui::SameLine();
        if (ImGui::Button("+1 day")) days = days + 1;
        ImGui::SameLine();
        bool freeze = stopped != 0;
        if (ImGui::Checkbox("Freeze clock", &freeze)) stopped = freeze ? 1 : 0;

        ImGui::Separator();
        const uint8_t chapter = *reinterpret_cast<volatile uint8_t*>(0xA147E0);
        const int32_t* table = reinterpret_cast<const int32_t*>(0xA147EC);
        const int season = chapter < 12 ? table[chapter] : 1;
        int pick = std::clamp(season - 1, 0, 2);
        ImGui::Text("Chapter %d", static_cast<int>(chapter));
        if (ImGui::Combo("Season##time", &pick, kSeasons)) SetSeason(pick + 1);
        if (chapter < 12 && g_seasonKept[chapter] && g_seasonSaved[chapter] != season) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Game's")) SetSeason(g_seasonSaved[chapter]);
        }
        ImGui::Combo("Weather##time", &g_forceWeather, kWeathers);
        ImGui::SameLine();
        if (ImGui::SmallButton("Set")) {
            *reinterpret_cast<volatile int16_t*>(0xCE7574) = static_cast<int16_t>(g_forceWeather);
            *reinterpret_cast<volatile int16_t*>(0xCE7570) = static_cast<int16_t>(g_forceWeather);
            *reinterpret_cast<volatile float*>(0xCE7568) = 0.0f;
        }
        ImGui::Separator();
    }
    applied |= DrawItems(g_time);
    return applied;
}

void Style()
{
    ImGuiStyle& s = ImGui::GetStyle();
    ImGui::StyleColorsDark();
    s.WindowRounding = 4; s.FrameRounding = 3; s.GrabRounding = 3; s.TabRounding = 3; s.ScrollbarRounding = 3;
    s.WindowPadding = ImVec2(8, 8); s.FramePadding = ImVec2(6, 3); s.ItemSpacing = ImVec2(6, 4); s.ItemInnerSpacing = ImVec2(4, 4);
    s.WindowBorderSize = 0; s.FrameBorderSize = 0; s.GrabMinSize = 8;
    ImVec4* c = s.Colors;
    const ImVec4 accent(0.95f, 0.62f, 0.20f, 1.0f);
    c[ImGuiCol_WindowBg] = ImVec4(0.07f, 0.07f, 0.08f, 0.94f);
    c[ImGuiCol_FrameBg] = ImVec4(0.15f, 0.15f, 0.17f, 1.0f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.22f, 0.22f, 0.25f, 1.0f);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.26f, 0.26f, 0.30f, 1.0f);
    c[ImGuiCol_TitleBg] = c[ImGuiCol_TitleBgActive] = ImVec4(0.07f, 0.07f, 0.08f, 1.0f);
    c[ImGuiCol_Tab] = ImVec4(0.13f, 0.13f, 0.15f, 1.0f);
    c[ImGuiCol_TabHovered] = c[ImGuiCol_TabSelected] = ImVec4(0.30f, 0.20f, 0.08f, 1.0f);
    c[ImGuiCol_SliderGrab] = c[ImGuiCol_SliderGrabActive] = c[ImGuiCol_CheckMark] = accent;
    c[ImGuiCol_Button] = ImVec4(0.18f, 0.18f, 0.20f, 1.0f);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.30f, 0.20f, 0.08f, 1.0f);
    c[ImGuiCol_ButtonActive] = accent;
    c[ImGuiCol_Header] = ImVec4(0.18f, 0.18f, 0.20f, 1.0f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.30f, 0.20f, 0.08f, 1.0f);
    c[ImGuiCol_Separator] = ImVec4(0.22f, 0.22f, 0.25f, 1.0f);
}

LRESULT CALLBACK MenuProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (g_open) {
        const bool mouse = msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST;
        if (!mouse) ImGui_ImplWin32_WndProcHandler(wnd, msg, wp, lp);
        const ImGuiIO& io = ImGui::GetIO();
        const bool keys = msg == WM_KEYDOWN || msg == WM_KEYUP || msg == WM_CHAR || msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP;
        if ((mouse && io.WantCaptureMouse) || (keys && io.WantCaptureKeyboard) || msg == WM_SETCURSOR) return msg == WM_SETCURSOR ? TRUE : 0;
    }
    return CallWindowProcW(g_prevProc, wnd, msg, wp, lp);
}

}

void Init(const std::wstring& gameDir, const Hooks& hooks)
{
    g_ini = gameDir + L"BullyDX12.ini";
    g_config = gameDir + L"Config\\";
    g_hooks = hooks;
}

bool Open() { return g_open; }

void Frame(IDirect3DDevice9* device)
{
    const bool key = (GetAsyncKeyState(VK_INSERT) & 0x8000) != 0;
    static bool s_testOpen = getenv("BULLYRT_TEST_MENU") != nullptr;
    const bool toggled = (key && !g_keyDown) || s_testOpen;
    s_testOpen = false;
    g_keyDown = key;
    if (!device) return;
    if (!g_ready) {
        if (!toggled) return;
        D3DDEVICE_CREATION_PARAMETERS cp = {};
        device->GetCreationParameters(&cp);
        g_wnd = cp.hFocusWindow ? cp.hFocusWindow : GetForegroundWindow();
        if (!g_wnd) return;
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        wchar_t fonts[MAX_PATH] = {};
        GetWindowsDirectoryW(fonts, MAX_PATH);
        char path[MAX_PATH * 2] = {};
        WideCharToMultiByte(CP_UTF8, 0, (std::wstring(fonts) + L"\\Fonts\\segoeui.ttf").c_str(), -1, path, sizeof(path), nullptr, nullptr);
        if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES || !io.Fonts->AddFontFromFileTTF(path, 15.0f, nullptr, io.Fonts->GetGlyphRangesDefault()))
            io.Fonts->AddFontDefault();
        Style();
        ImGui_ImplWin32_Init(g_wnd);
        ImGui_ImplDX9_Init(device);
        g_prevProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(g_wnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&MenuProc)));
        HookDirectInput();
        g_ready = true;
    }
    if (toggled) {
        g_open = !g_open;
        if (g_open) { LoadItems(g_rt); LoadItems(g_light); LoadItems(g_image); LoadItems(g_sky); LoadItems(g_time); g_cursor[0] = g_cursor[1] = -1; InterlockedExchange(&g_dx, 0); InterlockedExchange(&g_dy, 0); }
    }
    if (!g_open) return;

    ImGui_ImplDX9_NewFrame();
    ImGui_ImplWin32_NewFrame();
    FeedMouse();
    ImGui::NewFrame();
    ImGui::GetIO().MouseDrawCursor = true;
    ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(340, 0), ImGuiCond_FirstUseEver);
    bool applied = false;
    if (ImGui::Begin("BullyDX12", &g_open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::PushItemWidth(170);
        if (ImGui::BeginTabBar("tabs")) {
            if (ImGui::BeginTabItem("RT")) { applied |= DrawItems(g_rt); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Light")) { applied |= DrawItems(g_light); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Image")) { applied |= DrawItems(g_image); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Sky")) { applied |= DrawItems(g_sky); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Time")) { applied |= DrawTime(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Time cycle")) {
                if (ImGui::BeginChild("tc", ImVec2(0, 520))) DrawTimeCycle();
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Info")) {
                ImGui::Text("%.0f fps  %.1f ms", ImGui::GetIO().Framerate, 1000.0f / (std::max)(ImGui::GetIO().Framerate, 1.0f));
                if (g_hooks.status) ImGui::TextUnformatted(g_hooks.status().c_str());
                ImGui::Separator();
                static bool armed = false;
                if (!armed) {
                    if (ImGui::Button("Reset all settings")) armed = true;
                } else {
                    if (ImGui::Button("Confirm reset")) {
                        for (std::vector<Item>* list : {&g_rt, &g_light, &g_image, &g_sky, &g_time})
                            for (Item& i : *list) { i.v = i.def; IniSet(i.sec, i.key, i.v, i.kind != kFloat); }
                        applied = true;
                        armed = false;
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Cancel")) armed = false;
                }
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        ImGui::PopItemWidth();
    }
    ImGui::End();
    ImGui::EndFrame();
    if (applied && g_hooks.applied) g_hooks.applied();

    if (SUCCEEDED(device->BeginScene())) {
        ImGui::Render();
        ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
        device->EndScene();
    } else {
        ImGui::Render();
    }
}

void DeviceLost()
{
    if (g_ready) ImGui_ImplDX9_InvalidateDeviceObjects();
}

void Shutdown()
{
    if (!g_ready) return;
    UnhookDirectInput();
    if (g_wnd && g_prevProc) SetWindowLongPtrW(g_wnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_prevProc));
    ImGui_ImplDX9_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    g_ready = false;
}

}
