// The game's own error messages (Gamebryo), which the retail game throws away, written to our log.

#pragma once
#include <functional>
#include <string>

namespace gameerrors {

void Install(std::function<void(const std::string&)> log);

}
