#include "FramePipeline.h"
#include "Config.h"
#include "D3D12Context.h"
#include "ExtensionManager.h"

#include <wincodec.h>
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

namespace bdx12 {

namespace {
void SaveScreenshot(IDirect3DDevice9* device, IDirect3DSurface9* bb, const D3DSURFACE_DESC& desc, uint64_t frame)
{
    if (desc.Format != D3DFMT_X8R8G8B8 && desc.Format != D3DFMT_A8R8G8B8) {
        BDX_WARN("Screenshot: unsupported back buffer format %d", desc.Format);
        return;
    }
    ComPtr<IDirect3DSurface9> sys;
    if (FAILED(device->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM, &sys, nullptr)) ||
        FAILED(device->GetRenderTargetData(bb, sys.Get())))
        return;
    D3DLOCKED_RECT lr = {};
    if (FAILED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) return;

    const HRESULT coHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const std::wstring dir = GameDirectory() + L"BullyDX12\\screenshots";
    CreateDirectoryW((GameDirectory() + L"BullyDX12").c_str(), nullptr);
    CreateDirectoryW(dir.c_str(), nullptr);
    wchar_t name[64];
    swprintf_s(name, L"\\frame_%06llu.png", static_cast<unsigned long long>(frame));
    const std::wstring path = dir + name;

    ComPtr<IWICImagingFactory> wic;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> enc;
    ComPtr<IWICBitmapFrameEncode> fr;
    bool ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) &&
              SUCCEEDED(wic->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
              SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) &&
              SUCCEEDED(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache)) &&
              SUCCEEDED(enc->CreateNewFrame(&fr, nullptr)) && SUCCEEDED(fr->Initialize(nullptr)) &&
              SUCCEEDED(fr->SetSize(desc.Width, desc.Height));
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGR;
    ok = ok && SUCCEEDED(fr->SetPixelFormat(&fmt)) &&
         SUCCEEDED(fr->WritePixels(desc.Height, static_cast<UINT>(lr.Pitch), static_cast<UINT>(lr.Pitch) * desc.Height,
                                   static_cast<BYTE*>(lr.pBits))) &&
         SUCCEEDED(fr->Commit()) && SUCCEEDED(enc->Commit());
    sys->UnlockRect();
    fr.Reset(); enc.Reset(); stream.Reset(); wic.Reset();
    if (SUCCEEDED(coHr)) CoUninitialize();
    if (ok) BDX_INFO("Screenshot saved: %s", Narrow(path).c_str());
    else    BDX_WARN("Screenshot failed: %s", Narrow(path).c_str());
}

bool IsScreenshotFrame(uint64_t frame)
{
    static const std::vector<uint64_t> frames = [] {
        std::vector<uint64_t> v;
        const std::wstring list = GetConfig().GetString("Debug", "ScreenshotFrames", L"");
        const wchar_t* p = list.c_str();
        while (*p) {
            wchar_t* end = nullptr;
            const unsigned long long n = wcstoull(p, &end, 10);
            if (end == p) { ++p; continue; }
            v.push_back(n);
            p = end;
        }
        return v;
    }();
    for (uint64_t f : frames) if (f == frame) return true;
    return false;
}
}

FramePipeline& FramePipeline::Get()
{
    static FramePipeline s;
    return s;
}

void FramePipeline::ReleaseDeviceObjects()
{
    for (int i = 0; i < 2; ++i) {
        m_surfaces[i].Reset();
        m_targets[i].Reset();
    }
    m_width = m_height = 0;
    m_failed = false;
}

bool FramePipeline::EnsureTargets(IDirect3DDevice9* device, UINT width, UINT height)
{
    if (m_targets[0] && m_width == width && m_height == height) return true;
    ReleaseDeviceObjects();
    for (int i = 0; i < 2; ++i) {
        HRESULT hr = device->CreateTexture(width, height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
                                           &m_targets[i], nullptr);
        if (SUCCEEDED(hr)) hr = m_targets[i]->GetSurfaceLevel(0, &m_surfaces[i]);
        if (FAILED(hr)) {
            BDX_ERROR("Post-process target creation failed (%ux%u): %s", width, height, HResultName(hr));
            ReleaseDeviceObjects();
            return false;
        }
    }
    m_width  = width;
    m_height = height;
    BDX_INFO("Post-process targets created: %ux%u", width, height);
    return true;
}

bool FramePipeline::RunPass(size_t passIndex, IDirect3DTexture9* in, IDirect3DTexture9* out, UINT w, UINT h, float dt)
{
    D3D12Context& ctx = D3D12Context::Get();

    ComPtr<ID3D12Resource> in12, out12;
    if (FAILED(ctx.Unwrap(in, &in12))) return false;
    if (FAILED(ctx.Unwrap(out, &out12))) {
        ctx.Return(in);
        return false;
    }

    bool ok = false;
    if (ID3D12GraphicsCommandList* list = ctx.BeginCommandList()) {
        D3D12_RESOURCE_BARRIER b[2] = {};
        b[0].Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b[0].Transition.pResource   = in12.Get();
        b[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        b[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        b[1]                        = b[0];
        b[1].Transition.pResource   = out12.Get();
        b[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
        list->ResourceBarrier(2, b);

        PostProcessContext pc = {};
        pc.commandList        = list;
        pc.input              = in12.Get();
        pc.output             = out12.Get();
        pc.format             = out12->GetDesc().Format;
        pc.width              = w;
        pc.height             = h;
        pc.frameIndex         = ExtensionManager::Get().GetFrameIndex();
        pc.deltaTimeSeconds   = dt;
        ExtensionManager::Get().DispatchPostProcess(pc, passIndex);

        std::swap(b[0].Transition.StateBefore, b[0].Transition.StateAfter);
        std::swap(b[1].Transition.StateBefore, b[1].Transition.StateAfter);
        list->ResourceBarrier(2, b);
        ctx.Execute(list);
        ok = true;
    }

    ctx.DeferredRelease(in12.Detach());
    ctx.DeferredRelease(out12.Detach());
    ctx.Return(in);
    ctx.Return(out);
    return ok;
}

void FramePipeline::OnPresent(IDirect3DDevice9* device)
{
    if (!m_frequency.QuadPart) {
        QueryPerformanceFrequency(&m_frequency);
        QueryPerformanceCounter(&m_lastTime);
    }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const float dt = static_cast<float>(now.QuadPart - m_lastTime.QuadPart) / static_cast<float>(m_frequency.QuadPart);
    m_lastTime = now;

    ExtensionManager& em = ExtensionManager::Get();
    D3D12Context&     ctx = D3D12Context::Get();

    ComPtr<IDirect3DSurface9> backBuffer;
    device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer);
    D3DSURFACE_DESC bbDesc = {};
    if (backBuffer) backBuffer->GetDesc(&bbDesc);

    const bool canPostProcess = !m_failed && backBuffer && GetConfig().postProcessing &&
                                em.GetBackend() == Backend::D3D9On12 && ctx.IsReady() && ctx.Device9On12() &&
                                em.AnyWantsPostProcess();

    if (canPostProcess && EnsureTargets(device, bbDesc.Width, bbDesc.Height)) {
        HRESULT hr = device->StretchRect(backBuffer.Get(), nullptr, m_surfaces[0].Get(), nullptr, D3DTEXF_NONE);
        if (SUCCEEDED(hr)) {
            int cur = 0;
            const size_t passes = em.PostProcessPassCount();
            for (size_t i = 0; i < passes; ++i) {
                if (RunPass(i, m_targets[cur].Get(), m_targets[1 - cur].Get(), bbDesc.Width, bbDesc.Height, dt))
                    cur = 1 - cur;
            }
            if (cur != 0 || passes > 0)
                hr = device->StretchRect(m_surfaces[cur].Get(), nullptr, backBuffer.Get(), nullptr, D3DTEXF_NONE);
        }
        if (FAILED(hr)) {
            BDX_ERROR("Post-process StretchRect failed: %s - post-processing disabled", HResultName(hr));
            m_failed = true;
        }
    }

    if (em.GetFrameIndex() == 0)
        Startup("first frame presented: the game is running");
        BDX_INFO("First frame presented (%ux%u, backend %s, D3D12 post-process %s)", bbDesc.Width, bbDesc.Height,
                 em.GetBackend() == Backend::D3D9On12 ? "D3D9On12" : em.GetBackend() == Backend::Native ? "Native" : "External",
                 canPostProcess ? "active" : "inactive");

    FrameInfo fi        = {};
    fi.frameIndex       = em.GetFrameIndex();
    fi.deltaTimeSeconds = dt;
    fi.backBuffer       = backBuffer.Get();
    fi.width            = bbDesc.Width;
    fi.height           = bbDesc.Height;
    em.DispatchPresent(fi);

    if (backBuffer && IsScreenshotFrame(fi.frameIndex)) SaveScreenshot(device, backBuffer.Get(), bbDesc, fi.frameIndex);

    em.AdvanceFrame(dt);
    if (ctx.IsReady()) ctx.CollectGarbage();
}

}
