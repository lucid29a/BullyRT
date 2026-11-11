// The RT time cycle: our own keys per season x weather x hour, on top of what the game's timecyc gives.
// Every key is a set of multipliers (1 = untouched). The RT side mixes the keys of the current hour and the next,
// and of the weather the game is leaving and the one it is going to, the way the game mixes its own rows.

#pragma once
#include <string>

namespace rtcycle {

enum Field { kSun, kMoon, kSky, kClouds, kFog, kAmbient, kTintR, kTintG, kTintB, kFieldCount };
constexpr int kSeasons = 3, kWeathers = 6, kHours = 24;

struct Key { float v[kFieldCount]; };

struct Now { int season = 0, from = 0, to = 0; float blend = 0.0f, hours = 12.0f; bool fromGame = false; };

void        Load(const std::wstring& path);
bool        Save();
Key&        At(int season, int weather, int hour);
bool        IsDefault(const Key& k);
Key         Sample(const Now& n);
Now&        Current();
const char* FieldName(int f);

}
