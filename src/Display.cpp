#include "Display.h"
#include "Telemetry.h"
#include "Config.h"

namespace bdx12 {

namespace {
using PFN_ChangeDisplaySettingsA  = LONG(WINAPI*)(DEVMODEA*, DWORD);
using PFN_DirectDrawCreateEx      = HRESULT(WINAPI*)(GUID*, void**, REFIID, IUnknown*);
using PFN_DDSetCooperativeLevel   = HRESULT(STDMETHODCALLTYPE*)(void*, HWND, DWORD);
using PFN_DDSetDisplayMode        = HRESULT(STDMETHODCALLTYPE*)(void*, DWORD, DWORD, DWORD, DWORD, DWORD);

PFN_ChangeDisplaySettingsA g_realChangeDisplaySettingsA = nullptr;
PFN_DirectDrawCreateEx     g_realDirectDrawCreateEx     = nullptr;
PFN_DDSetCooperativeLevel  g_realSetCooperativeLevel    = nullptr;
PFN_DDSetDisplayMode       g_realSetDisplayMode         = nullptr;

constexpr DWORD kDDSCL_NORMAL = 0x00000008;
constexpr int   kVtSetCooperativeLevel = 20;
constexpr int   kVtSetDisplayMode      = 21;

LONG WINAPI Hook_ChangeDisplaySettingsA(DEVMODEA* mode, DWORD flags)
{
    if (mode) BDX_INFO("Borderless: blocked ChangeDisplaySettingsA(%lux%lu)", mode->dmPelsWidth, mode->dmPelsHeight);
    (void)flags;
    return DISP_CHANGE_SUCCESSFUL;
}

HRESULT STDMETHODCALLTYPE Hook_DDSetCooperativeLevel(void* self, HWND wnd, DWORD flags)
{
    if (flags != kDDSCL_NORMAL) BDX_TRACE("Borderless: DirectDraw cooperative level 0x%lx -> DDSCL_NORMAL", flags);
    return g_realSetCooperativeLevel(self, wnd, kDDSCL_NORMAL);
}

HRESULT STDMETHODCALLTYPE Hook_DDSetDisplayMode(void* self, DWORD w, DWORD h, DWORD bpp, DWORD refresh, DWORD flags)
{
    BDX_INFO("Borderless: blocked DirectDraw SetDisplayMode(%lux%lux%lu)", w, h, bpp);
    (void)self; (void)refresh; (void)flags;
    return S_OK;
}

void PatchPointer(void** slot, void* value)
{
    DWORD old = 0;
    if (VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
        *slot = value;
        VirtualProtect(slot, sizeof(void*), old, &old);
    }
}

using PFN_GetRect        = BOOL(WINAPI*)(HWND, LPRECT);
using PFN_MoveWindow     = BOOL(WINAPI*)(HWND, int, int, int, int, BOOL);
using PFN_SetWindowPos   = BOOL(WINAPI*)(HWND, HWND, int, int, int, int, UINT);
using PFN_ScreenToClient = BOOL(WINAPI*)(HWND, LPPOINT);
PFN_GetRect        g_realGetClientRect  = nullptr;
PFN_GetRect        g_realGetWindowRect  = nullptr;
PFN_MoveWindow     g_realMoveWindow     = nullptr;
PFN_SetWindowPos   g_realSetWindowPos   = nullptr;
PFN_ScreenToClient g_realScreenToClient = nullptr;

BOOL GameRect(HWND wnd, LPRECT r)
{
    const Display& d = Display::Get();
    if (!r || !d.OwnsWindow(wnd) || !d.GameWidth()) return FALSE;
    r->left = r->top = 0;
    r->right  = static_cast<LONG>(d.GameWidth());
    r->bottom = static_cast<LONG>(d.GameHeight());
    return TRUE;
}
BOOL WINAPI Hook_GetClientRect(HWND wnd, LPRECT r) { return GameRect(wnd, r) ? TRUE : g_realGetClientRect(wnd, r); }
BOOL WINAPI Hook_GetWindowRect(HWND wnd, LPRECT r) { return GameRect(wnd, r) ? TRUE : g_realGetWindowRect(wnd, r); }
BOOL WINAPI Hook_MoveWindow(HWND wnd, int x, int y, int w, int h, BOOL repaint)
{
    if (Display::Get().OwnsWindow(wnd)) return TRUE;
    return g_realMoveWindow(wnd, x, y, w, h, repaint);
}
BOOL WINAPI Hook_SetWindowPos(HWND wnd, HWND after, int x, int y, int w, int h, UINT flags)
{
    if (Display::Get().OwnsWindow(wnd)) flags |= SWP_NOMOVE | SWP_NOSIZE;
    return g_realSetWindowPos(wnd, after, x, y, w, h, flags);
}
BOOL WINAPI Hook_ScreenToClient(HWND wnd, LPPOINT pt)
{
    const BOOL ok = g_realScreenToClient(wnd, pt);
    const Display& d = Display::Get();
    if (ok && pt && d.OwnsWindow(wnd) && d.GameWidth()) {
        RECT c = {};
        ::GetClientRect(wnd, &c);
        if (c.right > 0 && c.bottom > 0) {
            pt->x = MulDiv(pt->x, static_cast<int>(d.GameWidth()), c.right);
            pt->y = MulDiv(pt->y, static_cast<int>(d.GameHeight()), c.bottom);
        }
    }
    return ok;
}

HRESULT WINAPI Hook_DirectDrawCreateEx(GUID* guid, void** out, REFIID iid, IUnknown* outer)
{
    const HRESULT hr = g_realDirectDrawCreateEx(guid, out, iid, outer);
    if (SUCCEEDED(hr) && out && *out) {
        void** vt = *static_cast<void***>(*out);
        if (vt[kVtSetDisplayMode] != reinterpret_cast<void*>(&Hook_DDSetDisplayMode)) {
            g_realSetCooperativeLevel = reinterpret_cast<PFN_DDSetCooperativeLevel>(vt[kVtSetCooperativeLevel]);
            g_realSetDisplayMode      = reinterpret_cast<PFN_DDSetDisplayMode>(vt[kVtSetDisplayMode]);
            PatchPointer(&vt[kVtSetCooperativeLevel], reinterpret_cast<void*>(&Hook_DDSetCooperativeLevel));
            PatchPointer(&vt[kVtSetDisplayMode], reinterpret_cast<void*>(&Hook_DDSetDisplayMode));
            BDX_INFO("Borderless: DirectDraw display mode changes neutralised");
        }
    }
    return hr;
}

void* PatchImport(HMODULE module, const char* dll, const char* func, void* hook)
{
    auto* base = reinterpret_cast<uint8_t*>(module);
    auto* dos  = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY& dirImp = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dirImp.VirtualAddress) return nullptr;
    for (auto* imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dirImp.VirtualAddress); imp->Name; ++imp) {
        if (_stricmp(reinterpret_cast<const char*>(base + imp->Name), dll) != 0) continue;
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        auto* iat   = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++iat) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            auto* byName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (strcmp(reinterpret_cast<const char*>(byName->Name), func) != 0) continue;
            void* original = reinterpret_cast<void*>(iat->u1.Function);
            PatchPointer(reinterpret_cast<void**>(&iat->u1.Function), hook);
            return original;
        }
    }
    return nullptr;
}
}

void Display::InstallModeChangeHooks()
{
    if (!m_borderless) return;
    HMODULE exe = GetModuleHandleW(nullptr);
    g_realChangeDisplaySettingsA = reinterpret_cast<PFN_ChangeDisplaySettingsA>(
        PatchImport(exe, "USER32.dll", "ChangeDisplaySettingsA", reinterpret_cast<void*>(&Hook_ChangeDisplaySettingsA)));
    g_realDirectDrawCreateEx = reinterpret_cast<PFN_DirectDrawCreateEx>(
        PatchImport(exe, "DDRAW.dll", "DirectDrawCreateEx", reinterpret_cast<void*>(&Hook_DirectDrawCreateEx)));
    auto hookUser = [&](const char* name, void* hook) { return PatchImport(exe, "USER32.dll", name, hook); };
    g_realGetClientRect  = reinterpret_cast<PFN_GetRect>(hookUser("GetClientRect", reinterpret_cast<void*>(&Hook_GetClientRect)));
    g_realGetWindowRect  = reinterpret_cast<PFN_GetRect>(hookUser("GetWindowRect", reinterpret_cast<void*>(&Hook_GetWindowRect)));
    g_realMoveWindow     = reinterpret_cast<PFN_MoveWindow>(hookUser("MoveWindow", reinterpret_cast<void*>(&Hook_MoveWindow)));
    g_realSetWindowPos   = reinterpret_cast<PFN_SetWindowPos>(hookUser("SetWindowPos", reinterpret_cast<void*>(&Hook_SetWindowPos)));
    g_realScreenToClient = reinterpret_cast<PFN_ScreenToClient>(hookUser("ScreenToClient", reinterpret_cast<void*>(&Hook_ScreenToClient)));
    BDX_INFO("Borderless: window hooks - GetClientRect %d, GetWindowRect %d, MoveWindow %d, SetWindowPos %d, ScreenToClient %d",
             g_realGetClientRect != nullptr, g_realGetWindowRect != nullptr, g_realMoveWindow != nullptr,
             g_realSetWindowPos != nullptr, g_realScreenToClient != nullptr);
    BDX_INFO("Borderless: mode-change hooks - ChangeDisplaySettingsA %s, DirectDrawCreateEx %s",
             g_realChangeDisplaySettingsA ? "hooked" : "not imported", g_realDirectDrawCreateEx ? "hooked" : "not imported");
}

Display& Display::Get()
{
    static Display d;
    return d;
}

void Display::LoadConfig()
{
    const Config& c = GetConfig();
    std::wstring mode = c.GetString("Display", "Mode", L"Borderless");
    std::wstring res  = c.GetString("Display", "Resolution", L"Game");
    for (auto& ch : mode) ch = static_cast<wchar_t>(towlower(ch));
    for (auto& ch : res) ch = static_cast<wchar_t>(towlower(ch));
    m_borderless        = mode == L"borderless";
    m_desktopResolution = res == L"desktop";
    m_vsync  = _wtoi(c.GetString("Display", "VSync", L"0").c_str()) != 0;
    telemetry::Init();
}

HWND Display::TargetWindow() const { return m_window && IsWindow(m_window) ? m_window : nullptr; }

RECT Display::MonitorRect(HWND wnd) const
{
    MONITORINFO mi = {sizeof(mi)};
    HMONITOR mon = MonitorFromWindow(wnd, MONITOR_DEFAULTTOPRIMARY);
    if (GetMonitorInfoW(mon, &mi)) return mi.rcMonitor;
    RECT r = {0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    return r;
}

void Display::MakeRealParameters(const D3DPRESENT_PARAMETERS& game, HWND focusWindow, D3DPRESENT_PARAMETERS* real)
{
    *real = game;
    if (!m_vsync) {
        if (game.PresentationInterval != D3DPRESENT_INTERVAL_IMMEDIATE) BDX_INFO("Display: the game asked for present interval 0x%x: no vsync", game.PresentationInterval);
        real->PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    }
    if (!m_borderless) return;

    m_window = game.hDeviceWindow ? game.hDeviceWindow : focusWindow;
    const RECT mr = MonitorRect(m_window);
    const UINT mw = static_cast<UINT>(mr.right - mr.left), mh = static_cast<UINT>(mr.bottom - mr.top);

    real->Windowed                   = TRUE;
    real->FullScreen_RefreshRateInHz = 0;
    if (m_desktopResolution || game.BackBufferWidth == 0 || game.BackBufferHeight == 0) {
        real->BackBufferWidth  = mw;
        real->BackBufferHeight = mh;
    }
    if (real->SwapEffect == D3DSWAPEFFECT_FLIP && real->BackBufferCount == 0) real->BackBufferCount = 1;
    m_gameWidth  = real->BackBufferWidth;
    m_gameHeight = real->BackBufferHeight;

    BDX_INFO("Borderless: game asked %ux%u (%s) -> back buffer %ux%u, stretched to the %ux%u monitor", game.BackBufferWidth,
             game.BackBufferHeight, game.Windowed ? "windowed" : "fullscreen", real->BackBufferWidth, real->BackBufferHeight, mw, mh);
}

void Display::ApplyWindow()
{
    HWND wnd = TargetWindow();
    if (!m_borderless || !wnd) return;

    const RECT mr = MonitorRect(wnd);
    LONG style = GetWindowLongW(wnd, GWL_STYLE);
    style &= ~(WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU | WS_BORDER | WS_DLGFRAME);
    style |= WS_POPUP | WS_VISIBLE;
    SetWindowLongW(wnd, GWL_STYLE, style);
    LONG ex = GetWindowLongW(wnd, GWL_EXSTYLE);
    ex &= ~(WS_EX_DLGMODALFRAME | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE | WS_EX_WINDOWEDGE);
    SetWindowLongW(wnd, GWL_EXSTYLE, ex);
    SetWindowPos(wnd, HWND_TOP, mr.left, mr.top, mr.right - mr.left, mr.bottom - mr.top,
                 SWP_FRAMECHANGED | SWP_SHOWWINDOW | SWP_NOOWNERZORDER);
}

void Display::Maintain()
{
    HWND wnd = TargetWindow();
    if (!m_borderless || !wnd) return;
    const uint64_t now = GetTickCount64();
    if (now - m_lastCheck < 500) return;
    m_lastCheck = now;
    if (IsIconic(wnd)) return;

    const RECT mr = MonitorRect(wnd);
    RECT wr = {};
    GetWindowRect(wnd, &wr);
    const LONG style = GetWindowLongW(wnd, GWL_STYLE);
    if ((style & (WS_CAPTION | WS_THICKFRAME)) || wr.left != mr.left || wr.top != mr.top || wr.right != mr.right ||
        wr.bottom != mr.bottom)
        ApplyWindow();
}

}
