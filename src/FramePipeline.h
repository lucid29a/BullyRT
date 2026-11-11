#pragma once
#include "Common.h"

namespace bdx12 {

class FramePipeline {
public:
    static FramePipeline& Get();

    void OnPresent(IDirect3DDevice9* device);
    void ReleaseDeviceObjects();

private:
    bool EnsureTargets(IDirect3DDevice9* device, UINT width, UINT height);
    bool RunPass(size_t passIndex, IDirect3DTexture9* in, IDirect3DTexture9* out, UINT w, UINT h, float dt);

    ComPtr<IDirect3DTexture9> m_targets[2];
    ComPtr<IDirect3DSurface9> m_surfaces[2];
    UINT                      m_width = 0, m_height = 0;
    LARGE_INTEGER             m_lastTime = {};
    LARGE_INTEGER             m_frequency = {};
    bool                      m_failed = false;
};

}
