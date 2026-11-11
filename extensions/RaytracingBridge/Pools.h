// The game's pools (CPool, as in GTA: objects, flags byte per slot, capacity, element size, next free), rebuilt: one table
// holds every pool that a longer draw distance fills, with every place in Bully.exe that fixes its size (the pool's own
// capacity, the u16 link array some of them have, and the loop that fills that array). All of them grow together, before
// the game makes its pools, and their use is logged so a pool near its end shows long before it runs out.

#pragma once
#include <functional>
#include <string>

namespace pools {

void Apply(int scale, const std::function<void(const std::string&)>& log);

void Report(const std::function<void(const std::string&)>& log);

}
