#pragma once
#include "Common.h"

namespace bdx12 {

class Display {
public:
    static Display& Get();

    void LoadConfig();
    bool Borderless() const { return m_borderless; }

    void InstallModeChangeHooks();

    void MakeRealParameters(const D3DPRESENT_PARAMETERS& game, HWND focusWindow, D3DPRESENT_PARAMETERS* real);
    bool OwnsWindow(HWND wnd) const { return m_borderless && wnd && wnd == m_window; }
    UINT GameWidth() const { return m_gameWidth; }
    UINT GameHeight() const { return m_gameHeight; }
    void ApplyWindow();
    void Maintain();

private:
    HWND TargetWindow() const;
    RECT MonitorRect(HWND wnd) const;

    bool     m_borderless = true;
    bool     m_desktopResolution = false;
    bool     m_vsync = false;
    HWND     m_window = nullptr;
    UINT     m_gameWidth = 0, m_gameHeight = 0;
    uint64_t m_lastCheck = 0;
};

}
