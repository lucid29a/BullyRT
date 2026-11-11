#pragma once
#include "Common.h"
#include "Config.h"

namespace bdx12 {

struct BackendResult {
    IDirect3D9* d3d9 = nullptr;
    UINT        preferredAdapter = D3DADAPTER_DEFAULT;
    Backend     type = Backend::Native;
};

HMODULE SystemD3D9();

bool CreateBackendD3D9On12(UINT sdkVersion, const Config& cfg, BackendResult* out);
bool CreateBackendNative(UINT sdkVersion, BackendResult* out);
bool CreateBackendExternal(UINT sdkVersion, const Config& cfg, BackendResult* out);

BackendResult CreateBackend(UINT sdkVersion, const Config& cfg);

}
