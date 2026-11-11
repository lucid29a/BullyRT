// Every light the game itself turns on, as it turns it on.

#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace gamelights {

struct Lamp {
    float    pos[3];
    float    col[3];
    float    reach;
    float    dir[3];
    float    cosOuter = -2.0f, cosInner = -2.0f;
    uint32_t key;
    uint8_t  kind;
};

struct Stats {
    uint32_t coronas = 0, objectCoronas = 0, pointLights = 0, lamps = 0;
};

bool Install(const std::function<void(const std::string&)>& log);
void Restore();
bool Active();

void Configure(float power, float reach, float carPower);

bool SetLampHours(float onHour, float offHour, const std::function<void(const std::string&)>& log);

bool InjectForTest(const char* spec);

void EndFrame();
const std::vector<Lamp>& Lamps();
Stats LastStats();

}
