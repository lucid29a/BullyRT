// BullyDX12 RaytracingFoundation extension

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "BullyDX12/BullyDX12.h"

using Microsoft::WRL::ComPtr;
using namespace bdx12;

namespace {

struct MeshKey {
    IDirect3DVertexBuffer9* vb;
    IDirect3DIndexBuffer9*  ib;
    UINT  vbOffset, stride, posOffset, posType;
    INT   baseVertex;
    UINT  minVertex, numVertices, startIndex, primCount;
    bool operator==(const MeshKey& o) const { return memcmp(this, &o, sizeof(MeshKey)) == 0; }
};
struct MeshKeyHash {
    size_t operator()(const MeshKey& k) const
    {
        const auto* p = reinterpret_cast<const uint8_t*>(&k);
        uint64_t h = 0xcbf29ce484222325ull;
        for (size_t i = 0; i < sizeof(MeshKey); ++i) { h ^= p[i]; h *= 0x100000001b3ull; }
        return static_cast<size_t>(h ^ (h >> 32));
    }
};

struct Blas {
    ComPtr<ID3D12Resource> as;
    uint64_t               lastUsedFrame = 0;
    bool                   built = false;
};

struct PendingBuild {
    MeshKey                key;
    ComPtr<ID3D12Resource> upload;
    UINT                   vertexCount;
    UINT                   indexCount;
};

enum SkipReason { kSkipNotIndexedTris, kSkipNoPosition, kSkipPosFormat, kSkipPool, kSkipLockFailed, kSkipBudget, kSkipSmall, kSkipCount };
const char* kSkipNames[kSkipCount] = {"non-indexed/non-triangle", "no POSITION", "position not FLOAT3/4",
                                      "DEFAULT/dynamic pool", "lock failed", "build budget", "too small"};

ComPtr<ID3D12Resource> CreateBuffer(ID3D12Device* dev, UINT64 size, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_FLAGS flags,
                                    D3D12_RESOURCE_STATES state)
{
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type                  = heap;
    D3D12_RESOURCE_DESC d    = {};
    d.Dimension              = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width                  = std::max<UINT64>(size, 256);
    d.Height                 = 1;
    d.DepthOrArraySize       = 1;
    d.MipLevels              = 1;
    d.SampleDesc.Count       = 1;
    d.Layout                 = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    d.Flags                  = flags;
    ComPtr<ID3D12Resource> r;
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&r))))
        return nullptr;
    return r;
}

class RaytracingFoundation final : public IExtension {
public:
    const char* GetName() const override { return "RaytracingFoundation"; }
    int         GetPriority() const override { return 50; }

    bool OnInitialize(IHost* host) override
    {
        m_host = host;
        if (host->GetConfigInt("Raytracing", "Enabled", 1) == 0) return false;
        if (host->GetBackend() != Backend::D3D9On12) {
            host->Log(LogLevel::Warning, "RaytracingFoundation: requires Backend=D3D9On12, disabled");
            return false;
        }
        const D3D12Caps* caps = host->GetD3D12Caps();
        if (!caps || caps->raytracingTier < D3D12_RAYTRACING_TIER_1_0) {
            if (sizeof(void*) == 4)
                host->Log(LogLevel::Warning,
                          "RaytracingFoundation: the GPU driver does not expose DXR to 32-bit processes (Bully.exe is "
                          "x86) - disabled. Ray tracing needs the x64 out-of-process bridge (see README).");
            else
                host->Log(LogLevel::Warning, "RaytracingFoundation: GPU does not support DXR, disabled");
            return false;
        }
        if (FAILED(host->GetD3D12Device()->QueryInterface(IID_PPV_ARGS(&m_device)))) {
            host->Log(LogLevel::Warning, "RaytracingFoundation: ID3D12Device5 unavailable, disabled");
            return false;
        }

        m_worldRegister     = host->GetConfigInt("Raytracing", "WorldMatrixRegister", -1);
        m_registerIsColumn  = host->GetConfigInt("Raytracing", "WorldMatrixColumnMajor", 1) != 0;
        m_maxInstances      = static_cast<UINT>(std::max(64, host->GetConfigInt("Raytracing", "MaxInstances", 16384)));
        m_maxBuildsPerFrame = static_cast<UINT>(std::max(1, host->GetConfigInt("Raytracing", "MaxBlasBuildsPerFrame", 48)));
        m_evictAfterFrames  = static_cast<uint64_t>(std::max(30, host->GetConfigInt("Raytracing", "EvictAfterFrames", 900)));
        m_minPrimitives     = static_cast<UINT>(std::max(1, host->GetConfigInt("Raytracing", "MinPrimitives", 2)));
        m_statsInterval     = static_cast<uint64_t>(std::max(0, host->GetConfigInt("Raytracing", "StatsIntervalFrames", 1800)));

        host->Log(LogLevel::Info, "RaytracingFoundation: DXR tier %d.%d, world matrix source: %s",
                  caps->raytracingTier / 10, caps->raytracingTier % 10,
                  m_worldRegister < 0 ? "fixed-function D3DTS_WORLD" : "vertex shader constants");
        return true;
    }

    void OnShutdown() override
    {
        ReleaseAll();
        m_tlas.Reset();
        m_tlasScratch.Reset();
        for (auto& b : m_instanceBuffers) b.Reset();
        m_device.Reset();
    }

    void OnDeviceLost() override { ReleaseAll(); }

    DrawResult OnDraw(const DrawCallInfo& d) override
    {
        if (d.kind != DrawKind::IndexedPrimitive || d.primitiveType != D3DPT_TRIANGLELIST || !d.indexBuffer ||
            !d.vertexDeclaration) {
            ++m_skips[kSkipNotIndexedTris];
            return DrawResult::Continue;
        }
        if (d.primitiveCount < m_minPrimitives) { ++m_skips[kSkipSmall]; return DrawResult::Continue; }
        if (m_frameInstances.size() >= m_maxInstances) return DrawResult::Continue;

        D3DVERTEXELEMENT9 el[MAXD3DDECLLENGTH + 1];
        UINT              n = 0;
        if (FAILED(d.vertexDeclaration->GetDeclaration(el, &n))) return DrawResult::Continue;
        const D3DVERTEXELEMENT9* pos = nullptr;
        for (UINT i = 0; i < n && el[i].Stream != 0xFF; ++i)
            if (el[i].Usage == D3DDECLUSAGE_POSITION && el[i].UsageIndex == 0) { pos = &el[i]; break; }
        if (!pos || pos->Stream >= 4 || !d.streamSource[pos->Stream]) { ++m_skips[kSkipNoPosition]; return DrawResult::Continue; }
        if (pos->Type != D3DDECLTYPE_FLOAT3 && pos->Type != D3DDECLTYPE_FLOAT4) { ++m_skips[kSkipPosFormat]; return DrawResult::Continue; }

        MeshKey k     = {};
        k.vb          = d.streamSource[pos->Stream];
        k.ib          = d.indexBuffer;
        k.vbOffset    = d.streamOffset[pos->Stream];
        k.stride      = d.streamStride[pos->Stream];
        k.posOffset   = pos->Offset;
        k.posType     = pos->Type;
        k.baseVertex  = d.baseVertexIndex;
        k.minVertex   = d.minVertexIndex;
        k.numVertices = d.numVertices;
        k.startIndex  = d.startIndexOrVertex;
        k.primCount   = d.primitiveCount;

        auto it = m_blas.find(k);
        if (it == m_blas.end()) {
            if (!QueueBuild(k)) return DrawResult::Continue;
            it = m_blas.find(k);
            if (it == m_blas.end()) return DrawResult::Continue;
        }
        it->second.lastUsedFrame = d.frameIndex;

        D3D12_RAYTRACING_INSTANCE_DESC inst = {};
        WorldTransform(d, inst.Transform);
        inst.InstanceID                          = static_cast<UINT>(m_frameInstances.size()) & 0xFFFFFF;
        inst.InstanceMask                        = 0xFF;
        inst.InstanceContributionToHitGroupIndex = 0;
        inst.Flags                               = D3D12_RAYTRACING_INSTANCE_FLAG_NONE;
        m_frameInstances.push_back({inst, &it->second});
        return DrawResult::Continue;
    }

    void OnPresent(const FrameInfo& frame) override
    {
        BuildFrame();
        Evict(frame.frameIndex);
        if (m_statsInterval && frame.frameIndex % m_statsInterval == 0 && frame.frameIndex) {
            char skips[512] = {};
            size_t off = 0;
            for (int i = 0; i < kSkipCount; ++i)
                off += static_cast<size_t>(snprintf(skips + off, sizeof(skips) - off, "%s%s=%llu", i ? ", " : "",
                                                    kSkipNames[i], m_skips[i]));
            m_host->Log(LogLevel::Info, "RaytracingFoundation: %zu cached BLAS, %u instances last frame | skipped: %s",
                        m_blas.size(), m_lastInstanceCount, skips);
        }
    }

private:
    struct FrameInstance {
        D3D12_RAYTRACING_INSTANCE_DESC desc;
        Blas*                          blas;
    };

    void WorldTransform(const DrawCallInfo& d, FLOAT out[3][4]) const
    {
        if (m_worldRegister >= 0 && m_worldRegister + 4 <= 256 && d.vertexShader) {
            const float* r = d.vsConstantsF + m_worldRegister * 4;
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 4; ++j)
                    out[i][j] = m_registerIsColumn ? r[i * 4 + j]
                                                   : r[j * 4 + i];
            return;
        }
        const D3DMATRIX& m = *d.worldTransform;
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 4; ++j) out[i][j] = m.m[j][i];
    }

    bool QueueBuild(const MeshKey& k)
    {
        if (m_pending.size() >= m_maxBuildsPerFrame) { ++m_skips[kSkipBudget]; return false; }

        D3DVERTEXBUFFER_DESC vd;
        D3DINDEXBUFFER_DESC  id;
        if (FAILED(k.vb->GetDesc(&vd)) || FAILED(k.ib->GetDesc(&id))) return false;
        const bool vbReadable = (vd.Pool == D3DPOOL_MANAGED || vd.Pool == D3DPOOL_SYSTEMMEM) && !(vd.Usage & D3DUSAGE_DYNAMIC);
        const bool ibReadable = (id.Pool == D3DPOOL_MANAGED || id.Pool == D3DPOOL_SYSTEMMEM) && !(id.Usage & D3DUSAGE_DYNAMIC);
        if (!vbReadable || !ibReadable) { ++m_skips[kSkipPool]; return false; }

        const UINT   indexSize  = id.Format == D3DFMT_INDEX32 ? 4u : 2u;
        const UINT   indexCount = k.primCount * 3;
        const UINT64 firstVtx   = static_cast<UINT64>(static_cast<INT64>(k.baseVertex) + k.minVertex);
        const UINT64 vtxStart   = k.vbOffset + firstVtx * k.stride;
        const UINT64 vtxBytes   = static_cast<UINT64>(k.numVertices) * k.stride;
        const UINT64 idxStart   = static_cast<UINT64>(k.startIndex) * indexSize;
        const UINT64 idxBytes   = static_cast<UINT64>(indexCount) * indexSize;
        if (k.numVertices == 0 || k.stride == 0 || vtxStart + vtxBytes > vd.Size || idxStart + idxBytes > id.Size)
            return false;

        const UINT64 posBytes   = static_cast<UINT64>(k.numVertices) * 12;
        const UINT64 uploadSize = posBytes + static_cast<UINT64>(indexCount) * 4;
        ComPtr<ID3D12Resource> upload = CreateBuffer(m_device.Get(), uploadSize, D3D12_HEAP_TYPE_UPLOAD,
                                                     D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
        if (!upload) return false;

        uint8_t* dst = nullptr;
        if (FAILED(upload->Map(0, nullptr, reinterpret_cast<void**>(&dst)))) return false;

        bool ok = false;
        void* vsrc = nullptr;
        void* isrc = nullptr;
        if (SUCCEEDED(k.vb->Lock(static_cast<UINT>(vtxStart), static_cast<UINT>(vtxBytes), &vsrc, D3DLOCK_READONLY))) {
            if (SUCCEEDED(k.ib->Lock(static_cast<UINT>(idxStart), static_cast<UINT>(idxBytes), &isrc, D3DLOCK_READONLY))) {
                auto* outPos = reinterpret_cast<float*>(dst);
                const auto* vbytes = static_cast<const uint8_t*>(vsrc);
                for (UINT v = 0; v < k.numVertices; ++v)
                    memcpy(outPos + v * 3, vbytes + static_cast<size_t>(v) * k.stride + k.posOffset, 12);

                auto* outIdx = reinterpret_cast<uint32_t*>(dst + posBytes);
                ok = true;
                for (UINT i = 0; i < indexCount; ++i) {
                    const uint32_t raw = indexSize == 4 ? static_cast<const uint32_t*>(isrc)[i]
                                                        : static_cast<const uint16_t*>(isrc)[i];
                    const int64_t local = static_cast<int64_t>(raw) - k.minVertex;
                    if (local < 0 || local >= k.numVertices) { ok = false; break; }
                    outIdx[i] = static_cast<uint32_t>(local);
                }
                k.ib->Unlock();
            }
            k.vb->Unlock();
        }
        upload->Unmap(0, nullptr);
        if (!ok) { ++m_skips[kSkipLockFailed]; return false; }

        k.vb->AddRef();
        k.ib->AddRef();
        m_blas.emplace(k, Blas{});
        m_pending.push_back({k, upload, k.numVertices, indexCount});
        return true;
    }

    void BuildFrame()
    {
        const UINT instanceCount = static_cast<UINT>(m_frameInstances.size());
        m_lastInstanceCount      = instanceCount;
        if (m_pending.empty() && instanceCount == 0) return;

        ID3D12GraphicsCommandList* base = m_host->BeginCommandList();
        ComPtr<ID3D12GraphicsCommandList4> cl;
        if (!base || FAILED(base->QueryInterface(IID_PPV_ARGS(&cl)))) {
            if (base) m_host->ExecuteCommandList(base);
            m_pending.clear();
            m_frameInstances.clear();
            return;
        }

        for (PendingBuild& p : m_pending) {
            auto it = m_blas.find(p.key);
            if (it == m_blas.end()) continue;

            D3D12_RAYTRACING_GEOMETRY_DESC g          = {};
            g.Type                                    = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
            g.Flags                                   = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
            g.Triangles.VertexBuffer.StartAddress     = p.upload->GetGPUVirtualAddress();
            g.Triangles.VertexBuffer.StrideInBytes    = 12;
            g.Triangles.VertexFormat                  = DXGI_FORMAT_R32G32B32_FLOAT;
            g.Triangles.VertexCount                   = p.vertexCount;
            g.Triangles.IndexBuffer                   = p.upload->GetGPUVirtualAddress() + static_cast<UINT64>(p.vertexCount) * 12;
            g.Triangles.IndexFormat                   = DXGI_FORMAT_R32_UINT;
            g.Triangles.IndexCount                    = p.indexCount;

            D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS in = {};
            in.Type           = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
            in.Flags          = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
            in.NumDescs       = 1;
            in.DescsLayout    = D3D12_ELEMENTS_LAYOUT_ARRAY;
            in.pGeometryDescs = &g;

            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO pre = {};
            m_device->GetRaytracingAccelerationStructurePrebuildInfo(&in, &pre);
            if (pre.ResultDataMaxSizeInBytes == 0) continue;

            ComPtr<ID3D12Resource> as = CreateBuffer(m_device.Get(), pre.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
                                                     D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                                     D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
            ComPtr<ID3D12Resource> scratch = CreateBuffer(m_device.Get(), pre.ScratchDataSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
                                                          D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                                          D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            if (!as || !scratch) continue;

            D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC b = {};
            b.Inputs                           = in;
            b.DestAccelerationStructureData    = as->GetGPUVirtualAddress();
            b.ScratchAccelerationStructureData = scratch->GetGPUVirtualAddress();
            cl->BuildRaytracingAccelerationStructure(&b, 0, nullptr);

            it->second.as    = as;
            it->second.built = true;
            m_host->DeferredRelease(scratch.Detach());
            m_host->DeferredRelease(p.upload.Detach());
        }
        m_pending.clear();

        D3D12_RESOURCE_BARRIER uav = {};
        uav.Type                   = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        cl->ResourceBarrier(1, &uav);

        if (instanceCount > 0 && EnsureTlas()) {
            ID3D12Resource* ib = m_instanceBuffers[m_instanceRing].Get();
            m_instanceRing     = (m_instanceRing + 1) % kInstanceRing;
            D3D12_RAYTRACING_INSTANCE_DESC* mapped = nullptr;
            UINT written = 0;
            if (SUCCEEDED(ib->Map(0, nullptr, reinterpret_cast<void**>(&mapped)))) {
                for (const FrameInstance& fi : m_frameInstances) {
                    if (!fi.blas->built || !fi.blas->as) continue;
                    mapped[written] = fi.desc;
                    mapped[written].AccelerationStructure = fi.blas->as->GetGPUVirtualAddress();
                    ++written;
                }
                ib->Unmap(0, nullptr);
            }

            D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC b = {};
            b.Inputs.Type          = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
            b.Inputs.Flags         = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
            b.Inputs.NumDescs      = written;
            b.Inputs.DescsLayout   = D3D12_ELEMENTS_LAYOUT_ARRAY;
            b.Inputs.InstanceDescs = ib->GetGPUVirtualAddress();
            b.DestAccelerationStructureData    = m_tlas->GetGPUVirtualAddress();
            b.ScratchAccelerationStructureData = m_tlasScratch->GetGPUVirtualAddress();
            cl->BuildRaytracingAccelerationStructure(&b, 0, nullptr);
            cl->ResourceBarrier(1, &uav);
            m_lastInstanceCount = written;
        }

        m_host->ExecuteCommandList(base);
        m_frameInstances.clear();
    }

    bool EnsureTlas()
    {
        if (m_tlas) return true;
        if (m_tlasFailed) return false;

        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS in = {};
        in.Type        = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
        in.Flags       = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
        in.NumDescs    = m_maxInstances;
        in.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO pre = {};
        m_device->GetRaytracingAccelerationStructurePrebuildInfo(&in, &pre);

        m_tlas        = CreateBuffer(m_device.Get(), pre.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
                                     D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                     D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
        m_tlasScratch = CreateBuffer(m_device.Get(), pre.ScratchDataSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
                                     D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        bool ok = m_tlas && m_tlasScratch;
        for (auto& b : m_instanceBuffers) {
            b = CreateBuffer(m_device.Get(), sizeof(D3D12_RAYTRACING_INSTANCE_DESC) * m_maxInstances,
                             D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
            ok = ok && b;
        }
        if (!ok) {
            m_host->Log(LogLevel::Error, "RaytracingFoundation: TLAS allocation failed (%u instances)", m_maxInstances);
            m_tlas.Reset();
            m_tlasScratch.Reset();
            m_tlasFailed = true;
            return false;
        }
        m_tlas->SetName(L"BullyDX12 TLAS");
        m_host->PublishResource("rt.tlas", m_tlas.Get());
        m_host->Log(LogLevel::Info, "RaytracingFoundation: TLAS ready (%llu KB, max %u instances) published as 'rt.tlas'",
                    pre.ResultDataMaxSizeInBytes / 1024, m_maxInstances);
        return true;
    }

    void Evict(uint64_t frame)
    {
        if (frame % 60 != 0) return;
        for (auto it = m_blas.begin(); it != m_blas.end();) {
            if (it->second.built && frame - it->second.lastUsedFrame > m_evictAfterFrames) {
                if (it->second.as) m_host->DeferredRelease(it->second.as.Detach());
                it->first.vb->Release();
                it->first.ib->Release();
                it = m_blas.erase(it);
            } else {
                ++it;
            }
        }
    }

    void ReleaseAll()
    {
        m_pending.clear();
        m_frameInstances.clear();
        for (auto& kv : m_blas) {
            if (kv.second.as) m_host->DeferredRelease(kv.second.as.Detach());
            kv.first.vb->Release();
            kv.first.ib->Release();
        }
        m_blas.clear();
    }

    static constexpr UINT kInstanceRing = 3;

    IHost*                                         m_host = nullptr;
    ComPtr<ID3D12Device5>                          m_device;
    std::unordered_map<MeshKey, Blas, MeshKeyHash> m_blas;
    std::vector<PendingBuild>                      m_pending;
    std::vector<FrameInstance>                     m_frameInstances;
    ComPtr<ID3D12Resource>                         m_tlas, m_tlasScratch;
    ComPtr<ID3D12Resource>                         m_instanceBuffers[kInstanceRing];
    UINT                                           m_instanceRing = 0;
    bool                                           m_tlasFailed = false;

    int      m_worldRegister = -1;
    bool     m_registerIsColumn = true;
    UINT     m_maxInstances = 16384;
    UINT     m_maxBuildsPerFrame = 48;
    uint64_t m_evictAfterFrames = 900;
    UINT     m_minPrimitives = 2;
    uint64_t m_statsInterval = 1800;
    UINT     m_lastInstanceCount = 0;
    unsigned long long m_skips[kSkipCount] = {};
};

}

BULLYDX12_DECLARE_EXTENSION(RaytracingFoundation)
