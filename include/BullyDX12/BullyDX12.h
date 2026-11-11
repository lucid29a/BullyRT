// =====================================================================================
// BullyDX12 SDK - public extension API

#pragma once

#include <cstdint>
#include <cstddef>
#include <d3d9.h>
#include <d3d12.h>

namespace bdx12 {

constexpr uint32_t kApiVersion = 4;

enum class Backend : uint32_t {
    D3D9On12 = 0,
    Native   = 1,
    External = 2,
};

enum class LogLevel : uint32_t { Trace = 0, Info = 1, Warning = 2, Error = 3 };

enum class ShaderStage : uint32_t { Vertex = 0, Pixel = 1 };

enum class DrawKind : uint32_t { Primitive, IndexedPrimitive, PrimitiveUP, IndexedPrimitiveUP };

enum class DrawResult : uint32_t {
    Continue = 0,
    Skip     = 1,
};

struct D3D12Caps {
    D3D_FEATURE_LEVEL                 featureLevel;
    D3D_SHADER_MODEL                  highestShaderModel;
    D3D12_RAYTRACING_TIER             raytracingTier;
    D3D12_RESOURCE_BINDING_TIER       resourceBindingTier;
    D3D12_MESH_SHADER_TIER            meshShaderTier;
    D3D12_VARIABLE_SHADING_RATE_TIER  vrsTier;
    BOOL                              enhancedBarriers;
    LUID                              adapterLuid;
    wchar_t                           adapterName[128];
    uint64_t                          dedicatedVideoMemory;
};

struct DeviceInfo {
    IDirect3DDevice9*       device9;
    D3DPRESENT_PARAMETERS   presentParams;
    HWND                    focusWindow;
    UINT                    adapter;
    D3DDEVTYPE              deviceType;
};

struct DrawCallInfo {
    DrawKind                       kind;
    D3DPRIMITIVETYPE               primitiveType;
    INT                            baseVertexIndex;
    UINT                           minVertexIndex;
    UINT                           numVertices;
    UINT                           startIndexOrVertex;
    UINT                           primitiveCount;

    IDirect3DVertexBuffer9*        streamSource[4];
    UINT                           streamOffset[4];
    UINT                           streamStride[4];
    IDirect3DIndexBuffer9*         indexBuffer;
    IDirect3DVertexDeclaration9*   vertexDeclaration;
    DWORD                          fvf;

    IDirect3DVertexShader9*        vertexShader;
    IDirect3DPixelShader9*         pixelShader;
    uint64_t                       vertexShaderHash;
    uint64_t                       pixelShaderHash;

    const float*                   vsConstantsF;
    const float*                   psConstantsF;
    const D3DMATRIX*               worldTransform;
    const D3DMATRIX*               viewTransform;
    const D3DMATRIX*               projectionTransform;

    IDirect3DSurface9*             renderTarget0;
    IDirect3DSurface9*             depthStencil;
    IDirect3DBaseTexture9*         textures[8];
    uint64_t                       frameIndex;

    const DWORD*                   renderStates;
    D3DVIEWPORT9                   viewport;
};

struct PostProcessContext {
    ID3D12GraphicsCommandList*  commandList;
    ID3D12Resource*             input;
    ID3D12Resource*             output;
    DXGI_FORMAT                 format;
    UINT                        width;
    UINT                        height;
    uint64_t                    frameIndex;
    float                       deltaTimeSeconds;
};

struct FrameInfo {
    uint64_t                    frameIndex;
    float                       deltaTimeSeconds;
    IDirect3DSurface9*          backBuffer;
    UINT                        width;
    UINT                        height;
};

class IHost {
public:
    virtual uint32_t              GetApiVersion() const = 0;
    virtual Backend               GetBackend() const = 0;
    virtual void                  Log(LogLevel level, const char* fmt, ...) = 0;

    virtual int                   GetConfigInt(const char* section, const char* key, int defaultValue) = 0;
    virtual float                 GetConfigFloat(const char* section, const char* key, float defaultValue) = 0;
    virtual size_t                GetConfigString(const char* section, const char* key, const char* defaultValue,
                                                  char* out, size_t outSize) = 0;
    virtual const wchar_t*        GetGameDirectory() const = 0;

    virtual IDirect3DDevice9*     GetDevice9() = 0;

    virtual ID3D12Device*         GetD3D12Device() = 0;
    virtual ID3D12CommandQueue*   GetD3D12Queue() = 0;
    virtual const D3D12Caps*      GetD3D12Caps() = 0;

    virtual HRESULT               UnwrapResource(IDirect3DResource9* resource9, ID3D12Resource** outResource12) = 0;
    virtual HRESULT               ReturnResource(IDirect3DResource9* resource9) = 0;

    virtual ID3D12GraphicsCommandList* BeginCommandList() = 0;
    virtual uint64_t              ExecuteCommandList(ID3D12GraphicsCommandList* list) = 0;
    virtual uint64_t              GetCompletedFenceValue() = 0;
    virtual void                  WaitForFence(uint64_t value) = 0;
    virtual void                  DeferredRelease(IUnknown* object) = 0;

    virtual void                  PublishResource(const char* name, IUnknown* object) = 0;
    virtual IUnknown*             FindResource(const char* name) = 0;

    virtual uint64_t              GetFrameIndex() const = 0;

    virtual bool                  FindShaderConstant(ShaderStage stage, uint64_t shaderHash, const char* name,
                                                     UINT* outRegister, UINT* outCount, UINT* outRegisterSet) = 0;
};

class IExtension {
public:
    virtual ~IExtension() = default;

    virtual const char*  GetName() const = 0;
    virtual int          GetPriority() const { return 100; }

    virtual bool         OnInitialize(IHost* host) = 0;
    virtual void         OnShutdown() {}

    virtual void         OnDeviceCreated(const DeviceInfo& info) { (void)info; }
    virtual void         OnDeviceLost() {}
    virtual void         OnDeviceReset(const DeviceInfo& info) { (void)info; }

    virtual void         OnBeginScene(uint64_t frameIndex) { (void)frameIndex; }
    virtual void         OnEndScene(uint64_t frameIndex) { (void)frameIndex; }
    virtual DrawResult   OnDraw(const DrawCallInfo& draw) { (void)draw; return DrawResult::Continue; }

    virtual void         OnSetLight(DWORD index, const D3DLIGHT9& light) { (void)index; (void)light; }
    virtual void         OnLightEnable(DWORD index, BOOL enable) { (void)index; (void)enable; }

    virtual const DWORD* OnCreateShader(ShaderStage stage, uint64_t hash, const DWORD* bytecode, size_t sizeBytes)
    { (void)stage; (void)hash; (void)bytecode; (void)sizeBytes; return nullptr; }

    virtual bool         WantsPostProcess() const { return false; }
    virtual void         OnPostProcess(const PostProcessContext& ctx) { (void)ctx; }

    virtual void         OnPresent(const FrameInfo& frame) { (void)frame; }

    virtual void         OnClear(DWORD flags, IDirect3DSurface9* renderTarget0, IDirect3DSurface9* depthStencil)
    {
        (void)flags; (void)renderTarget0; (void)depthStencil;
    }
    virtual void         OnCopySurface(IDirect3DSurface9* source, IDirect3DSurface9* dest) { (void)source; (void)dest; }
};

using PFN_CreateExtension  = IExtension* (__cdecl*)(uint32_t hostApiVersion);
using PFN_DestroyExtension = void (__cdecl*)(IExtension*);

}

#define BULLYDX12_EXPORT extern "C" __declspec(dllexport)

#define BULLYDX12_DECLARE_EXTENSION(ClassName)                                                     \
    BULLYDX12_EXPORT bdx12::IExtension* __cdecl BullyDX12_CreateExtension(uint32_t hostApiVersion) \
    {                                                                                              \
        if (hostApiVersion < bdx12::kApiVersion) return nullptr;                                   \
        return new ClassName();                                                                    \
    }                                                                                              \
    BULLYDX12_EXPORT void __cdecl BullyDX12_DestroyExtension(bdx12::IExtension* e) { delete e; }
