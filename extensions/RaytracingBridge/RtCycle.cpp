#include "RtCycle.h"
#include <cmath>
#include <cstdio>
#include <cwchar>

namespace rtcycle {
namespace {
Key          g_keys[kSeasons][kWeathers][kHours];
std::wstring g_path;
Now          g_now;

void Reset()
{
    for (auto& s : g_keys) for (auto& w : s) for (Key& k : w) for (float& v : k.v) v = 1.0f;
}
int Wrap(int h) { return ((h % kHours) + kHours) % kHours; }
}

const char* FieldName(int f)
{
    static const char* const names[kFieldCount] = {"Sun", "Moon", "Sky", "Clouds", "Fog", "Ambient", "Tint R", "Tint G", "Tint B"};
    return f >= 0 && f < kFieldCount ? names[f] : "";
}

void Load(const std::wstring& path)
{
    g_path = path;
    Reset();
    FILE* fp = _wfopen(path.c_str(), L"rt");
    if (!fp) return;
    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        if (line[0] == '#' || line[0] == ';') continue;
        int s, w, h;
        float v[kFieldCount];
        if (sscanf_s(line, "%d %d %d %f %f %f %f %f %f %f %f %f", &s, &w, &h, &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &v[8]) != 3 + kFieldCount) continue;
        if (s < 0 || s >= kSeasons || w < 0 || w >= kWeathers || h < 0 || h >= kHours) continue;
        for (int f = 0; f < kFieldCount; ++f) g_keys[s][w][h].v[f] = v[f];
    }
    fclose(fp);
}

bool Save()
{
    if (g_path.empty()) return false;
    FILE* fp = _wfopen(g_path.c_str(), L"wt");
    if (!fp) return false;
    fputs("# BullyDX12 RT time cycle: season(0 fall,1 winter,2 spring) weather(0..5) hour  sun moon sky clouds fog ambient tintR tintG tintB\n", fp);
    for (int s = 0; s < kSeasons; ++s)
        for (int w = 0; w < kWeathers; ++w)
            for (int h = 0; h < kHours; ++h) {
                const Key& k = g_keys[s][w][h];
                if (IsDefault(k)) continue;
                fprintf(fp, "%d %d %d", s, w, h);
                for (float v : k.v) fprintf(fp, " %.3f", v);
                fputc('\n', fp);
            }
    fclose(fp);
    return true;
}

Key& At(int season, int weather, int hour)
{
    season  = season < 0 || season >= kSeasons ? 0 : season;
    weather = weather < 0 || weather >= kWeathers ? 0 : weather;
    return g_keys[season][weather][Wrap(hour)];
}

bool IsDefault(const Key& k)
{
    for (float v : k.v) if (std::fabs(v - 1.0f) > 1e-4f) return false;
    return true;
}

Key Sample(const Now& n)
{
    const float hours = std::fmod(n.hours + 24.0f, 24.0f);
    const int   h0 = static_cast<int>(hours) % kHours, h1 = (h0 + 1) % kHours;
    const float hf = hours - std::floor(hours);
    const float b  = n.blend < 0.0f ? 0.0f : (n.blend > 1.0f ? 1.0f : n.blend);
    const Key &a0 = At(n.season, n.from, h0), &a1 = At(n.season, n.from, h1);
    const Key &c0 = At(n.season, n.to, h0),   &c1 = At(n.season, n.to, h1);
    Key out;
    for (int f = 0; f < kFieldCount; ++f)
        out.v[f] = (a0.v[f] * (1 - hf) + a1.v[f] * hf) * (1 - b) + (c0.v[f] * (1 - hf) + c1.v[f] * hf) * b;
    return out;
}

Now& Current() { return g_now; }

}
