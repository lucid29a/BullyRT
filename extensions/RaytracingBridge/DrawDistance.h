// The draw distance of the world's objects. Each object of the game's IDE files (Objects\ide.img) carries the distance up
// to which its detailed model is drawn and kept streamed in; past it the game shows its low-detail LOD. The PC values are
// short (100-180 on the school grounds), so the detailed model is requested only once the player is already near and
// arrives late. The setter the loader calls for every object is patched to multiply the distance by a factor.

#pragma once
#include <functional>
#include <string>

namespace drawdistance {

bool Apply(float factor, const std::function<void(const std::string&)>& log);

void Restore();

}
