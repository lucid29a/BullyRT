// The in-game menu (Dear ImGui on the game's own D3D9 device, drawn at Present over the finished image). Insert opens it.
// Every control is a key of BullyDX12.ini: written when changed, then the game side and the bridge read it again.

#pragma once
#include <d3d9.h>
#include <functional>
#include <string>

namespace menu {

struct Hooks {
    std::function<void()>        applied;
    std::function<void()>        timeCycleSaved;
    std::function<std::string()> status;
};

void Init(const std::wstring& gameDir, const Hooks& hooks);
void Frame(IDirect3DDevice9* device);
void DeviceLost();
void Shutdown();
bool Open();

}
