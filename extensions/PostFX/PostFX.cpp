// BullyDX12 PostFX extension: a complete example of a native D3D12 render pass injected into the
// game's frame (root signature, PSO, descriptor heaps, per-format pipeline cache, INI settings).

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <wrl/client.h>
#include <unordered_map>

#include "BullyDX12/BullyDX12.h"

#include "PostFX_VSMain.h"
#include "PostFX_PSMain.h"

using Microsoft::WRL::ComPtr;
using namespace bdx12;

namespace {

DXGI_FORMAT TypedFormat(DXGI_FORMAT f)
{
    switch (f) {
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:  return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:  return DXGI_FORMAT_B8G8R8X8_UNORM;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:  return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default:                             return f;
    }
}

struct Params {
    float invSize[2];
    float sharpen;
    float saturation;
    float vibrance;
    float contrast;
    float vignette;
    float gamma;
};
static_assert(sizeof(Params) == 32, "root constants layout");

class PostFX final : public IExtension {
public:
    const char* GetName() const override { return "PostFX"; }
    int         GetPriority() const override { return m_priority; }
    bool        WantsPostProcess() const override { return m_enabled; }

    bool OnInitialize(IHost* host) override
    {
        m_host     = host;
        m_enabled  = host->GetConfigInt("PostFX", "Enabled", 1) != 0;
        m_priority = host->GetConfigInt("PostFX", "Priority", 500);
        if (!m_enabled) return false;

        if (host->GetBackend() != Backend::D3D9On12) {
            host->Log(LogLevel::Warning, "PostFX: requires Backend=D3D9On12, disabled");
            return false;
        }
        m_params.sharpen    = host->GetConfigFloat("PostFX", "Sharpen", 0.35f);
        m_params.saturation = host->GetConfigFloat("PostFX", "Saturation", 1.05f);
        m_params.vibrance   = host->GetConfigFloat("PostFX", "Vibrance", 0.10f);
        m_params.contrast   = host->GetConfigFloat("PostFX", "Contrast", 1.03f);
        m_params.vignette   = host->GetConfigFloat("PostFX", "Vignette", 0.15f);
        m_params.gamma      = host->GetConfigFloat("PostFX", "Gamma", 1.0f);
        return true;
    }

    void OnShutdown() override
    {
        m_psos.clear();
        m_rootSig.Reset();
        m_srvHeap.Reset();
        m_rtvHeap.Reset();
    }

    void OnPostProcess(const PostProcessContext& ctx) override
    {
        if (!EnsureBaseObjects()) return;
        const DXGI_FORMAT fmt = TypedFormat(ctx.format);
        ID3D12PipelineState* pso = GetPso(fmt);
        if (!pso) return;

        ID3D12Device* dev = m_host->GetD3D12Device();
        const UINT slot   = m_ring++ % kRing;

        D3D12_CPU_DESCRIPTOR_HANDLE srvCpu = m_srvHeap->GetCPUDescriptorHandleForHeapStart();
        D3D12_GPU_DESCRIPTOR_HANDLE srvGpu = m_srvHeap->GetGPUDescriptorHandleForHeapStart();
        srvCpu.ptr += static_cast<SIZE_T>(slot) * m_srvInc;
        srvGpu.ptr += static_cast<UINT64>(slot) * m_srvInc;
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += static_cast<SIZE_T>(slot) * m_rtvInc;

        D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
        srv.Format                          = TypedFormat(ctx.input->GetDesc().Format);
        srv.ViewDimension                   = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping         = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels             = 1;
        dev->CreateShaderResourceView(ctx.input, &srv, srvCpu);

        D3D12_RENDER_TARGET_VIEW_DESC rd = {};
        rd.Format                        = fmt;
        rd.ViewDimension                 = D3D12_RTV_DIMENSION_TEXTURE2D;
        dev->CreateRenderTargetView(ctx.output, &rd, rtv);

        Params p    = m_params;
        p.invSize[0] = 1.0f / static_cast<float>(ctx.width);
        p.invSize[1] = 1.0f / static_cast<float>(ctx.height);

        ID3D12GraphicsCommandList* cl = ctx.commandList;
        ID3D12DescriptorHeap* heaps[] = {m_srvHeap.Get()};
        cl->SetDescriptorHeaps(1, heaps);
        cl->SetGraphicsRootSignature(m_rootSig.Get());
        cl->SetGraphicsRoot32BitConstants(0, sizeof(Params) / 4, &p, 0);
        cl->SetGraphicsRootDescriptorTable(1, srvGpu);
        cl->SetPipelineState(pso);
        const D3D12_VIEWPORT vp = {0, 0, static_cast<float>(ctx.width), static_cast<float>(ctx.height), 0, 1};
        const D3D12_RECT     sc = {0, 0, static_cast<LONG>(ctx.width), static_cast<LONG>(ctx.height)};
        cl->RSSetViewports(1, &vp);
        cl->RSSetScissorRects(1, &sc);
        cl->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cl->DrawInstanced(3, 1, 0, 0);
    }

private:
    static constexpr UINT kRing = 256;

    bool EnsureBaseObjects()
    {
        if (m_rootSig) return true;
        if (m_broken) return false;
        ID3D12Device* dev = m_host->GetD3D12Device();
        if (!dev) return false;

        D3D12_DESCRIPTOR_RANGE range            = {};
        range.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors                    = 1;
        range.OffsetInDescriptorsFromTableStart = 0;

        D3D12_ROOT_PARAMETER params[2]                 = {};
        params[0].ParameterType                        = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants.Num32BitValues             = sizeof(Params) / 4;
        params[0].ShaderVisibility                     = D3D12_SHADER_VISIBILITY_PIXEL;
        params[1].ParameterType                        = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable.NumDescriptorRanges  = 1;
        params[1].DescriptorTable.pDescriptorRanges    = &range;
        params[1].ShaderVisibility                     = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC rs = {};
        rs.NumParameters             = 2;
        rs.pParameters               = params;
        rs.Flags = D3D12_ROOT_SIGNATURE_FLAG_DENY_VERTEX_SHADER_ROOT_ACCESS |
                   D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
                   D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
                   D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

        ComPtr<ID3DBlob> blob, err;
        HRESULT hr = D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err);
        if (SUCCEEDED(hr))
            hr = dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&m_rootSig));
        if (FAILED(hr)) {
            m_host->Log(LogLevel::Error, "PostFX: root signature failed: %s",
                        err ? static_cast<const char*>(err->GetBufferPointer()) : "");
            m_broken = true;
            return false;
        }

        D3D12_DESCRIPTOR_HEAP_DESC hd = {};
        hd.Type                       = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors             = kRing;
        hd.Flags                      = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        hr = dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_srvHeap));
        if (SUCCEEDED(hr)) {
            hd.Type  = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
            hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
            hr       = dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_rtvHeap));
        }
        if (FAILED(hr)) {
            m_host->Log(LogLevel::Error, "PostFX: descriptor heap creation failed (0x%08lX)", hr);
            m_rootSig.Reset();
            m_broken = true;
            return false;
        }
        m_srvInc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        m_rtvInc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        m_host->Log(LogLevel::Info, "PostFX: D3D12 pipeline ready");
        return true;
    }

    ID3D12PipelineState* GetPso(DXGI_FORMAT fmt)
    {
        auto it = m_psos.find(fmt);
        if (it != m_psos.end()) return it->second.Get();

        D3D12_GRAPHICS_PIPELINE_STATE_DESC d = {};
        d.pRootSignature                     = m_rootSig.Get();
        d.VS                                 = {g_PostFX_VSMain, sizeof(g_PostFX_VSMain)};
        d.PS                                 = {g_PostFX_PSMain, sizeof(g_PostFX_PSMain)};
        d.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        d.SampleMask                         = UINT_MAX;
        d.RasterizerState.FillMode           = D3D12_FILL_MODE_SOLID;
        d.RasterizerState.CullMode           = D3D12_CULL_MODE_NONE;
        d.RasterizerState.DepthClipEnable    = TRUE;
        d.PrimitiveTopologyType              = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        d.NumRenderTargets                   = 1;
        d.RTVFormats[0]                      = fmt;
        d.SampleDesc.Count                   = 1;

        ComPtr<ID3D12PipelineState> pso;
        const HRESULT hr = m_host->GetD3D12Device()->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso));
        if (FAILED(hr)) {
            m_host->Log(LogLevel::Error, "PostFX: PSO creation failed for format %d (0x%08lX)", fmt, hr);
        }
        m_psos[fmt] = pso;
        return pso.Get();
    }

    IHost*                                                    m_host = nullptr;
    bool                                                      m_enabled = true;
    bool                                                      m_broken = false;
    int                                                       m_priority = 500;
    Params                                                    m_params = {};
    ComPtr<ID3D12RootSignature>                               m_rootSig;
    ComPtr<ID3D12DescriptorHeap>                              m_srvHeap, m_rtvHeap;
    UINT                                                      m_srvInc = 0, m_rtvInc = 0;
    UINT                                                      m_ring = 0;
    std::unordered_map<DXGI_FORMAT, ComPtr<ID3D12PipelineState>> m_psos;
};

}

BULLYDX12_DECLARE_EXTENSION(PostFX)
