#include "Backend.h"
#include "D3D12Context.h"

namespace bdx12 {

HMODULE SystemD3D9()
{
    static HMODULE mod = [] {
        wchar_t dir[MAX_PATH] = {};
        GetSystemDirectoryW(dir, MAX_PATH);
        const std::wstring path = std::wstring(dir) + L"\\d3d9.dll";
        HMODULE m = LoadLibraryW(path.c_str());
        if (!m) BDX_ERROR("Unable to load system d3d9.dll from %s (error %lu)", Narrow(path).c_str(), GetLastError());
        return m;
    }();
    return mod;
}

namespace {
bool LuidEqual(const LUID& a, const LUID& b) { return a.LowPart == b.LowPart && a.HighPart == b.HighPart; }

bool FindD3D9Ordinal(UINT sdkVersion, const LUID& luid, UINT* outOrdinal, LUID* outDefaultLuid)
{
    using PFN_Create9Ex = HRESULT(WINAPI*)(UINT, IDirect3D9Ex**);
    auto create9Ex = reinterpret_cast<PFN_Create9Ex>(GetProcAddress(SystemD3D9(), "Direct3DCreate9Ex"));
    if (!create9Ex) return false;
    ComPtr<IDirect3D9Ex> ex;
    if (FAILED(create9Ex(sdkVersion, &ex))) return false;

    bool found = false;
    const UINT count = ex->GetAdapterCount();
    for (UINT i = 0; i < count; ++i) {
        LUID l = {};
        if (FAILED(ex->GetAdapterLUID(i, &l))) continue;
        if (i == D3DADAPTER_DEFAULT && outDefaultLuid) *outDefaultLuid = l;
        if (!found && LuidEqual(l, luid)) {
            *outOrdinal = i;
            found = true;
        }
    }
    return found;
}
}

bool CreateBackendNative(UINT sdkVersion, BackendResult* out)
{
    using PFN = IDirect3D9*(WINAPI*)(UINT);
    auto fn = reinterpret_cast<PFN>(GetProcAddress(SystemD3D9(), "Direct3DCreate9"));
    if (!fn) return false;
    out->d3d9             = fn(sdkVersion);
    out->type             = Backend::Native;
    out->preferredAdapter = D3DADAPTER_DEFAULT;
    if (out->d3d9) BDX_INFO("Backend: native Direct3D 9");
    return out->d3d9 != nullptr;
}

bool CreateBackendExternal(UINT sdkVersion, const Config& cfg, BackendResult* out)
{
    if (cfg.externalDll.empty()) {
        BDX_ERROR("Backend=External but [Renderer] ExternalDll is empty");
        return false;
    }
    HMODULE mod = LoadLibraryExW(cfg.externalDll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!mod) {
        BDX_ERROR("Failed to load external renderer %s (error %lu)", Narrow(cfg.externalDll).c_str(), GetLastError());
        return false;
    }
    using PFN = IDirect3D9*(WINAPI*)(UINT);
    auto fn = reinterpret_cast<PFN>(GetProcAddress(mod, "Direct3DCreate9"));
    if (!fn) {
        BDX_ERROR("External renderer %s does not export Direct3DCreate9", Narrow(cfg.externalDll).c_str());
        return false;
    }
    out->d3d9             = fn(sdkVersion);
    out->type             = Backend::External;
    out->preferredAdapter = D3DADAPTER_DEFAULT;
    if (out->d3d9) BDX_INFO("Backend: external renderer %s", Narrow(cfg.externalDll).c_str());
    return out->d3d9 != nullptr;
}

bool CreateBackendD3D9On12(UINT sdkVersion, const Config& cfg, BackendResult* out)
{
    auto create9On12 = reinterpret_cast<PFN_Direct3DCreate9On12>(GetProcAddress(SystemD3D9(), "Direct3DCreate9On12"));
    if (!create9On12) {
        BDX_ERROR("System d3d9.dll has no Direct3DCreate9On12 (requires Windows 10 1803 or newer)");
        return false;
    }

    LUID         luid = {};
    std::wstring name;
    if (!D3D12Context::PickAdapterLuid(cfg, &luid, &name)) return false;

    UINT ordinal     = D3DADAPTER_DEFAULT;
    LUID defaultLuid = {};
    if (!FindD3D9Ordinal(sdkVersion, luid, &ordinal, &defaultLuid)) {
        BDX_WARN("Preferred GPU '%s' has no display output visible to D3D9 - using D3D9's default adapter",
                 Narrow(name).c_str());
        luid    = defaultLuid;
        ordinal = D3DADAPTER_DEFAULT;
    }

    if (!D3D12Context::Get().Initialize(luid, cfg)) return false;

    D3D9ON12_ARGS args    = {};
    args.Enable9On12      = TRUE;
    args.pD3D12Device     = D3D12Context::Get().Device();
    args.ppD3D12Queues[0] = D3D12Context::Get().Queue();
    args.NumQueues        = 1;
    args.NodeMask         = 0;

    IDirect3D9* d3d = create9On12(sdkVersion, &args, 1);
    if (!d3d) {
        BDX_ERROR("Direct3DCreate9On12 failed");
        D3D12Context::Get().Shutdown();
        return false;
    }

    out->d3d9             = d3d;
    out->type             = Backend::D3D9On12;
    out->preferredAdapter = ordinal;
    BDX_INFO("Backend: D3D9On12 on '%s' (D3D9 adapter ordinal %u)", Narrow(D3D12Context::Get().Caps().adapterName).c_str(),
             ordinal);
    return true;
}

BackendResult CreateBackend(UINT sdkVersion, const Config& cfg)
{
    BackendResult r;
    bool ok = false;
    switch (cfg.backend) {
    case Backend::D3D9On12: ok = CreateBackendD3D9On12(sdkVersion, cfg, &r); break;
    case Backend::External: ok = CreateBackendExternal(sdkVersion, cfg, &r); break;
    case Backend::Native:   ok = CreateBackendNative(sdkVersion, &r); break;
    }
    if (!ok && cfg.backend != Backend::Native) {
        if (cfg.fallbackToNative) {
            BDX_WARN("Configured backend failed - falling back to native Direct3D 9");
            r  = BackendResult{};
            ok = CreateBackendNative(sdkVersion, &r);
        } else {
            BDX_ERROR("Configured backend failed and FallbackToNative=0");
        }
    }
    return r;
}

}
