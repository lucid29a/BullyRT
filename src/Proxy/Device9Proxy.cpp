#include "Device9Proxy.h"
#include "Direct3D9Proxy.h"
#include "../D3D12Context.h"
#include "../ExtensionManager.h"
#include "../FramePipeline.h"
#include "../ShaderOverride.h"
#include "../Display.h"
#include "../Telemetry.h"

#include <algorithm>

namespace bdx12 {

namespace {
class StateBlock9Proxy final : public IDirect3DStateBlock9 {
public:
    StateBlock9Proxy(IDirect3DStateBlock9* real, Device9Proxy* dev) : m_real(real), m_dev(dev) { m_dev->AddRef(); }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IDirect3DStateBlock9)) {
            *ppv = static_cast<IDirect3DStateBlock9*>(this);
            AddRef();
            return S_OK;
        }
        return m_real->QueryInterface(riid, ppv);
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&m_refs)); }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const LONG r = InterlockedDecrement(&m_refs);
        if (r == 0) {
            m_real->Release();
            m_dev->Release();
            delete this;
        }
        return static_cast<ULONG>(r);
    }
    HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9** ppDevice) override
    {
        if (!ppDevice) return D3DERR_INVALIDCALL;
        m_dev->AddRef();
        *ppDevice = m_dev;
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE Capture() override { return m_real->Capture(); }
    HRESULT STDMETHODCALLTYPE Apply() override
    {
        const HRESULT hr = m_real->Apply();
        m_dev->MarkStateDirty();
        return hr;
    }

private:
    IDirect3DStateBlock9* m_real;
    Device9Proxy*         m_dev;
    volatile LONG         m_refs = 1;
};

template <typename T>
T* Weak(T* p)
{
    if (p) p->Release();
    return p;
}
}

Device9Proxy::Device9Proxy(IDirect3DDevice9* real, Direct3D9Proxy* d3d, const D3DPRESENT_PARAMETERS* pp, HWND focus,
                           UINT adapter, D3DDEVTYPE type)
    : m_real(real), m_d3d(d3d), m_focus(focus), m_adapter(adapter), m_type(type)
{
    m_d3d->AddRef();
    if (pp) m_pp = *pp;

    ExtensionManager& em = ExtensionManager::Get();
    em.SetDevice9(m_real);

    IDirect3DDevice9On12* on12 = nullptr;
    if (SUCCEEDED(m_real->QueryInterface(__uuidof(IDirect3DDevice9On12), reinterpret_cast<void**>(&on12))) && on12) {
        ComPtr<ID3D12Device> d12;
        on12->GetD3D12Device(IID_PPV_ARGS(&d12));
        const bool sameDevice = d12 && d12.Get() == D3D12Context::Get().Device();
        D3D12Context::Get().SetDevice9On12(sameDevice ? on12 : nullptr);
        on12->Release();
        if (sameDevice)
            BDX_INFO("D3D9 device is running on D3D9On12 (shared ID3D12Device/queue confirmed)");
        else
            BDX_WARN("D3D9On12 device does not use the BullyDX12 D3D12 device - D3D12 interop disabled");
    } else if (em.GetBackend() == Backend::D3D9On12) {
        BDX_WARN("Device does not expose IDirect3DDevice9On12 - running without D3D12 interop");
    }

    ResetShadow();
    em.DispatchDeviceCreated(MakeDeviceInfo());
}

Device9Proxy::~Device9Proxy() { m_d3d->Release(); }

void Device9Proxy::OnDeviceGoingAway()
{
    ExtensionManager::Get().DispatchDeviceLost();
    FramePipeline::Get().ReleaseDeviceObjects();
    if (D3D12Context::Get().IsReady()) {
        D3D12Context::Get().WaitIdle();
        D3D12Context::Get().CollectGarbage();
    }
}

HRESULT Device9Proxy::QueryInterface(REFIID riid, void** ppv)
{
    if (!ppv) return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IDirect3DDevice9)) {
        *ppv = static_cast<IDirect3DDevice9*>(this);
        AddRef();
        return S_OK;
    }
    return m_real->QueryInterface(riid, ppv);
}

ULONG Device9Proxy::AddRef() { return static_cast<ULONG>(InterlockedIncrement(&m_refs)); }

ULONG Device9Proxy::Release()
{
    const LONG r = InterlockedDecrement(&m_refs);
    if (r == 0) {
        BDX_INFO("Device released by the game - shutting down device objects");
        OnDeviceGoingAway();
        BDX_TRACE("Device objects released, GPU idle");
        D3D12Context::Get().SetDevice9On12(nullptr);
        ExtensionManager::Get().SetDevice9(nullptr);
        ShaderRegistry::Get().Clear();
        const ULONG left = m_real->Release();
        BDX_INFO("Backend device released (remaining references: %lu)", left);
        delete this;
    }
    return static_cast<ULONG>(r);
}

DeviceInfo Device9Proxy::MakeDeviceInfo() const
{
    DeviceInfo i    = {};
    i.device9       = m_real;
    i.presentParams = m_pp;
    i.focusWindow   = m_focus;
    i.adapter       = m_adapter;
    i.deviceType    = m_type;
    return i;
}

void Device9Proxy::ResetShadow()
{
    m_s = Shadow{};
    const D3DMATRIX identity = {{{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}}};
    m_s.world = m_s.view = m_s.proj = identity;
    m_stateDirty = true;
}

void Device9Proxy::RefreshShadowFromDevice()
{
    m_stateDirty = false;

    IDirect3DVertexShader9* vs = nullptr;
    if (SUCCEEDED(m_real->GetVertexShader(&vs))) m_s.vs = Weak(vs);
    IDirect3DPixelShader9* ps = nullptr;
    if (SUCCEEDED(m_real->GetPixelShader(&ps))) m_s.ps = Weak(ps);
    m_s.vsHash = ShaderRegistry::Get().HashOf(m_s.vs);
    m_s.psHash = ShaderRegistry::Get().HashOf(m_s.ps);

    for (UINT i = 0; i < 4; ++i) {
        IDirect3DVertexBuffer9* vb = nullptr;
        UINT off = 0, stride = 0;
        if (SUCCEEDED(m_real->GetStreamSource(i, &vb, &off, &stride))) {
            m_s.stream[i] = Weak(vb);
            m_s.offset[i] = off;
            m_s.stride[i] = stride;
        }
    }
    IDirect3DIndexBuffer9* ib = nullptr;
    if (SUCCEEDED(m_real->GetIndices(&ib))) m_s.indices = Weak(ib);
    IDirect3DVertexDeclaration9* decl = nullptr;
    if (SUCCEEDED(m_real->GetVertexDeclaration(&decl))) m_s.decl = Weak(decl);
    m_real->GetFVF(&m_s.fvf);
    for (DWORD i = 0; i < 8; ++i) {
        IDirect3DBaseTexture9* t = nullptr;
        if (SUCCEEDED(m_real->GetTexture(i, &t))) m_s.textures[i] = Weak(t);
    }
    IDirect3DSurface9* rt = nullptr;
    m_s.rt0 = SUCCEEDED(m_real->GetRenderTarget(0, &rt)) ? Weak(rt) : nullptr;
    IDirect3DSurface9* ds = nullptr;
    m_s.ds = SUCCEEDED(m_real->GetDepthStencilSurface(&ds)) ? Weak(ds) : nullptr;

    m_real->GetVertexShaderConstantF(0, m_s.vsConst, 256);
    m_real->GetPixelShaderConstantF(0, m_s.psConst, 224);
    m_real->GetTransform(D3DTS_WORLD, &m_s.world);
    m_real->GetTransform(D3DTS_VIEW, &m_s.view);
    m_real->GetTransform(D3DTS_PROJECTION, &m_s.proj);
    for (DWORD i = 0; i < 256; ++i) m_real->GetRenderState(static_cast<D3DRENDERSTATETYPE>(i), &m_s.renderStates[i]);
    m_real->GetViewport(&m_s.viewport);
}

DrawResult Device9Proxy::NotifyDraw(DrawKind kind, D3DPRIMITIVETYPE type, INT baseVertex, UINT minVertex,
                                    UINT numVertices, UINT start, UINT primCount)
{
    telemetry::Scope telemetryScope(telemetry::kDrawHook);
    ExtensionManager& em = ExtensionManager::Get();
    if (!em.HasExtensions()) return DrawResult::Continue;
    if (m_stateDirty) RefreshShadowFromDevice();

    DrawCallInfo d        = {};
    d.kind                = kind;
    d.primitiveType       = type;
    d.baseVertexIndex     = baseVertex;
    d.minVertexIndex      = minVertex;
    d.numVertices         = numVertices;
    d.startIndexOrVertex  = start;
    d.primitiveCount      = primCount;
    for (int i = 0; i < 4; ++i) {
        d.streamSource[i] = m_s.stream[i];
        d.streamOffset[i] = m_s.offset[i];
        d.streamStride[i] = m_s.stride[i];
    }
    d.indexBuffer         = m_s.indices;
    d.vertexDeclaration   = m_s.decl;
    d.fvf                 = m_s.fvf;
    d.vertexShader        = m_s.vs;
    d.pixelShader         = m_s.ps;
    d.vertexShaderHash    = m_s.vsHash;
    d.pixelShaderHash     = m_s.psHash;
    d.vsConstantsF        = m_s.vsConst;
    d.psConstantsF        = m_s.psConst;
    d.worldTransform      = &m_s.world;
    d.viewTransform       = &m_s.view;
    d.projectionTransform = &m_s.proj;
    d.renderTarget0       = m_s.rt0;
    d.depthStencil        = m_s.ds;
    for (int i = 0; i < 8; ++i) d.textures[i] = m_s.textures[i];
    d.frameIndex          = em.GetFrameIndex();
    d.renderStates        = m_s.renderStates;
    d.viewport            = m_s.viewport;
    return em.DispatchDraw(d);
}

HRESULT Device9Proxy::GetDirect3D(IDirect3D9** pp)
{
    if (!pp) return D3DERR_INVALIDCALL;
    m_d3d->AddRef();
    *pp = m_d3d;
    return D3D_OK;
}

HRESULT Device9Proxy::Reset(D3DPRESENT_PARAMETERS* pp)
{
    BDX_INFO("Reset(%ux%u, windowed %d)", pp ? pp->BackBufferWidth : 0, pp ? pp->BackBufferHeight : 0,
             pp ? pp->Windowed : 0);
    OnDeviceGoingAway();
    if (!pp) return D3DERR_INVALIDCALL;
    D3DPRESENT_PARAMETERS realPp;
    Display::Get().MakeRealParameters(*pp, m_focus, &realPp);
    const HRESULT hr = m_real->Reset(&realPp);
    if (SUCCEEDED(hr)) Display::Get().ApplyWindow();
    ResetShadow();
    if (SUCCEEDED(hr)) {
        m_pp = realPp;
        ExtensionManager::Get().DispatchDeviceReset(MakeDeviceInfo());
    } else {
        BDX_WARN("Reset failed: %s", HResultName(hr));
    }
    return hr;
}

HRESULT Device9Proxy::Present(const RECT* src, const RECT* dst, HWND wnd, const RGNDATA* dirty)
{
    LARGE_INTEGER t0, t1, t2, t3;
    QueryPerformanceCounter(&t0);
    FramePipeline::Get().OnPresent(m_real);
    Display::Get().Maintain();
    QueryPerformanceCounter(&t1);
    QueryPerformanceCounter(&t2);
    const HRESULT hr = Display::Get().Borderless() ? m_real->Present(nullptr, nullptr, nullptr, nullptr)
                                                   : m_real->Present(src, dst, wnd, dirty);
    QueryPerformanceCounter(&t3);
    telemetry::OnPresent(t0, t1, t2, t3, hr);
    return hr;
}

HRESULT Device9Proxy::BeginScene()
{
    telemetry::Scope telemetryScope(telemetry::kOther);
    const HRESULT hr = m_real->BeginScene();
    if (SUCCEEDED(hr)) ExtensionManager::Get().DispatchBeginScene();
    return hr;
}

HRESULT Device9Proxy::EndScene()
{
    telemetry::Scope telemetryScope(telemetry::kOther);
    ExtensionManager::Get().DispatchEndScene();
    return m_real->EndScene();
}

HRESULT Device9Proxy::SetTransform(D3DTRANSFORMSTATETYPE state, const D3DMATRIX* m)
{
    const HRESULT hr = m_real->SetTransform(state, m);
    if (SUCCEEDED(hr) && m && !m_inStateBlockRecord) {
        if (state == D3DTS_WORLD) m_s.world = *m;
        else if (state == D3DTS_VIEW) m_s.view = *m;
        else if (state == D3DTS_PROJECTION) m_s.proj = *m;
    }
    return hr;
}

HRESULT Device9Proxy::MultiplyTransform(D3DTRANSFORMSTATETYPE state, const D3DMATRIX* m)
{
    const HRESULT hr = m_real->MultiplyTransform(state, m);
    if (SUCCEEDED(hr) && !m_inStateBlockRecord) m_stateDirty = true;
    return hr;
}

HRESULT Device9Proxy::SetLight(DWORD index, const D3DLIGHT9* light)
{
    const HRESULT hr = m_real->SetLight(index, light);
    if (SUCCEEDED(hr) && light && !m_inStateBlockRecord) ExtensionManager::Get().DispatchSetLight(index, *light);
    return hr;
}

HRESULT Device9Proxy::LightEnable(DWORD index, BOOL enable)
{
    const HRESULT hr = m_real->LightEnable(index, enable);
    if (SUCCEEDED(hr) && !m_inStateBlockRecord) ExtensionManager::Get().DispatchLightEnable(index, enable);
    return hr;
}

HRESULT Device9Proxy::CreateStateBlock(D3DSTATEBLOCKTYPE type, IDirect3DStateBlock9** ppSB)
{
    if (!ppSB) return D3DERR_INVALIDCALL;
    IDirect3DStateBlock9* sb = nullptr;
    const HRESULT hr = m_real->CreateStateBlock(type, &sb);
    *ppSB = SUCCEEDED(hr) && sb ? new StateBlock9Proxy(sb, this) : nullptr;
    return hr;
}

HRESULT Device9Proxy::BeginStateBlock()
{
    const HRESULT hr = m_real->BeginStateBlock();
    if (SUCCEEDED(hr)) m_inStateBlockRecord = true;
    return hr;
}

HRESULT Device9Proxy::EndStateBlock(IDirect3DStateBlock9** ppSB)
{
    if (!ppSB) return D3DERR_INVALIDCALL;
    IDirect3DStateBlock9* sb = nullptr;
    const HRESULT hr = m_real->EndStateBlock(&sb);
    m_inStateBlockRecord = false;
    *ppSB = SUCCEEDED(hr) && sb ? new StateBlock9Proxy(sb, this) : nullptr;
    return hr;
}

HRESULT Device9Proxy::SetTexture(DWORD stage, IDirect3DBaseTexture9* tex)
{
    const HRESULT hr = m_real->SetTexture(stage, tex);
    if (SUCCEEDED(hr) && stage < 8 && !m_inStateBlockRecord) m_s.textures[stage] = tex;
    return hr;
}

HRESULT Device9Proxy::SetRenderTarget(DWORD index, IDirect3DSurface9* rt)
{
    telemetry::Scope telemetryScope(telemetry::kOther);
    const HRESULT hr = m_real->SetRenderTarget(index, rt);
    if (SUCCEEDED(hr) && index == 0) {
        m_s.rt0 = rt;
        m_real->GetViewport(&m_s.viewport);
    }
    return hr;
}

HRESULT Device9Proxy::SetDepthStencilSurface(IDirect3DSurface9* ds)
{
    telemetry::Scope telemetryScope(telemetry::kOther);
    const HRESULT hr = m_real->SetDepthStencilSurface(ds);
    if (SUCCEEDED(hr)) m_s.ds = ds;
    return hr;
}

HRESULT Device9Proxy::SetVertexDeclaration(IDirect3DVertexDeclaration9* decl)
{
    const HRESULT hr = m_real->SetVertexDeclaration(decl);
    if (SUCCEEDED(hr) && !m_inStateBlockRecord) {
        m_s.decl = decl;
        m_s.fvf  = 0;
    }
    return hr;
}

HRESULT Device9Proxy::SetFVF(DWORD fvf)
{
    const HRESULT hr = m_real->SetFVF(fvf);
    if (SUCCEEDED(hr) && !m_inStateBlockRecord) {
        m_s.fvf = fvf;
        if (fvf) m_stateDirty = true;
    }
    return hr;
}

HRESULT Device9Proxy::SetStreamSource(UINT n, IDirect3DVertexBuffer9* vb, UINT offset, UINT stride)
{
    const HRESULT hr = m_real->SetStreamSource(n, vb, offset, stride);
    if (SUCCEEDED(hr) && n < 4 && !m_inStateBlockRecord) {
        m_s.stream[n] = vb;
        m_s.offset[n] = offset;
        m_s.stride[n] = stride;
    }
    return hr;
}

HRESULT Device9Proxy::SetIndices(IDirect3DIndexBuffer9* ib)
{
    const HRESULT hr = m_real->SetIndices(ib);
    if (SUCCEEDED(hr) && !m_inStateBlockRecord) m_s.indices = ib;
    return hr;
}

HRESULT Device9Proxy::CreateVertexShader(const DWORD* fn, IDirect3DVertexShader9** pp)
{
    telemetry::Scope telemetryScope(telemetry::kCreate);
    uint64_t hash = 0;
    const DWORD* code = ShaderRegistry::Get().Resolve(ShaderStage::Vertex, fn, &hash);
    HRESULT hr = m_real->CreateVertexShader(code, pp);
    if (FAILED(hr) && code != fn) {
        BDX_ERROR("Replacement vertex shader %016llx rejected (%s) - using the original", hash, HResultName(hr));
        hr   = m_real->CreateVertexShader(fn, pp);
        code = fn;
    }
    if (SUCCEEDED(hr) && pp && *pp) {
        ShaderRegistry::Get().Register(*pp, hash);
        ShaderRegistry::Get().RegisterConstants(ShaderStage::Vertex, hash, code);
    }
    return hr;
}

HRESULT Device9Proxy::CreatePixelShader(const DWORD* fn, IDirect3DPixelShader9** pp)
{
    telemetry::Scope telemetryScope(telemetry::kCreate);
    uint64_t hash = 0;
    const DWORD* code = ShaderRegistry::Get().Resolve(ShaderStage::Pixel, fn, &hash);
    HRESULT hr = m_real->CreatePixelShader(code, pp);
    if (FAILED(hr) && code != fn) {
        BDX_ERROR("Replacement pixel shader %016llx rejected (%s) - using the original", hash, HResultName(hr));
        hr   = m_real->CreatePixelShader(fn, pp);
        code = fn;
    }
    if (SUCCEEDED(hr) && pp && *pp) {
        ShaderRegistry::Get().Register(*pp, hash);
        ShaderRegistry::Get().RegisterConstants(ShaderStage::Pixel, hash, code);
    }
    return hr;
}

HRESULT Device9Proxy::SetVertexShader(IDirect3DVertexShader9* s)
{
    const HRESULT hr = m_real->SetVertexShader(s);
    if (SUCCEEDED(hr) && !m_inStateBlockRecord) {
        m_s.vs     = s;
        m_s.vsHash = ShaderRegistry::Get().HashOf(s);
    }
    return hr;
}

HRESULT Device9Proxy::SetPixelShader(IDirect3DPixelShader9* s)
{
    const HRESULT hr = m_real->SetPixelShader(s);
    if (SUCCEEDED(hr) && !m_inStateBlockRecord) {
        m_s.ps     = s;
        m_s.psHash = ShaderRegistry::Get().HashOf(s);
    }
    return hr;
}

HRESULT Device9Proxy::SetVertexShaderConstantF(UINT start, const float* data, UINT count)
{
    const HRESULT hr = m_real->SetVertexShaderConstantF(start, data, count);
    if (SUCCEEDED(hr) && data && !m_inStateBlockRecord && start < 256) {
        const UINT n = std::min(count, 256u - start);
        memcpy(&m_s.vsConst[start * 4], data, n * 4 * sizeof(float));
    }
    return hr;
}

HRESULT Device9Proxy::SetPixelShaderConstantF(UINT start, const float* data, UINT count)
{
    const HRESULT hr = m_real->SetPixelShaderConstantF(start, data, count);
    if (SUCCEEDED(hr) && data && !m_inStateBlockRecord && start < 224) {
        const UINT n = std::min(count, 224u - start);
        memcpy(&m_s.psConst[start * 4], data, n * 4 * sizeof(float));
    }
    return hr;
}

HRESULT Device9Proxy::DrawPrimitive(D3DPRIMITIVETYPE t, UINT startVertex, UINT primCount)
{
    telemetry::Scope telemetryScope(telemetry::kDrawTotal);
    ++telemetry::g_draws;
    if (NotifyDraw(DrawKind::Primitive, t, 0, 0, 0, startVertex, primCount) == DrawResult::Skip) return D3D_OK;
    return m_real->DrawPrimitive(t, startVertex, primCount);
}

HRESULT Device9Proxy::DrawIndexedPrimitive(D3DPRIMITIVETYPE t, INT baseVertex, UINT minVertex, UINT numVertices,
                                           UINT startIndex, UINT primCount)
{
    telemetry::Scope telemetryScope(telemetry::kDrawTotal);
    ++telemetry::g_draws;
    if (NotifyDraw(DrawKind::IndexedPrimitive, t, baseVertex, minVertex, numVertices, startIndex, primCount) ==
        DrawResult::Skip)
        return D3D_OK;
    return m_real->DrawIndexedPrimitive(t, baseVertex, minVertex, numVertices, startIndex, primCount);
}

HRESULT Device9Proxy::DrawPrimitiveUP(D3DPRIMITIVETYPE t, UINT primCount, const void* data, UINT stride)
{
    telemetry::Scope telemetryScope(telemetry::kDrawTotal);
    ++telemetry::g_draws;
    if (NotifyDraw(DrawKind::PrimitiveUP, t, 0, 0, 0, 0, primCount) == DrawResult::Skip) return D3D_OK;
    const HRESULT hr = m_real->DrawPrimitiveUP(t, primCount, data, stride);
    m_s.stream[0] = nullptr;
    m_s.offset[0] = m_s.stride[0] = 0;
    return hr;
}

HRESULT Device9Proxy::DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE t, UINT minVertex, UINT numVertices, UINT primCount,
                                             const void* idx, D3DFORMAT idxFmt, const void* data, UINT stride)
{
    telemetry::Scope telemetryScope(telemetry::kDrawTotal);
    ++telemetry::g_draws;
    if (NotifyDraw(DrawKind::IndexedPrimitiveUP, t, 0, minVertex, numVertices, 0, primCount) == DrawResult::Skip)
        return D3D_OK;
    const HRESULT hr = m_real->DrawIndexedPrimitiveUP(t, minVertex, numVertices, primCount, idx, idxFmt, data, stride);
    m_s.stream[0] = nullptr;
    m_s.offset[0] = m_s.stride[0] = 0;
    m_s.indices   = nullptr;
    return hr;
}

HRESULT Device9Proxy::TestCooperativeLevel() { return m_real->TestCooperativeLevel(); }
UINT    Device9Proxy::GetAvailableTextureMem() { return m_real->GetAvailableTextureMem(); }
HRESULT Device9Proxy::EvictManagedResources() { return m_real->EvictManagedResources(); }
HRESULT Device9Proxy::GetDeviceCaps(D3DCAPS9* p) { return m_real->GetDeviceCaps(p); }
HRESULT Device9Proxy::GetDisplayMode(UINT sc, D3DDISPLAYMODE* p) { return m_real->GetDisplayMode(sc, p); }
HRESULT Device9Proxy::GetCreationParameters(D3DDEVICE_CREATION_PARAMETERS* p) { return m_real->GetCreationParameters(p); }
HRESULT Device9Proxy::SetCursorProperties(UINT x, UINT y, IDirect3DSurface9* s) { return m_real->SetCursorProperties(x, y, s); }
void    Device9Proxy::SetCursorPosition(int x, int y, DWORD f) { m_real->SetCursorPosition(x, y, f); }
BOOL    Device9Proxy::ShowCursor(BOOL b) { return m_real->ShowCursor(b); }
HRESULT Device9Proxy::CreateAdditionalSwapChain(D3DPRESENT_PARAMETERS* pp, IDirect3DSwapChain9** sc) { return m_real->CreateAdditionalSwapChain(pp, sc); }
HRESULT Device9Proxy::GetSwapChain(UINT i, IDirect3DSwapChain9** sc) { return m_real->GetSwapChain(i, sc); }
UINT    Device9Proxy::GetNumberOfSwapChains() { return m_real->GetNumberOfSwapChains(); }
HRESULT Device9Proxy::GetBackBuffer(UINT sc, UINT bb, D3DBACKBUFFER_TYPE t, IDirect3DSurface9** pp) { return m_real->GetBackBuffer(sc, bb, t, pp); }
HRESULT Device9Proxy::GetRasterStatus(UINT sc, D3DRASTER_STATUS* p) { return m_real->GetRasterStatus(sc, p); }
HRESULT Device9Proxy::SetDialogBoxMode(BOOL b) { return m_real->SetDialogBoxMode(b); }
void    Device9Proxy::SetGammaRamp(UINT sc, DWORD f, const D3DGAMMARAMP* r) { m_real->SetGammaRamp(sc, f, r); }
void    Device9Proxy::GetGammaRamp(UINT sc, D3DGAMMARAMP* r) { m_real->GetGammaRamp(sc, r); }
HRESULT Device9Proxy::CreateTexture(UINT w, UINT h, UINT l, DWORD u, D3DFORMAT f, D3DPOOL p, IDirect3DTexture9** pp, HANDLE* sh) { telemetry::Scope telemetryScope(telemetry::kCreate); return m_real->CreateTexture(w, h, l, u, f, p, pp, sh); }
HRESULT Device9Proxy::CreateVolumeTexture(UINT w, UINT h, UINT d, UINT l, DWORD u, D3DFORMAT f, D3DPOOL p, IDirect3DVolumeTexture9** pp, HANDLE* sh) { telemetry::Scope telemetryScope(telemetry::kCreate); return m_real->CreateVolumeTexture(w, h, d, l, u, f, p, pp, sh); }
HRESULT Device9Proxy::CreateCubeTexture(UINT e, UINT l, DWORD u, D3DFORMAT f, D3DPOOL p, IDirect3DCubeTexture9** pp, HANDLE* sh) { telemetry::Scope telemetryScope(telemetry::kCreate); return m_real->CreateCubeTexture(e, l, u, f, p, pp, sh); }
HRESULT Device9Proxy::CreateVertexBuffer(UINT len, DWORD u, DWORD fvf, D3DPOOL p, IDirect3DVertexBuffer9** pp, HANDLE* sh) { telemetry::Scope telemetryScope(telemetry::kCreate); return m_real->CreateVertexBuffer(len, u, fvf, p, pp, sh); }
HRESULT Device9Proxy::CreateIndexBuffer(UINT len, DWORD u, D3DFORMAT f, D3DPOOL p, IDirect3DIndexBuffer9** pp, HANDLE* sh) { telemetry::Scope telemetryScope(telemetry::kCreate); return m_real->CreateIndexBuffer(len, u, f, p, pp, sh); }
HRESULT Device9Proxy::CreateRenderTarget(UINT w, UINT h, D3DFORMAT f, D3DMULTISAMPLE_TYPE ms, DWORD q, BOOL lock, IDirect3DSurface9** pp, HANDLE* sh) { telemetry::Scope telemetryScope(telemetry::kCreate); return m_real->CreateRenderTarget(w, h, f, ms, q, lock, pp, sh); }
HRESULT Device9Proxy::CreateDepthStencilSurface(UINT w, UINT h, D3DFORMAT f, D3DMULTISAMPLE_TYPE ms, DWORD q, BOOL disc, IDirect3DSurface9** pp, HANDLE* sh) { telemetry::Scope telemetryScope(telemetry::kCreate); return m_real->CreateDepthStencilSurface(w, h, f, ms, q, disc, pp, sh); }
HRESULT Device9Proxy::UpdateSurface(IDirect3DSurface9* s, const RECT* r, IDirect3DSurface9* d, const POINT* pt) { telemetry::Scope telemetryScope(telemetry::kOther); return m_real->UpdateSurface(s, r, d, pt); }
HRESULT Device9Proxy::UpdateTexture(IDirect3DBaseTexture9* s, IDirect3DBaseTexture9* d) { telemetry::Scope telemetryScope(telemetry::kOther); return m_real->UpdateTexture(s, d); }
HRESULT Device9Proxy::GetRenderTargetData(IDirect3DSurface9* rt, IDirect3DSurface9* d)
{
    telemetry::Scope telemetryScope(telemetry::kOther);
    ExtensionManager::Get().DispatchCopySurface(rt, d);
    return m_real->GetRenderTargetData(rt, d);
}
HRESULT Device9Proxy::GetFrontBufferData(UINT sc, IDirect3DSurface9* d) { return m_real->GetFrontBufferData(sc, d); }
HRESULT Device9Proxy::StretchRect(IDirect3DSurface9* s, const RECT* sr, IDirect3DSurface9* d, const RECT* dr, D3DTEXTUREFILTERTYPE f)
{
    telemetry::Scope telemetryScope(telemetry::kOther);
    ExtensionManager::Get().DispatchCopySurface(s, d);
    return m_real->StretchRect(s, sr, d, dr, f);
}
HRESULT Device9Proxy::ColorFill(IDirect3DSurface9* s, const RECT* r, D3DCOLOR c) { telemetry::Scope telemetryScope(telemetry::kOther); return m_real->ColorFill(s, r, c); }
HRESULT Device9Proxy::CreateOffscreenPlainSurface(UINT w, UINT h, D3DFORMAT f, D3DPOOL p, IDirect3DSurface9** pp, HANDLE* sh) { telemetry::Scope telemetryScope(telemetry::kCreate); return m_real->CreateOffscreenPlainSurface(w, h, f, p, pp, sh); }
HRESULT Device9Proxy::GetRenderTarget(DWORD i, IDirect3DSurface9** pp) { return m_real->GetRenderTarget(i, pp); }
HRESULT Device9Proxy::GetDepthStencilSurface(IDirect3DSurface9** pp) { return m_real->GetDepthStencilSurface(pp); }
HRESULT Device9Proxy::Clear(DWORD c, const D3DRECT* r, DWORD f, D3DCOLOR col, float z, DWORD s)
{
    telemetry::Scope telemetryScope(telemetry::kOther);
    ExtensionManager::Get().DispatchClear(f, m_s.rt0, m_s.ds);
    return m_real->Clear(c, r, f, col, z, s);
}
HRESULT Device9Proxy::GetTransform(D3DTRANSFORMSTATETYPE s, D3DMATRIX* m) { return m_real->GetTransform(s, m); }
HRESULT Device9Proxy::SetViewport(const D3DVIEWPORT9* v)
{
    const HRESULT hr = m_real->SetViewport(v);
    if (SUCCEEDED(hr) && v && !m_inStateBlockRecord) m_s.viewport = *v;
    return hr;
}
HRESULT Device9Proxy::GetViewport(D3DVIEWPORT9* v) { return m_real->GetViewport(v); }
HRESULT Device9Proxy::SetMaterial(const D3DMATERIAL9* m) { return m_real->SetMaterial(m); }
HRESULT Device9Proxy::GetMaterial(D3DMATERIAL9* m) { return m_real->GetMaterial(m); }
HRESULT Device9Proxy::GetLight(DWORD i, D3DLIGHT9* l) { return m_real->GetLight(i, l); }
HRESULT Device9Proxy::GetLightEnable(DWORD i, BOOL* e) { return m_real->GetLightEnable(i, e); }
HRESULT Device9Proxy::SetClipPlane(DWORD i, const float* p) { return m_real->SetClipPlane(i, p); }
HRESULT Device9Proxy::GetClipPlane(DWORD i, float* p) { return m_real->GetClipPlane(i, p); }
HRESULT Device9Proxy::SetRenderState(D3DRENDERSTATETYPE s, DWORD v)
{
    const HRESULT hr = m_real->SetRenderState(s, v);
    if (SUCCEEDED(hr) && !m_inStateBlockRecord && static_cast<UINT>(s) < 256) m_s.renderStates[s] = v;
    return hr;
}
HRESULT Device9Proxy::GetRenderState(D3DRENDERSTATETYPE s, DWORD* v) { return m_real->GetRenderState(s, v); }
HRESULT Device9Proxy::SetClipStatus(const D3DCLIPSTATUS9* c) { return m_real->SetClipStatus(c); }
HRESULT Device9Proxy::GetClipStatus(D3DCLIPSTATUS9* c) { return m_real->GetClipStatus(c); }
HRESULT Device9Proxy::GetTexture(DWORD s, IDirect3DBaseTexture9** pp) { return m_real->GetTexture(s, pp); }
HRESULT Device9Proxy::GetTextureStageState(DWORD s, D3DTEXTURESTAGESTATETYPE t, DWORD* v) { return m_real->GetTextureStageState(s, t, v); }
HRESULT Device9Proxy::SetTextureStageState(DWORD s, D3DTEXTURESTAGESTATETYPE t, DWORD v) { return m_real->SetTextureStageState(s, t, v); }
HRESULT Device9Proxy::GetSamplerState(DWORD s, D3DSAMPLERSTATETYPE t, DWORD* v) { return m_real->GetSamplerState(s, t, v); }
HRESULT Device9Proxy::SetSamplerState(DWORD s, D3DSAMPLERSTATETYPE t, DWORD v) { return m_real->SetSamplerState(s, t, v); }
HRESULT Device9Proxy::ValidateDevice(DWORD* n) { return m_real->ValidateDevice(n); }
HRESULT Device9Proxy::SetPaletteEntries(UINT n, const PALETTEENTRY* e) { return m_real->SetPaletteEntries(n, e); }
HRESULT Device9Proxy::GetPaletteEntries(UINT n, PALETTEENTRY* e) { return m_real->GetPaletteEntries(n, e); }
HRESULT Device9Proxy::SetCurrentTexturePalette(UINT n) { return m_real->SetCurrentTexturePalette(n); }
HRESULT Device9Proxy::GetCurrentTexturePalette(UINT* n) { return m_real->GetCurrentTexturePalette(n); }
HRESULT Device9Proxy::SetScissorRect(const RECT* r) { return m_real->SetScissorRect(r); }
HRESULT Device9Proxy::GetScissorRect(RECT* r) { return m_real->GetScissorRect(r); }
HRESULT Device9Proxy::SetSoftwareVertexProcessing(BOOL b) { return m_real->SetSoftwareVertexProcessing(b); }
BOOL    Device9Proxy::GetSoftwareVertexProcessing() { return m_real->GetSoftwareVertexProcessing(); }
HRESULT Device9Proxy::SetNPatchMode(float n) { return m_real->SetNPatchMode(n); }
float   Device9Proxy::GetNPatchMode() { return m_real->GetNPatchMode(); }
HRESULT Device9Proxy::ProcessVertices(UINT s, UINT d, UINT c, IDirect3DVertexBuffer9* b, IDirect3DVertexDeclaration9* decl, DWORD f) { return m_real->ProcessVertices(s, d, c, b, decl, f); }
HRESULT Device9Proxy::CreateVertexDeclaration(const D3DVERTEXELEMENT9* e, IDirect3DVertexDeclaration9** pp) { telemetry::Scope telemetryScope(telemetry::kCreate); return m_real->CreateVertexDeclaration(e, pp); }
HRESULT Device9Proxy::GetVertexDeclaration(IDirect3DVertexDeclaration9** pp) { return m_real->GetVertexDeclaration(pp); }
HRESULT Device9Proxy::GetFVF(DWORD* f) { return m_real->GetFVF(f); }
HRESULT Device9Proxy::GetVertexShader(IDirect3DVertexShader9** pp) { return m_real->GetVertexShader(pp); }
HRESULT Device9Proxy::GetVertexShaderConstantF(UINT s, float* d, UINT c) { return m_real->GetVertexShaderConstantF(s, d, c); }
HRESULT Device9Proxy::SetVertexShaderConstantI(UINT s, const int* d, UINT c) { return m_real->SetVertexShaderConstantI(s, d, c); }
HRESULT Device9Proxy::GetVertexShaderConstantI(UINT s, int* d, UINT c) { return m_real->GetVertexShaderConstantI(s, d, c); }
HRESULT Device9Proxy::SetVertexShaderConstantB(UINT s, const BOOL* d, UINT c) { return m_real->SetVertexShaderConstantB(s, d, c); }
HRESULT Device9Proxy::GetVertexShaderConstantB(UINT s, BOOL* d, UINT c) { return m_real->GetVertexShaderConstantB(s, d, c); }
HRESULT Device9Proxy::GetStreamSource(UINT n, IDirect3DVertexBuffer9** pp, UINT* o, UINT* s) { return m_real->GetStreamSource(n, pp, o, s); }
HRESULT Device9Proxy::SetStreamSourceFreq(UINT n, UINT s) { return m_real->SetStreamSourceFreq(n, s); }
HRESULT Device9Proxy::GetStreamSourceFreq(UINT n, UINT* s) { return m_real->GetStreamSourceFreq(n, s); }
HRESULT Device9Proxy::GetIndices(IDirect3DIndexBuffer9** pp) { return m_real->GetIndices(pp); }
HRESULT Device9Proxy::GetPixelShader(IDirect3DPixelShader9** pp) { return m_real->GetPixelShader(pp); }
HRESULT Device9Proxy::GetPixelShaderConstantF(UINT s, float* d, UINT c) { return m_real->GetPixelShaderConstantF(s, d, c); }
HRESULT Device9Proxy::SetPixelShaderConstantI(UINT s, const int* d, UINT c) { return m_real->SetPixelShaderConstantI(s, d, c); }
HRESULT Device9Proxy::GetPixelShaderConstantI(UINT s, int* d, UINT c) { return m_real->GetPixelShaderConstantI(s, d, c); }
HRESULT Device9Proxy::SetPixelShaderConstantB(UINT s, const BOOL* d, UINT c) { return m_real->SetPixelShaderConstantB(s, d, c); }
HRESULT Device9Proxy::GetPixelShaderConstantB(UINT s, BOOL* d, UINT c) { return m_real->GetPixelShaderConstantB(s, d, c); }
HRESULT Device9Proxy::DrawRectPatch(UINT h, const float* s, const D3DRECTPATCH_INFO* i) { return m_real->DrawRectPatch(h, s, i); }
HRESULT Device9Proxy::DrawTriPatch(UINT h, const float* s, const D3DTRIPATCH_INFO* i) { return m_real->DrawTriPatch(h, s, i); }
HRESULT Device9Proxy::DeletePatch(UINT h) { return m_real->DeletePatch(h); }
HRESULT Device9Proxy::CreateQuery(D3DQUERYTYPE t, IDirect3DQuery9** pp) { return m_real->CreateQuery(t, pp); }

}
