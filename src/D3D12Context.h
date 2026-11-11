#pragma once
#include "Common.h"
#include "Config.h"

#include <deque>

namespace bdx12 {

class D3D12Context {
public:
    static D3D12Context& Get();

    bool Initialize(const LUID& luid, const Config& cfg);
    void Shutdown();
    bool IsReady() const { return m_device != nullptr; }

    ID3D12Device*        Device() const { return m_device.Get(); }
    ID3D12CommandQueue*  Queue() const { return m_queue.Get(); }
    const D3D12Caps&     Caps() const { return m_caps; }

    void SetDevice9On12(IDirect3DDevice9On12* dev);
    IDirect3DDevice9On12* Device9On12() const { return m_device9On12.Get(); }

    HRESULT Unwrap(IDirect3DResource9* res9, ID3D12Resource** out);
    HRESULT Return(IDirect3DResource9* res9);

    ID3D12GraphicsCommandList* BeginCommandList();
    uint64_t                   Execute(ID3D12GraphicsCommandList* list);
    uint64_t                   Signal();
    uint64_t                   CompletedValue() const;
    void                       Wait(uint64_t value);
    void                       WaitIdle();
    void                       DeferredRelease(IUnknown* obj);
    void                       CollectGarbage();

    static bool PickAdapterLuid(const Config& cfg, LUID* outLuid, std::wstring* outName);

private:
    struct PooledList {
        ComPtr<ID3D12CommandAllocator>    allocator;
        ComPtr<ID3D12GraphicsCommandList> list;
        uint64_t                          fenceValue = 0;
        bool                              open = false;
    };
    struct Deferred {
        ComPtr<IUnknown> object;
        uint64_t         fenceValue;
    };

    void QueryCaps(IDXGIAdapter1* adapter);

    ComPtr<ID3D12Device>          m_device;
    ComPtr<ID3D12CommandQueue>    m_queue;
    ComPtr<ID3D12Fence>           m_fence;
    HANDLE                        m_fenceEvent = nullptr;
    uint64_t                      m_nextFenceValue = 1;
    uint64_t                      m_lastSignaled = 0;
    ComPtr<IDirect3DDevice9On12>  m_device9On12;
    D3D12Caps                     m_caps = {};

    std::recursive_mutex          m_mutex;
    std::vector<PooledList>       m_lists;
    std::deque<Deferred>          m_deferred;
};

}
