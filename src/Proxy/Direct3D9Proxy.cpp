#include "Direct3D9Proxy.h"
#include "Device9Proxy.h"
#include "../Display.h"

namespace bdx12 {

Direct3D9Proxy::Direct3D9Proxy(IDirect3D9* real, UINT preferredAdapter) : m_real(real), m_preferred(preferredAdapter)
{
    if (m_preferred >= m_real->GetAdapterCount()) m_preferred = D3DADAPTER_DEFAULT;
}

Direct3D9Proxy::~Direct3D9Proxy() { m_real->Release(); }

UINT Direct3D9Proxy::Map(UINT adapter) const
{
    if (m_preferred == D3DADAPTER_DEFAULT) return adapter;
    if (adapter == D3DADAPTER_DEFAULT) return m_preferred;
    if (adapter == m_preferred) return D3DADAPTER_DEFAULT;
    return adapter;
}

HRESULT Direct3D9Proxy::QueryInterface(REFIID riid, void** ppv)
{
    if (!ppv) return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IDirect3D9)) {
        *ppv = static_cast<IDirect3D9*>(this);
        AddRef();
        return S_OK;
    }
    return m_real->QueryInterface(riid, ppv);
}

ULONG Direct3D9Proxy::AddRef() { return static_cast<ULONG>(InterlockedIncrement(&m_refs)); }

ULONG Direct3D9Proxy::Release()
{
    const LONG r = InterlockedDecrement(&m_refs);
    if (r == 0) delete this;
    return static_cast<ULONG>(r);
}

HRESULT Direct3D9Proxy::RegisterSoftwareDevice(void* f) { return m_real->RegisterSoftwareDevice(f); }
UINT    Direct3D9Proxy::GetAdapterCount() { return m_real->GetAdapterCount(); }
HRESULT Direct3D9Proxy::GetAdapterIdentifier(UINT a, DWORD f, D3DADAPTER_IDENTIFIER9* p) { return m_real->GetAdapterIdentifier(Map(a), f, p); }
UINT    Direct3D9Proxy::GetAdapterModeCount(UINT a, D3DFORMAT f) { return m_real->GetAdapterModeCount(Map(a), f); }
HRESULT Direct3D9Proxy::EnumAdapterModes(UINT a, D3DFORMAT f, UINT m, D3DDISPLAYMODE* p) { return m_real->EnumAdapterModes(Map(a), f, m, p); }
HRESULT Direct3D9Proxy::GetAdapterDisplayMode(UINT a, D3DDISPLAYMODE* p) { return m_real->GetAdapterDisplayMode(Map(a), p); }
HRESULT Direct3D9Proxy::CheckDeviceType(UINT a, D3DDEVTYPE t, D3DFORMAT af, D3DFORMAT bf, BOOL w) { return m_real->CheckDeviceType(Map(a), t, af, bf, w); }
HRESULT Direct3D9Proxy::CheckDeviceFormat(UINT a, D3DDEVTYPE t, D3DFORMAT af, DWORD u, D3DRESOURCETYPE r, D3DFORMAT f) { return m_real->CheckDeviceFormat(Map(a), t, af, u, r, f); }
HRESULT Direct3D9Proxy::CheckDeviceMultiSampleType(UINT a, D3DDEVTYPE t, D3DFORMAT f, BOOL w, D3DMULTISAMPLE_TYPE m, DWORD* q) { return m_real->CheckDeviceMultiSampleType(Map(a), t, f, w, m, q); }
HRESULT Direct3D9Proxy::CheckDepthStencilMatch(UINT a, D3DDEVTYPE t, D3DFORMAT af, D3DFORMAT rf, D3DFORMAT df) { return m_real->CheckDepthStencilMatch(Map(a), t, af, rf, df); }
HRESULT Direct3D9Proxy::CheckDeviceFormatConversion(UINT a, D3DDEVTYPE t, D3DFORMAT s, D3DFORMAT d) { return m_real->CheckDeviceFormatConversion(Map(a), t, s, d); }
HRESULT Direct3D9Proxy::GetDeviceCaps(UINT a, D3DDEVTYPE t, D3DCAPS9* p)
{
    const HRESULT hr = m_real->GetDeviceCaps(Map(a), t, p);
    if (SUCCEEDED(hr) && p) p->AdapterOrdinal = a;
    return hr;
}
HMONITOR Direct3D9Proxy::GetAdapterMonitor(UINT a) { return m_real->GetAdapterMonitor(Map(a)); }

HRESULT Direct3D9Proxy::CreateDevice(UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow, DWORD BehaviorFlags,
                                     D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** ppDevice)
{
    Startup("CreateDevice called by the game");
    if (!ppDevice) return D3DERR_INVALIDCALL;
    *ppDevice = nullptr;

    const UINT real = Map(Adapter);
    if (!pp) return D3DERR_INVALIDCALL;
    D3DPRESENT_PARAMETERS realPp;
    Display::Get().MakeRealParameters(*pp, hFocusWindow, &realPp);
    BDX_INFO("CreateDevice(adapter %u -> %u, type %d, flags 0x%08lX, %ux%u, fmt %d, windowed %d, ms %d)", Adapter, real,
             DeviceType, BehaviorFlags, realPp.BackBufferWidth, realPp.BackBufferHeight,
             realPp.BackBufferFormat, realPp.Windowed, realPp.MultiSampleType);

    IDirect3DDevice9* dev = nullptr;
    const HRESULT hr = m_real->CreateDevice(real, DeviceType, hFocusWindow, BehaviorFlags, &realPp, &dev);
    if (FAILED(hr)) {
        BDX_ERROR("IDirect3D9::CreateDevice failed: %s", HResultName(hr));
        return hr;
    }

    *ppDevice = new Device9Proxy(dev, this, &realPp, hFocusWindow, real, DeviceType);
    Display::Get().ApplyWindow();
    return hr;
}

}
