#pragma once
#include "Common.h"

#include <map>

namespace bdx12 {

class ExtensionManager final : public IHost {
public:
    static ExtensionManager& Get();

    void LoadAll();
    void UnloadAll();

    void SetBackend(Backend b) { m_backend = b; }
    void SetDevice9(IDirect3DDevice9* dev) { m_device9 = dev; }
    void AdvanceFrame(float dt) { ++m_frameIndex; m_lastDelta = dt; }
    float LastDelta() const { return m_lastDelta; }

    bool HasExtensions() const { return !m_extensions.empty(); }
    bool AnyWantsPostProcess() const;

    void         DispatchDeviceCreated(const DeviceInfo& info);
    void         DispatchDeviceLost();
    void         DispatchDeviceReset(const DeviceInfo& info);
    void         DispatchBeginScene();
    void         DispatchEndScene();
    void         DispatchCopySurface(IDirect3DSurface9* source, IDirect3DSurface9* dest);
    void         DispatchClear(DWORD flags, IDirect3DSurface9* renderTarget0, IDirect3DSurface9* depthStencil);
    DrawResult   DispatchDraw(const DrawCallInfo& draw);
    void         DispatchSetLight(DWORD index, const D3DLIGHT9& light);
    void         DispatchLightEnable(DWORD index, BOOL enable);
    const DWORD* DispatchCreateShader(ShaderStage stage, uint64_t hash, const DWORD* code, size_t size);
    void         DispatchPostProcess(const PostProcessContext& ctx, size_t passIndex);
    size_t       PostProcessPassCount() const;
    void         DispatchPresent(const FrameInfo& frame);

    uint32_t                   GetApiVersion() const override { return kApiVersion; }
    Backend                    GetBackend() const override { return m_backend; }
    void                       Log(LogLevel level, const char* fmt, ...) override;
    int                        GetConfigInt(const char* section, const char* key, int def) override;
    float                      GetConfigFloat(const char* section, const char* key, float def) override;
    size_t                     GetConfigString(const char* section, const char* key, const char* def, char* out,
                                               size_t outSize) override;
    const wchar_t*             GetGameDirectory() const override;
    IDirect3DDevice9*          GetDevice9() override { return m_device9; }
    ID3D12Device*              GetD3D12Device() override;
    ID3D12CommandQueue*        GetD3D12Queue() override;
    const D3D12Caps*           GetD3D12Caps() override;
    HRESULT                    UnwrapResource(IDirect3DResource9* res9, ID3D12Resource** out) override;
    HRESULT                    ReturnResource(IDirect3DResource9* res9) override;
    ID3D12GraphicsCommandList* BeginCommandList() override;
    uint64_t                   ExecuteCommandList(ID3D12GraphicsCommandList* list) override;
    uint64_t                   GetCompletedFenceValue() override;
    void                       WaitForFence(uint64_t value) override;
    void                       DeferredRelease(IUnknown* object) override;
    void                       PublishResource(const char* name, IUnknown* object) override;
    IUnknown*                  FindResource(const char* name) override;
    uint64_t                   GetFrameIndex() const override { return m_frameIndex; }
    bool                       FindShaderConstant(ShaderStage stage, uint64_t hash, const char* name, UINT* reg,
                                                  UINT* count, UINT* set) override;

private:
    struct Loaded {
        HMODULE              module = nullptr;
        IExtension*          ext = nullptr;
        PFN_DestroyExtension destroy = nullptr;
        std::string          name;
    };

    bool D3D12Available() const;

    std::vector<Loaded>                       m_extensions;
    std::vector<IExtension*>                  m_postProcessors;
    std::map<std::string, ComPtr<IUnknown>>   m_registry;
    Backend                                   m_backend = Backend::Native;
    IDirect3DDevice9*                         m_device9 = nullptr;
    uint64_t                                  m_frameIndex = 0;
    float                                     m_lastDelta = 0.0f;
};

}
