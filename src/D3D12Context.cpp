#include "D3D12Context.h"

#include <d3d12sdklayers.h>

namespace bdx12 {

D3D12Context& D3D12Context::Get()
{
    static D3D12Context s;
    return s;
}

bool D3D12Context::PickAdapterLuid(const Config& cfg, LUID* outLuid, std::wstring* outName)
{
    ComPtr<IDXGIFactory6> factory6;
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        BDX_ERROR("CreateDXGIFactory1 failed");
        return false;
    }
    factory.As(&factory6);

    ComPtr<IDXGIAdapter1> chosen;
    auto acceptable = [](IDXGIAdapter1* a) {
        DXGI_ADAPTER_DESC1 d;
        a->GetDesc1(&d);
        if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) return false;
        return SUCCEEDED(D3D12CreateDevice(a, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), nullptr));
    };

    if (cfg.adapterPreference == AdapterPreference::Index) {
        ComPtr<IDXGIAdapter1> a;
        if (SUCCEEDED(factory->EnumAdapters1(static_cast<UINT>(cfg.adapterIndex), &a)) && acceptable(a.Get()))
            chosen = a;
        else
            BDX_WARN("Adapter index %d is not usable, falling back to default", cfg.adapterIndex);
    }

    if (!chosen && factory6 && cfg.adapterPreference != AdapterPreference::Default) {
        const DXGI_GPU_PREFERENCE pref = cfg.adapterPreference == AdapterPreference::MinimumPower
                                             ? DXGI_GPU_PREFERENCE_MINIMUM_POWER
                                             : DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE;
        for (UINT i = 0;; ++i) {
            ComPtr<IDXGIAdapter1> a;
            if (FAILED(factory6->EnumAdapterByGpuPreference(i, pref, IID_PPV_ARGS(&a)))) break;
            if (acceptable(a.Get())) { chosen = a; break; }
        }
    }

    if (!chosen) {
        for (UINT i = 0;; ++i) {
            ComPtr<IDXGIAdapter1> a;
            if (FAILED(factory->EnumAdapters1(i, &a))) break;
            if (acceptable(a.Get())) { chosen = a; break; }
        }
    }

    if (!chosen) {
        BDX_ERROR("No D3D12 capable hardware adapter found");
        return false;
    }

    DXGI_ADAPTER_DESC1 desc;
    chosen->GetDesc1(&desc);
    *outLuid = desc.AdapterLuid;
    if (outName) *outName = desc.Description;
    return true;
}

bool D3D12Context::Initialize(const LUID& luid, const Config& cfg)
{
    if (m_device) return true;

    if (cfg.debugLayer) {
        ComPtr<ID3D12Debug> debug;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
            debug->EnableDebugLayer();
            BDX_INFO("D3D12 debug layer enabled");
            if (cfg.gpuBasedValidation) {
                ComPtr<ID3D12Debug1> debug1;
                if (SUCCEEDED(debug.As(&debug1))) {
                    debug1->SetEnableGPUBasedValidation(TRUE);
                    BDX_INFO("D3D12 GPU based validation enabled");
                }
            }
        } else {
            BDX_WARN("D3D12 debug layer requested but not installed (Graphics Tools optional feature)");
        }
    }

    ComPtr<IDXGIFactory4> factory;
    if (FAILED(CreateDXGIFactory2(cfg.debugLayer ? DXGI_CREATE_FACTORY_DEBUG : 0, IID_PPV_ARGS(&factory))) &&
        FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) {
        BDX_ERROR("CreateDXGIFactory2 failed");
        return false;
    }

    ComPtr<IDXGIAdapter1> adapter;
    if (FAILED(factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter)))) {
        BDX_ERROR("EnumAdapterByLuid failed");
        return false;
    }

    HRESULT hr = E_FAIL;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_12_2, D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0,
                                        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    for (D3D_FEATURE_LEVEL fl : levels) {
        hr = D3D12CreateDevice(adapter.Get(), fl, IID_PPV_ARGS(&m_device));
        if (SUCCEEDED(hr)) break;
    }
    if (FAILED(hr)) {
        BDX_ERROR("D3D12CreateDevice failed: %s", HResultName(hr));
        return false;
    }
    m_device->SetName(L"BullyDX12 Device");

    if (cfg.debugLayer) {
        ComPtr<ID3D12InfoQueue> iq;
        if (SUCCEEDED(m_device.As(&iq))) {
            iq->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, TRUE);
            iq->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, FALSE);
        }
    }

    D3D12_COMMAND_QUEUE_DESC qd = {};
    qd.Type  = D3D12_COMMAND_LIST_TYPE_DIRECT;
    qd.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    hr = m_device->CreateCommandQueue(&qd, IID_PPV_ARGS(&m_queue));
    if (FAILED(hr)) {
        BDX_ERROR("CreateCommandQueue failed: %s", HResultName(hr));
        m_device.Reset();
        return false;
    }
    m_queue->SetName(L"BullyDX12 Direct Queue (shared with D3D9On12)");

    hr = m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence));
    if (FAILED(hr)) {
        BDX_ERROR("CreateFence failed: %s", HResultName(hr));
        m_queue.Reset();
        m_device.Reset();
        return false;
    }
    m_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    QueryCaps(adapter.Get());

    static const char* kFL[] = {"11_0", "11_1", "12_0", "12_1", "12_2"};
    const int flIdx = m_caps.featureLevel == D3D_FEATURE_LEVEL_12_2   ? 4
                      : m_caps.featureLevel == D3D_FEATURE_LEVEL_12_1 ? 3
                      : m_caps.featureLevel == D3D_FEATURE_LEVEL_12_0 ? 2
                      : m_caps.featureLevel == D3D_FEATURE_LEVEL_11_1 ? 1
                                                                       : 0;
    BDX_INFO("D3D12 device: %s | FL %s | SM %d.%d | DXR tier %d.%d | Mesh shaders %s | VRS tier %d | VRAM %llu MB",
             Narrow(m_caps.adapterName).c_str(), kFL[flIdx], (m_caps.highestShaderModel >> 4) & 0xF,
             m_caps.highestShaderModel & 0xF, m_caps.raytracingTier / 10, m_caps.raytracingTier % 10,
             m_caps.meshShaderTier != D3D12_MESH_SHADER_TIER_NOT_SUPPORTED ? "yes" : "no",
             static_cast<int>(m_caps.vrsTier), m_caps.dedicatedVideoMemory / (1024ull * 1024ull));
    return true;
}

void D3D12Context::QueryCaps(IDXGIAdapter1* adapter)
{
    m_caps = {};

    DXGI_ADAPTER_DESC1 ad;
    adapter->GetDesc1(&ad);
    m_caps.adapterLuid          = ad.AdapterLuid;
    m_caps.dedicatedVideoMemory = ad.DedicatedVideoMemory;
    wcsncpy_s(m_caps.adapterName, ad.Description, _TRUNCATE);

    const D3D_FEATURE_LEVEL req[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_12_0,
                                     D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_2};
    D3D12_FEATURE_DATA_FEATURE_LEVELS fl = {};
    fl.NumFeatureLevels        = static_cast<UINT>(std::size(req));
    fl.pFeatureLevelsRequested = req;
    m_caps.featureLevel = SUCCEEDED(m_device->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &fl, sizeof(fl)))
                              ? fl.MaxSupportedFeatureLevel
                              : D3D_FEATURE_LEVEL_11_0;

    const D3D_SHADER_MODEL models[] = {D3D_SHADER_MODEL_6_8, D3D_SHADER_MODEL_6_7, D3D_SHADER_MODEL_6_6,
                                       D3D_SHADER_MODEL_6_5, D3D_SHADER_MODEL_6_4, D3D_SHADER_MODEL_6_3,
                                       D3D_SHADER_MODEL_6_2, D3D_SHADER_MODEL_6_1, D3D_SHADER_MODEL_6_0};
    m_caps.highestShaderModel = D3D_SHADER_MODEL_5_1;
    for (D3D_SHADER_MODEL sm : models) {
        D3D12_FEATURE_DATA_SHADER_MODEL d = {sm};
        if (SUCCEEDED(m_device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &d, sizeof(d)))) {
            m_caps.highestShaderModel = d.HighestShaderModel;
            break;
        }
    }

    D3D12_FEATURE_DATA_D3D12_OPTIONS o = {};
    if (SUCCEEDED(m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &o, sizeof(o))))
        m_caps.resourceBindingTier = o.ResourceBindingTier;

    D3D12_FEATURE_DATA_D3D12_OPTIONS5 o5 = {};
    m_caps.raytracingTier = SUCCEEDED(m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &o5, sizeof(o5)))
                                ? o5.RaytracingTier
                                : D3D12_RAYTRACING_TIER_NOT_SUPPORTED;

    D3D12_FEATURE_DATA_D3D12_OPTIONS6 o6 = {};
    if (SUCCEEDED(m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS6, &o6, sizeof(o6))))
        m_caps.vrsTier = o6.VariableShadingRateTier;

    D3D12_FEATURE_DATA_D3D12_OPTIONS7 o7 = {};
    if (SUCCEEDED(m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &o7, sizeof(o7))))
        m_caps.meshShaderTier = o7.MeshShaderTier;

    D3D12_FEATURE_DATA_D3D12_OPTIONS12 o12 = {};
    if (SUCCEEDED(m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS12, &o12, sizeof(o12))))
        m_caps.enhancedBarriers = o12.EnhancedBarriersSupported;
}

void D3D12Context::SetDevice9On12(IDirect3DDevice9On12* dev)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    m_device9On12 = dev;
}

HRESULT D3D12Context::Unwrap(IDirect3DResource9* res9, ID3D12Resource** out)
{
    if (!out) return E_POINTER;
    *out = nullptr;
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_device9On12 || !res9) return E_FAIL;
    const HRESULT hr = m_device9On12->UnwrapUnderlyingResource(res9, m_queue.Get(), IID_PPV_ARGS(out));
    if (FAILED(hr)) BDX_WARN("UnwrapUnderlyingResource failed: %s", HResultName(hr));
    return hr;
}

HRESULT D3D12Context::Return(IDirect3DResource9* res9)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_device9On12 || !res9) return E_FAIL;
    UINT64       value = Signal();
    ID3D12Fence* fence = m_fence.Get();
    const HRESULT hr   = m_device9On12->ReturnUnderlyingResource(res9, 1, &value, &fence);
    if (FAILED(hr)) BDX_WARN("ReturnUnderlyingResource failed: %s", HResultName(hr));
    return hr;
}

ID3D12GraphicsCommandList* D3D12Context::BeginCommandList()
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_device) return nullptr;

    const uint64_t completed = m_fence->GetCompletedValue();
    PooledList*    slot      = nullptr;
    for (auto& l : m_lists) {
        if (!l.open && l.fenceValue <= completed) { slot = &l; break; }
    }

    if (!slot) {
        PooledList nl;
        if (FAILED(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&nl.allocator))))
            return nullptr;
        if (FAILED(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, nl.allocator.Get(), nullptr,
                                               IID_PPV_ARGS(&nl.list))))
            return nullptr;
        nl.list->SetName(L"BullyDX12 Command List");
        nl.open = true;
        m_lists.push_back(std::move(nl));
        return m_lists.back().list.Get();
    }

    slot->allocator->Reset();
    slot->list->Reset(slot->allocator.Get(), nullptr);
    slot->open = true;
    return slot->list.Get();
}

uint64_t D3D12Context::Execute(ID3D12GraphicsCommandList* list)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    for (auto& l : m_lists) {
        if (l.list.Get() != list) continue;
        if (!l.open) break;
        const HRESULT hr = list->Close();
        l.open = false;
        if (FAILED(hr)) {
            BDX_ERROR("ID3D12GraphicsCommandList::Close failed: %s (invalid commands recorded?)", HResultName(hr));
            m_lists.erase(m_lists.begin() + (&l - m_lists.data()));
            return 0;
        }
        ID3D12CommandList* lists[] = {list};
        m_queue->ExecuteCommandLists(1, lists);
        l.fenceValue = Signal();
        return l.fenceValue;
    }
    BDX_ERROR("ExecuteCommandList called with a list that was not obtained from BeginCommandList");
    return m_lastSignaled;
}

uint64_t D3D12Context::Signal()
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    const uint64_t v = m_nextFenceValue++;
    m_queue->Signal(m_fence.Get(), v);
    m_lastSignaled = v;
    return v;
}

uint64_t D3D12Context::CompletedValue() const { return m_fence ? m_fence->GetCompletedValue() : 0; }

void D3D12Context::Wait(uint64_t value)
{
    if (!m_fence || m_fence->GetCompletedValue() >= value) return;
    m_fence->SetEventOnCompletion(value, m_fenceEvent);
    WaitForSingleObject(m_fenceEvent, 5000);
}

void D3D12Context::WaitIdle()
{
    if (!m_queue) return;
    Wait(Signal());
}

void D3D12Context::DeferredRelease(IUnknown* obj)
{
    if (!obj) return;
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    Deferred d;
    d.object     = obj;
    d.fenceValue = m_lastSignaled + 1;
    obj->Release();
    m_deferred.push_back(std::move(d));
}

void D3D12Context::CollectGarbage()
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_fence) return;
    if (!m_deferred.empty() && m_deferred.back().fenceValue > m_lastSignaled) Signal();
    const uint64_t done = m_fence->GetCompletedValue();
    while (!m_deferred.empty() && m_deferred.front().fenceValue <= done) m_deferred.pop_front();
}

void D3D12Context::Shutdown()
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (m_queue && m_fence) WaitIdle();
    m_deferred.clear();
    m_lists.clear();
    m_device9On12.Reset();
    m_fence.Reset();
    m_queue.Reset();
    if (m_fenceEvent) {
        CloseHandle(m_fenceEvent);
        m_fenceEvent = nullptr;
    }
    m_device.Reset();
}

}
