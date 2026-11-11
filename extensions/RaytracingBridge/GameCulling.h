// The game's culling, replaced. Bully tests each object's bounding sphere against the six planes of the camera
// (MdBullyCullingProcess) and hands on only what touches the screen: the rays then lose everything behind the camera
// and beside it, and the renderer had to guess from where objects were last seen. With the plane test off the game
// hands on every object it has loaded (its own day/night and hidden flags still apply), and RaytracingBridge culls
// the raster itself against the real view while the rays keep the whole world.

#pragma once
#include <functional>
#include <string>

namespace gameculling {

bool DisableFrustum(const std::function<void(const std::string&)>& log);

void Restore();

bool Active();

}
