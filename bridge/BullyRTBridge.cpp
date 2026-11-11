// BullyRTBridge.exe - 64-bit ray tracing companion process for BullyDX12.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <psapi.h>
#include <array>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

#include "BridgeProtocol.h"
#include "RTX_CSTrace.h"
#include "RTX_CSComposite.h"
#include "RTX_CSExposure.h"
#include "RTX_CSVolRestir.h"
#include "RTX_CSFroxel.h"
#include "RTX_CSSharcUpdate.h"
#include "RTX_CSSharcResolve.h"
#include "RTX_CSAirIntegrate.h"
#include <immintrin.h>
#include "RTX_CSNoisy.h"
#include "nvsdk_ngx.h"
#include "nvsdk_ngx_helpers.h"
#include "nvsdk_ngx_helpers_dlssd.h"
#include "nvsdk_ngx_helpers_dlssd_d3d.h"

namespace {
void Rgb565(uint16_t c, uint8_t o[3]) { o[0] = static_cast<uint8_t>(((c >> 11) & 31) * 255 / 31); o[1] = static_cast<uint8_t>(((c >> 5) & 63) * 255 / 63); o[2] = static_cast<uint8_t>((c & 31) * 255 / 31); }

void DecodeColorBlock(const uint8_t* b, uint8_t out[16][4], bool bc1)
{
    const uint16_t c0 = static_cast<uint16_t>(b[0] | (b[1] << 8)), c1 = static_cast<uint16_t>(b[2] | (b[3] << 8));
    uint8_t pal[4][4] = {};
    Rgb565(c0, pal[0]); Rgb565(c1, pal[1]);
    pal[0][3] = pal[1][3] = pal[2][3] = pal[3][3] = 255;
    for (int k = 0; k < 3; ++k) {
        if (!bc1 || c0 > c1) {
            pal[2][k] = static_cast<uint8_t>((2 * pal[0][k] + pal[1][k]) / 3);
            pal[3][k] = static_cast<uint8_t>((pal[0][k] + 2 * pal[1][k]) / 3);
        } else {
            pal[2][k] = static_cast<uint8_t>((pal[0][k] + pal[1][k]) / 2);
            pal[3][k] = 0;
        }
    }
    if (bc1 && c0 <= c1) pal[3][3] = 0;
    const uint32_t bits = b[4] | (b[5] << 8) | (b[6] << 16) | (static_cast<uint32_t>(b[7]) << 24);
    for (int i = 0; i < 16; ++i) memcpy(out[i], pal[(bits >> (2 * i)) & 3], 4);
}

template <typename T> void DecodeTexture(const bdx12::bridge::TexOp& op, const uint8_t* src, T& dst)
{
    dst.w = op.width; dst.h = op.height;
    dst.rgba.assign(static_cast<size_t>(op.width) * op.height * 4, 255);
    const DXGI_FORMAT f = static_cast<DXGI_FORMAT>(op.dxgiFormat);
    if (f == DXGI_FORMAT_B8G8R8A8_UNORM || f == DXGI_FORMAT_B8G8R8X8_UNORM) {
        for (uint32_t y = 0; y < op.height && y < op.rows; ++y)
            for (uint32_t x = 0; x < op.width; ++x) {
                const uint8_t* s = src + static_cast<size_t>(y) * op.rowBytes + x * 4;
                uint8_t* d = &dst.rgba[(static_cast<size_t>(y) * op.width + x) * 4];
                d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = f == DXGI_FORMAT_B8G8R8X8_UNORM ? 255 : s[3];
            }
        return;
    }
    const bool bc1 = f == DXGI_FORMAT_BC1_UNORM, bc2 = f == DXGI_FORMAT_BC2_UNORM, bc3 = f == DXGI_FORMAT_BC3_UNORM;
    if (!bc1 && !bc2 && !bc3) return;
    const uint32_t blockBytes = bc1 ? 8 : 16;
    for (uint32_t by = 0; by < (op.height + 3) / 4 && by < op.rows; ++by)
        for (uint32_t bx = 0; bx < (op.width + 3) / 4; ++bx) {
            const uint8_t* b = src + static_cast<size_t>(by) * op.rowBytes + bx * blockBytes;
            uint8_t px[16][4];
            DecodeColorBlock(bc1 ? b : b + 8, px, bc1);
            if (bc2)
                for (int i = 0; i < 16; ++i) px[i][3] = static_cast<uint8_t>(((b[i / 2] >> ((i & 1) * 4)) & 15) * 17);
            if (bc3) {
                uint8_t a[8] = {b[0], b[1]};
                for (int k = 2; k < 8; ++k)
                    a[k] = a[0] > a[1] ? static_cast<uint8_t>(((8 - k) * a[0] + (k - 1) * a[1]) / 7)
                                       : (k < 6 ? static_cast<uint8_t>(((6 - k) * a[0] + (k - 1) * a[1]) / 5) : (k == 6 ? 0 : 255));
                uint64_t bits = 0;
                for (int k = 0; k < 6; ++k) bits |= static_cast<uint64_t>(b[2 + k]) << (8 * k);
                for (int i = 0; i < 16; ++i) px[i][3] = a[(bits >> (3 * i)) & 7];
            }
            for (int i = 0; i < 16; ++i) {
                const uint32_t x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
                if (x < op.width && y < op.height) memcpy(&dst.rgba[(static_cast<size_t>(y) * op.width + x) * 4], px[i], 4);
            }
        }
}

bool WriteTga(const std::wstring& path, uint32_t w, uint32_t h, const std::vector<uint8_t>& rgba)
{
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return false;
    uint8_t hdr[18] = {0, 0, 2};
    hdr[12] = static_cast<uint8_t>(w & 255); hdr[13] = static_cast<uint8_t>(w >> 8);
    hdr[14] = static_cast<uint8_t>(h & 255); hdr[15] = static_cast<uint8_t>(h >> 8);
    hdr[16] = 32; hdr[17] = 0x28;
    fwrite(hdr, 1, 18, f);
    std::vector<uint8_t> bgra(rgba.size());
    for (size_t i = 0; i < rgba.size(); i += 4) { bgra[i] = rgba[i + 2]; bgra[i + 1] = rgba[i + 1]; bgra[i + 2] = rgba[i]; bgra[i + 3] = rgba[i + 3]; }
    fwrite(bgra.data(), 1, bgra.size(), f);
    fclose(f);
    return true;
}

void LogNrdAssert(const char* msg); }
#define NRD_INTEGRATION_ASSERT(expr, msg) do { if (!(expr)) LogNrdAssert(msg); } while (0)
#include "NRI.h"
#include "Extensions/NRIHelper.h"
#include "Extensions/NRIWrapperD3D12.h"
#include "NRD.h"
#include "NRDIntegration.hpp"

using Microsoft::WRL::ComPtr;
using namespace bdx12::bridge;

namespace {

FILE* g_log = nullptr;
std::wstring g_dir;
Header* g_header = nullptr;

void Log(const char* fmt, ...)
{
    char msg[1024];
    va_list a;
    va_start(a, fmt);
    vsnprintf(msg, sizeof(msg), fmt, a);
    va_end(a);
    SYSTEMTIME t;
    GetLocalTime(&t);
    if (g_log) {
        fprintf(g_log, "[%02u:%02u:%02u.%03u] %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, msg);
        fflush(g_log);
    }
    OutputDebugStringA(msg);
}

void LogNrdAssert(const char* msg)
{
    static int count = 0;
    if (count++ < 50) Log("NRD ASSERT: %s", msg);
}

void Fail(const char* fmt, ...)
{
    char msg[256];
    va_list a;
    va_start(a, fmt);
    vsnprintf(msg, sizeof(msg), fmt, a);
    va_end(a);
    Log("FATAL: %s", msg);
    if (g_header) {
        strncpy_s(g_header->bridgeMessage, msg, _TRUNCATE);
        MemoryBarrier();
        g_header->bridgeState = 2;
    }
}

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

bool Invert4x4(const float m[4][4], float out[4][4])
{
    double a[4][8];
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 8; ++j) a[i][j] = j < 4 ? m[i][j] : (j - 4 == i ? 1.0 : 0.0);
    for (int c = 0; c < 4; ++c) {
        int p = c;
        for (int r = c + 1; r < 4; ++r)
            if (std::fabs(a[r][c]) > std::fabs(a[p][c])) p = r;
        if (std::fabs(a[p][c]) < 1e-12) return false;
        if (p != c)
            for (int j = 0; j < 8; ++j) std::swap(a[p][j], a[c][j]);
        const double inv = 1.0 / a[c][c];
        for (int j = 0; j < 8; ++j) a[c][j] *= inv;
        for (int r = 0; r < 4; ++r) {
            if (r == c) continue;
            const double f = a[r][c];
            for (int j = 0; j < 8; ++j) a[r][j] -= f * a[c][j];
        }
    }
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) out[i][j] = static_cast<float>(a[i][j + 4]);
    return true;
}

constexpr uint32_t kMaxPortals = 128;
constexpr UINT     kFrameCbStride = 65536;
struct FrameConstants {
    float    viewProj[4][4];
    float    invViewProj[4][4];
    float    prevViewProj[4][4];
    float    cameraPos[4];
    float    sunDir[4];
    float    sunColor[4];
    float    skyColor[4];
    float    projParams[4];
    float    strength[4];
    uint32_t size[2];
    float    hitBaked;
    float    lightCount;
    float    atmosphere[4];
    float    trueSun[4];
    float    clouds[4];
    float    lightPos[kMaxLights][4];
    float    lightCol[kMaxLights][4];
    float    lightDir[kMaxLights][4];
    float    lightAtt[kMaxLights][4];
    uint32_t debugMode;
    uint32_t pad2[3];
    float    exposure[4];
    float    windowLight[4];
    uint32_t rtSize[2];
    uint32_t rtScale;
    uint32_t options;
    float    osd[4];
    uint32_t osdText[32];
    float    fog[4];
    float    bounce[4];
    float    weatherTop[4];
    float    weatherBot[4];
    float    prevCam[4];
    float    portalInfo[4];
    float    portalC[kMaxPortals][4];
    float    portalU[kMaxPortals][4];
    float    portalV[kMaxPortals][4];
    float    portalN[kMaxPortals][4];
    float    sharc[4];
    float    sharc2[4];
    float    movedMin[8][4];
    float    movedMax[8][4];
    float    restirInfo[4];
    float    restirTune[4];
    uint32_t restirMap[80];
    float    aoInfo[4];
    float    volInfo[4];
    uint32_t rtJitter[4];
    float    roomAmbient[4];
    float    fixInfo[4];
    float    fixGrid[4];
    float    fixPos[768][4];
    float    fixCol[768][4];
    uint32_t fixCells[1024];
    uint32_t fixList[4096];
};
static_assert(sizeof(FrameConstants) <= kFrameCbStride, "constant buffer stride");

struct Camera {
    float view[4][4];
    float proj[4][4];
    float ndcA, ndcB;
    float orthoError;
};

Camera Decompose(const float m[4][4])
{
    Camera c = {};
    auto len = [](const float* r) { return std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]); };
    const float s = std::max(len(m[3]), 1e-6f), sx = std::max(len(m[0]), 1e-6f), sy = std::max(len(m[1]), 1e-6f);
    for (int j = 0; j < 4; ++j) {
        c.view[0][j] = m[0][j] / sx;
        c.view[1][j] = m[1][j] / sy;
        c.view[2][j] = m[3][j] / s;
    }
    c.view[3][3] = 1.0f;
    const float a = m[2][0] * c.view[2][0] + m[2][1] * c.view[2][1] + m[2][2] * c.view[2][2];
    const float bb = m[2][3] - a * c.view[2][3];
    c.proj[0][0] = sx;
    c.proj[1][1] = sy;
    c.proj[2][2] = a;
    c.proj[2][3] = bb;
    c.proj[3][2] = s;
    c.ndcA = a / s;
    c.ndcB = bb / s;
    auto dot3 = [](const float* x, const float* y) { return x[0] * y[0] + x[1] * y[1] + x[2] * y[2]; };
    c.orthoError = std::fabs(dot3(c.view[0], c.view[1])) + std::fabs(dot3(c.view[0], c.view[2])) + std::fabs(dot3(c.view[1], c.view[2]));
    return c;
}

void ToColumnMajor(const float r[4][4], float out[16])
{
    for (int col = 0; col < 4; ++col)
        for (int row = 0; row < 4; ++row) out[col * 4 + row] = r[row][col];
}

class Bridge {
public:
    bool Init(const std::wstring& mappingName);
    void Run();

private:
    struct Mesh {
        ComPtr<ID3D12Resource> blas;
        ComPtr<ID3D12Resource> attr;
        uint32_t               attrIndex = 0;
        uint32_t               vertexCount = 0;
    };
    struct CpuMesh { std::vector<float> pos, uv; std::vector<uint32_t> idx, col; };
    struct CpuTex { float glassTint[3] = {1.0f, 1.0f, 1.0f}; uint32_t w = 0, h = 0; std::vector<uint8_t> rgba; bool written = false; bool veil = false; bool glowTex = false; float glowCol[3] = {-1.0f, 0.0f, 0.0f}; uint32_t hash = 0; float emissive = 0, lampPower = 0, lampCol[3] = {1, 1, 1}; bool glass = false, mirror = false, night = false; };
    struct Material { float emissive = 0, lampPower = 0, col[3] = {1, 1, 1}; int glass = 0, night = 0; };
    std::unordered_map<uint32_t, Material> m_materials;
    bool m_materialsLoaded = false;
    void LoadMaterials();
    struct LampCluster { float lo[3], hi[3], area, n[3]; };
    float m_bounceColor = 1.5f, m_beams = 1.0f, m_sharcScale = 50.0f, m_sharcTile = 8.0f, m_sharcSurfaces = 4.0f, m_beamsOutside = 1.0f, m_lampFalloff = 0.02f, m_lampReach = 80.0f, m_imageGain = 1.35f, m_fog[4] = {40.0f, 0.006f, 0.7f, 450.0f}, m_windowPower = 0.4f;
    std::unordered_map<uint64_t, std::vector<LampCluster>> m_lampClusters;
    float m_textureLamps = 0.0f;
    float m_emitLampPower = 3.0f, m_emitGlowSent = 2.0f, m_gsaNearLamp = 0.2f;
    bool  m_emitLogged = false;
    uint32_t m_settingsGen = 0;
    struct CatalogLamp { float pos[3], n[3], area_m2, col[3]; int area; uint32_t flags, model; bool flickOn = true; uint64_t flickAt = 0; };
    std::vector<CatalogLamp> m_catalog;
    bool  m_catalogRead = false;
    float m_catalogPower = 1.0f, m_fixtureReach = 15.0f;
    static constexpr float kCandelaToRadiance = 0.04f;
    int   m_lampsOn = 18, m_lampsOff = 7;
    void  LoadLampCatalog();
    std::vector<std::array<float, 3>> m_emitNear;
    const std::vector<LampCluster>& LampClustersOf(uint32_t meshId, uint32_t textureId);
    struct GameLight { char name[64]; float pos[3], dif[3], amb[3], dim, nightDif[3], nightAmb[3], nightDim, att[3], dir[3], cosOuter, cosInner, zone; bool spot, cutscene, flicker, flickOn, daylight; uint64_t flickAt; };
    std::vector<GameLight> m_gameLights[64];
    bool m_gameLightsRead = false;
    int m_gameLightsArea = -2;
    const GameLight* m_roomSunLight = nullptr;
    float m_roomSun[3] = {0.0f, 0.0f, 1.0f};
    bool m_haveRoomSun = false;
    float m_gameArea = -1.0f, m_gameHour = -1.0f, m_gameLightPower = 1.0f, m_gameLightAmbient = 1.0f, m_gameDaylight = 0.0f, m_roomSunOn = 1.0f, m_roomSunPower = 3.0f, m_decalGlow = 1.8f, m_decalLight = 0.35f, m_sharcAccumulate = 32.0f, m_sharcStale = 64.0f, m_windowsPerRay = 128.0f;
    void LoadGameLights();
    bool UseGameLights(FrameConstants& fc, const FrameSlot& s);
    float m_expoSettings[4] = {0.0f, 0.5f, 8.0f, 0.04f}, m_lampTint[3] = {1.0f, 0.97f, 0.9f};
    float m_windowLight[3] = {16.0f, 15.0f, 12.5f}, m_sunTint[3] = {1.0f, 0.95f, 0.85f};
    int m_optVolumetric = 1, m_optVolKey = 0, m_optRT = 1, m_optReflections = 1, m_optSunShadows = 1, m_optLocalShadows = 1, m_optLighting = 2;
    bool m_optKeyDown[5] = {}, m_f9Down = false;
    float m_roomFill = 0.0f, m_samplePeriod = 32.0f;
    uint64_t m_osdUntil = 0, m_osdTextUntil = 0;
    std::string m_osdText;
    void ShowText(const std::string& s) { m_osdText = s; m_osdTextUntil = GetTickCount64() + 3000; Log("On screen: %s", s.c_str()); }
    void UpdateOptions();
    bool m_settingsRead = false;
    uint32_t m_rtScale = 2, m_pendingScale = 0;
    void ReadSettings();
    std::unordered_map<uint32_t, CpuTex> m_cpuTex;
    std::unordered_map<uint32_t, CpuMesh> m_cpuMeshes;
    bool m_dumpKeyDown = false, m_debugKeyDown = false, m_fixturesLogged = false;
    uint32_t m_lastFixtures = 0;
    uint32_t m_debugMode = 0;
    struct Pending {
        uint32_t               meshId;
        ComPtr<ID3D12Resource> upload;
        uint32_t               vertexCount, indexCount;
        uint64_t               bytes;
    };
    struct PendingTexture {
        uint32_t               id;
        ComPtr<ID3D12Resource> upload, texture;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
    };
    struct GeomRecord {
        uint32_t attr, posOffset, uvOffset, idxOffset;
        int32_t  tex;
        uint32_t flags;
        float    alphaRef;
        uint32_t colorOffset;
        uint32_t prevAttr, prevPosOffset, moved, pad0;
        float    prevT[3][4];
    };
    struct PrevDyn { uint32_t vertexCount, indexCount, textureId, attr, dataOffset; bool used; };
    std::vector<PrevDyn> m_prevDyn;
    std::unordered_map<uint64_t, std::vector<std::array<float, 12>>> m_prevPlacements;
    std::unordered_map<uint32_t, std::array<float, 6>> m_meshBounds;
    struct FreedAttr { uint32_t index; uint64_t fence; };
    static constexpr UINT kSharcEntries = 1u << 21;
    static constexpr UINT kTexBase = 96, kMaxTextures = 4096, kAttrBase = kTexBase + kMaxTextures, kMaxAttrs = 16384;
    D3D12_CPU_DESCRIPTOR_HANDLE HeapCpu(UINT i) const
    {
        D3D12_CPU_DESCRIPTOR_HANDLE h = m_heap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += static_cast<SIZE_T>(i) * m_heapInc;
        return h;
    }
    D3D12_GPU_DESCRIPTOR_HANDLE HeapGpu(UINT i) const
    {
        D3D12_GPU_DESCRIPTOR_HANDLE h = m_heap->GetGPUDescriptorHandleForHeapStart();
        h.ptr += static_cast<UINT64>(i) * m_heapInc;
        return h;
    }
    void CreateRawSrv(ID3D12Resource* buffer, UINT64 bytes, UINT attrIndex);
    struct Retired {
        ComPtr<ID3D12Resource> res;
        uint64_t               fence;
    };
    struct FrameList {
        ComPtr<ID3D12CommandAllocator>     alloc;
        ComPtr<ID3D12GraphicsCommandList4> list;
        uint64_t                           fence = 0;
    };

    static constexpr nrd::Identifier kSigma = 0, kReblur = 1;
    bool OpenSharedResources();
    bool ProcessSlot(FrameSlot& s);
    bool EnsureTlas();
    void Retire(ComPtr<ID3D12Resource> r) { m_retired.push_back({std::move(r), m_internalValue + 1}); }
    void CollectRetired();
    std::wstring Name(const wchar_t* what) const { return m_mappingName + L"_" + what; }

    std::wstring                 m_mappingName;
    HANDLE                       m_mapping = nullptr;
    uint8_t*                     m_view = nullptr;
    HANDLE                       m_gameProcess = nullptr;
    HANDLE                       m_frameEvent = nullptr;
    HANDLE                       m_slotFreeEvent = nullptr;

    ComPtr<ID3D12Device5>        m_device;
    ComPtr<ID3D12CommandQueue>   m_queue;
    ComPtr<ID3D12Fence>          m_internalFence;
    uint64_t                     m_internalValue = 0;
    HANDLE                       m_internalEvent = nullptr;
    ComPtr<ID3D12Fence>          m_fenceGame, m_fenceBridge;
    ComPtr<ID3D12RootSignature>  m_rootSig;
    ComPtr<ID3D12PipelineState>  m_tracePso, m_sharcUpdatePso, m_sharcResolvePso, m_volRestirPso, m_froxelPso, m_airIntegratePso, m_compositePso, m_exposurePso;
    ComPtr<ID3D12Resource>       m_exposureBuf, m_restirReservoirs, m_restirSurfaces, m_zeroBuf;
    bool                         m_resetExposure = false;
    UINT                         m_restirRowPitch = 0, m_restirArrayPitch = 0;
    float                        m_aoReach = 1.5f, m_aoStrength = 1.0f;
    float                        m_volDensity = 0.01f, m_volDensityOut = 0.0f, m_volLocal = 0.08f, m_volAmbient = 0.03f;
    float                        m_jitterOffset[2] = {}, m_rrJitter = 1.0f, m_rrJitterSign = -1.0f;
    float                        m_restirPrevKey[160][3] = {};
    uint32_t                     m_restirPrevLamps = 0, m_restirPrevWindows = 0;
    float                        m_restirOn = 2.0f, m_restirSamples = 4.0f, m_restirHistory = 4.0f, m_restirRadius = 16.0f, m_restirNeighbors = 1.0f, m_restirPermute = 1.0f;
    ComPtr<ID3D12Resource>       m_probeBuf;

    bool                         m_nrWanted = false, m_nrFailed = false;
    float                        m_nrIntensity = 1.0f, m_nrTone = 1.0f, m_nrStructure = 1.0f;
    std::string                  m_nrStyle = "natural";
    HMODULE                      m_nrDll = nullptr;
    void*                        m_nrHandle = nullptr;
    void*                      (*m_nrCreate)(uint32_t, uint32_t, const char*) = nullptr;
    int                        (*m_nrProcess)(void*, const void*, void*, uint32_t) = nullptr;
    void                       (*m_nrDestroy)(void*) = nullptr;
    const char*                (*m_nrError)() = nullptr;
    uint32_t                     m_nrW = 0, m_nrH = 0;
    UINT64                       m_nrBytes = 0;
    ComPtr<ID3D12Resource>       m_nrRead, m_nrUp;
    ComPtr<ID3D12CommandAllocator>    m_nrAlloc;
    ComPtr<ID3D12GraphicsCommandList> m_nrList;
    std::vector<uint8_t>         m_nrIn, m_nrOut;
    double                       m_nrMs = 0;

    bool NeuralFail(const char* why) { Log("DLSS 5 neural pass off: %s", why); m_nrFailed = true; return false; }

    void NeuralRun(ID3D12Resource* tex, bool upload, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& fp)
    {
        m_nrAlloc->Reset();
        m_nrList->Reset(m_nrAlloc.Get(), nullptr);
        D3D12_RESOURCE_BARRIER bar = {};
        bar.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        bar.Transition.pResource = tex;
        bar.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        bar.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        bar.Transition.StateAfter = upload ? D3D12_RESOURCE_STATE_COPY_DEST : D3D12_RESOURCE_STATE_COPY_SOURCE;
        m_nrList->ResourceBarrier(1, &bar);
        D3D12_TEXTURE_COPY_LOCATION tl = {}, bl = {};
        tl.pResource = tex; tl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        bl.pResource = upload ? m_nrUp.Get() : m_nrRead.Get(); bl.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; bl.PlacedFootprint = fp;
        if (upload) m_nrList->CopyTextureRegion(&tl, 0, 0, 0, &bl, nullptr); else m_nrList->CopyTextureRegion(&bl, 0, 0, 0, &tl, nullptr);
        std::swap(bar.Transition.StateBefore, bar.Transition.StateAfter);
        m_nrList->ResourceBarrier(1, &bar);
        m_nrList->Close();
        ID3D12CommandList* ls[] = {m_nrList.Get()};
        m_queue->ExecuteCommandLists(1, ls);
        const UINT64 v = ++m_internalValue;
        m_queue->Signal(m_internalFence.Get(), v);
        if (upload) return;
        m_internalFence->SetEventOnCompletion(v, m_internalEvent);
        WaitForSingleObject(m_internalEvent, 5000);
    }

    bool NeuralPass(ID3D12Resource* out)
    {
        if (m_nrFailed || !out) return false;
        const D3D12_RESOURCE_DESC d = out->GetDesc();
        if (d.Format != DXGI_FORMAT_R16G16B16A16_FLOAT) return NeuralFail("the output is not RGBA16F");
        const LARGE_INTEGER t0 = [] { LARGE_INTEGER q; QueryPerformanceCounter(&q); return q; }();
        if (!m_nrDll) {
            m_nrDll = LoadLibraryExW((g_dir + L"te_dlss5_native.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
            if (!m_nrDll) return NeuralFail("te_dlss5_native.dll is not in the bridge folder");
            m_nrCreate  = reinterpret_cast<decltype(m_nrCreate)>(GetProcAddress(m_nrDll, "te_nr_create"));
            m_nrProcess = reinterpret_cast<decltype(m_nrProcess)>(GetProcAddress(m_nrDll, "te_nr_process_rgba"));
            m_nrDestroy = reinterpret_cast<decltype(m_nrDestroy)>(GetProcAddress(m_nrDll, "te_nr_destroy"));
            m_nrError   = reinterpret_cast<decltype(m_nrError)>(GetProcAddress(m_nrDll, "te_nr_last_error"));
            if (!m_nrCreate || !m_nrProcess) return NeuralFail("te_dlss5_native.dll has no te_nr_create / te_nr_process_rgba");
            if (FAILED(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_nrAlloc))) ||
                FAILED(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_nrAlloc.Get(), nullptr, IID_PPV_ARGS(&m_nrList)))) return NeuralFail("command list");
            m_nrList->Close();
        }
        const uint32_t W = static_cast<uint32_t>(d.Width), H = d.Height;
        if (!m_nrHandle || W != m_nrW || H != m_nrH) {
            if (m_nrHandle && m_nrDestroy) m_nrDestroy(m_nrHandle);
            char dir[MAX_PATH * 3] = {};
            WideCharToMultiByte(CP_UTF8, 0, g_dir.c_str(), -1, dir, sizeof(dir), nullptr, nullptr);
            std::string esc;
            for (const char* c = dir; *c; ++c) { if (*c == '\\' || *c == '\"') esc += '\\'; esc += *c; }
            char json[2048];
            snprintf(json, sizeof(json), "{\"runtimeDir\":\"%s\",\"style\":\"%s\",\"intensity\":%g,\"localToneStrength\":%g,\"localStructureStrength\":%g,\"guidance\":\"zero\",\"depthInterval\":1}",
                     esc.c_str(), m_nrStyle.c_str(), m_nrIntensity, m_nrTone, m_nrStructure);
            m_nrHandle = m_nrCreate(W, H, json);
            if (!m_nrHandle) return NeuralFail(m_nrError ? m_nrError() : "te_nr_create failed");
            m_nrW = W; m_nrH = H;
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp0; UINT rows0; UINT64 rb0, total0;
            m_device->GetCopyableFootprints(&d, 0, 1, 0, &fp0, &rows0, &rb0, &total0);
            m_nrBytes = total0;
            m_nrRead = CreateBuffer(m_device.Get(), total0, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
            m_nrUp   = CreateBuffer(m_device.Get(), total0, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
            m_nrIn.assign(static_cast<size_t>(W) * H * 4, 0);
            m_nrOut.assign(static_cast<size_t>(W) * H * 4, 0);
            if (!m_nrRead || !m_nrUp) return NeuralFail("buffers");
            Log("DLSS 5 neural pass on: %ux%u, style %s, intensity %g, tone %g, structure %g", W, H, m_nrStyle.c_str(), m_nrIntensity, m_nrTone, m_nrStructure);
        }
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp; UINT rows; UINT64 rowBytes, total;
        m_device->GetCopyableFootprints(&d, 0, 1, 0, &fp, &rows, &rowBytes, &total);
        NeuralRun(out, false, fp);
        uint8_t* rd = nullptr; const D3D12_RANGE all = {0, static_cast<SIZE_T>(total)};
        if (FAILED(m_nrRead->Map(0, &all, reinterpret_cast<void**>(&rd)))) return NeuralFail("readback map");
        const __m128 k255 = _mm_set1_ps(255.0f), half = _mm_set1_ps(0.5f), zero = _mm_setzero_ps();
        for (uint32_t y = 0; y < H; ++y) {
            const uint16_t* s = reinterpret_cast<const uint16_t*>(rd + static_cast<size_t>(y) * fp.Footprint.RowPitch);
            uint8_t* o = &m_nrIn[static_cast<size_t>(y) * W * 4];
            for (uint32_t x = 0; x < W; ++x) {
                __m128 v = _mm_cvtph_ps(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(s + x * 4)));
                v = _mm_add_ps(_mm_mul_ps(_mm_min_ps(_mm_max_ps(v, zero), _mm_set1_ps(1.0f)), k255), half);
                const __m128i i = _mm_cvttps_epi32(v);
                const __m128i p = _mm_packus_epi16(_mm_packs_epi32(i, i), _mm_setzero_si128());
                *reinterpret_cast<uint32_t*>(o + x * 4) = static_cast<uint32_t>(_mm_cvtsi128_si32(p)) | 0xFF000000u;
            }
        }
        const D3D12_RANGE none = {0, 0};
        m_nrRead->Unmap(0, &none);
        if (m_nrProcess(m_nrHandle, m_nrIn.data(), m_nrOut.data(), W * H * 4) != 0) return NeuralFail(m_nrError ? m_nrError() : "te_nr_process_rgba failed");
        uint8_t* up = nullptr;
        if (FAILED(m_nrUp->Map(0, &none, reinterpret_cast<void**>(&up)))) return NeuralFail("upload map");
        const __m128 inv = _mm_set1_ps(1.0f / 255.0f);
        for (uint32_t y = 0; y < H; ++y) {
            uint16_t* dst = reinterpret_cast<uint16_t*>(up + static_cast<size_t>(y) * fp.Footprint.RowPitch);
            const uint8_t* s = &m_nrOut[static_cast<size_t>(y) * W * 4];
            for (uint32_t x = 0; x < W; ++x) {
                const __m128i b = _mm_cvtsi32_si128(*reinterpret_cast<const int*>(s + x * 4));
                const __m128 f = _mm_mul_ps(_mm_cvtepi32_ps(_mm_cvtepu8_epi32(b)), inv);
                _mm_storel_epi64(reinterpret_cast<__m128i*>(dst + x * 4), _mm_cvtps_ph(f, 0));
            }
        }
        const D3D12_RANGE wrote = {0, static_cast<SIZE_T>(total)};
        m_nrUp->Unmap(0, &wrote);
        NeuralRun(out, true, fp);
        LARGE_INTEGER t1, fq; QueryPerformanceCounter(&t1); QueryPerformanceFrequency(&fq);
        m_nrMs = m_nrMs * 0.9 + 0.1 * (t1.QuadPart - t0.QuadPart) * 1000.0 / fq.QuadPart;
        static uint32_t s_n = 0;
        if (++s_n % 300 == 0) Log("DLSS 5 neural pass: %.1f ms per frame", m_nrMs);
        return true;
    }
    ComPtr<ID3D12Resource>       m_cb;
    uint8_t*                     m_cbMapped = nullptr;
    nrd::Integration             m_nrd;
    bool                         m_historyReset = true;
    float                        m_prevViewProj[4][4] = {};
    Camera                       m_prevCamera = {};
    float                        m_prevCameraPos[3] = {};
    bool                         m_havePrev = false;
    ComPtr<ID3D12Resource>       m_rrColor, m_rrDiff, m_rrSpec, m_rrNormal, m_rrMv, m_rrOut;
    ComPtr<ID3D12PipelineState>  m_noisyPso;
    bool                         m_rrAlbedo = false, m_nrdReady = false, m_rrReady = false, m_rrWanted = true, m_rrNow = false, m_rrFailed = false, m_rrKey = false;
    NVSDK_NGX_Parameter*         m_ngxParams = nullptr;
    NVSDK_NGX_Handle*            m_rrHandle = nullptr;
    uint32_t                     m_rrW = 0, m_rrH = 0;
    ComPtr<ID3D12Resource>       m_airLocal[2], m_airTrans, m_volRes[2];
    ComPtr<ID3D12Resource>       m_airGrid, m_airLit[2], m_sharcBuf[3], m_volumetric, m_viewZ, m_normalRough, m_motion, m_penumbra, m_diffIn, m_specIn, m_sigmaOut, m_diffOut, m_specOut;
    ComPtr<ID3D12DescriptorHeap> m_heap;
    UINT                         m_heapInc = 0;

    uint32_t                     m_generation = 0;
    ComPtr<ID3D12Resource>       m_depth[2], m_output[2], m_opaque[2], m_gNormal[2], m_gAlbedo[2], m_color[2];
    uint32_t                     m_width = 0, m_height = 0;

    std::unordered_map<uint32_t, Mesh> m_meshes;
    std::vector<Pending>         m_pending;
    std::vector<PendingTexture>  m_pendingTextures;
    std::unordered_map<uint32_t, int32_t> m_textureSlots;
    std::vector<ComPtr<ID3D12Resource>> m_textures;
    std::vector<uint32_t>        m_freeAttrs;
    std::deque<FreedAttr>        m_freedAttrs;
    ComPtr<ID3D12Resource>       m_dynUpload[3], m_dynBlas[3], m_dynScratch[3], m_records[3];
    uint8_t*                     m_dynMapped[3] = {};
    GeomRecord*                  m_recordsMapped[3] = {};
    UINT64                       m_dynBlasSize[3] = {}, m_dynScratchSize[3] = {};
    std::deque<Retired>          m_retired;
    FrameList                    m_lists[3];
    uint32_t                     m_listIndex = 0;

    ComPtr<ID3D12Resource>       m_tlas, m_tlasScratch, m_instances[3];
    uint32_t                     m_instanceRing = 0;
    ComPtr<ID3D12QueryHeap>      m_timestamps;
    ComPtr<ID3D12Resource>       m_timestampReadback;
    uint64_t                     m_timestampFreq = 0;
    static constexpr UINT        kGpuMarks = 8;
    double                       m_gpuSum[kGpuMarks] = {}, m_gpuTotal = 0.0, m_gpuWorst = 0.0, m_gpuWorstParts[kGpuMarks] = {};
    uint32_t                     m_gpuFrames = 0, m_paceOver40 = 0, m_paceOver100 = 0;
    volatile double              m_telGpu[kGpuMarks] = {}, m_telGpuMax = 0.0, m_telSlotMs = 0.0, m_telSlotMax = 0.0, m_telListWaitMs = 0.0;
    volatile uint32_t            m_telFrames = 0, m_telSlots = 0, m_telInstances = 0, m_telLights = 0;
    static DWORD WINAPI          TelemetryThread(void* self);
    void                         TelemetryLoop();
    uint64_t                     m_paceLast = 0, m_paceWorst = 0, m_paceStart = 0;
    uint64_t                     m_framesDone = 0;
};

bool Bridge::Init(const std::wstring& mappingName)
{
    m_mappingName = mappingName;
    m_mapping     = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, mappingName.c_str());
    if (!m_mapping) { Log("FATAL: OpenFileMapping failed (%lu)", GetLastError()); return false; }
    m_view = static_cast<uint8_t*>(MapViewOfFile(m_mapping, FILE_MAP_ALL_ACCESS, 0, 0, 0));
    if (!m_view) { Log("FATAL: MapViewOfFile failed (%lu)", GetLastError()); return false; }
    g_header = reinterpret_cast<Header*>(m_view);
    Header& h = *g_header;
    if (h.magic != kMagic || h.version != kProtocolVersion || h.headerBytes != sizeof(Header)) {
        Log("FATAL: protocol mismatch (magic %08x version %u header %u, expected %u)", h.magic, h.version, h.headerBytes,
            static_cast<unsigned>(sizeof(Header)));
        return false;
    }
    m_gameProcess = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, h.gamePid);
    m_frameEvent  = OpenEventW(SYNCHRONIZE, FALSE, Name(L"frame").c_str());
    m_slotFreeEvent = OpenEventW(EVENT_MODIFY_STATE, FALSE, Name(L"slotfree").c_str());
    if (!m_frameEvent || !m_slotFreeEvent) { Fail("cannot open events (%lu)", GetLastError()); return false; }

    ComPtr<IDXGIFactory4> factory;
    CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
    LUID luid = {h.adapterLuidLow, h.adapterLuidHigh};
    ComPtr<IDXGIAdapter1> adapter;
    if (!factory || FAILED(factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter)))) { Fail("game adapter not found"); return false; }
    if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&m_device)))) {
        Fail("D3D12CreateDevice (FL 12_0, ID3D12Device5) failed");
        return false;
    }
    DXGI_ADAPTER_DESC1 ad;
    adapter->GetDesc1(&ad);

    D3D12_FEATURE_DATA_D3D12_OPTIONS5 o5 = {};
    m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &o5, sizeof(o5));
    h.bridgeRaytracingTier = o5.RaytracingTier;
    if (o5.RaytracingTier < D3D12_RAYTRACING_TIER_1_1) {
        Fail("GPU needs DXR tier 1.1 (inline ray queries); reported tier %d", o5.RaytracingTier);
        return false;
    }
    Log("Device: %ls, DXR tier %d.%d", ad.Description, o5.RaytracingTier / 10, o5.RaytracingTier % 10);
    {
        const std::wstring ini = g_dir + L"..\\..\\BullyDX12.ini";
        m_rrWanted = GetPrivateProfileIntW(L"RTOptions", L"Denoiser", 1, ini.c_str()) != 0;
        m_rrAlbedo = GetPrivateProfileIntW(L"RTOptions", L"RRAlbedo", 0, ini.c_str()) != 0;
        m_nrWanted = GetPrivateProfileIntW(L"RTOptions", L"Neural", 0, ini.c_str()) != 0;
        {
            wchar_t buf[64] = {};
            const auto num = [&](const wchar_t* key, float def) { GetPrivateProfileStringW(L"RTOptions", key, L"", buf, 64, ini.c_str()); return buf[0] ? static_cast<float>(_wtof(buf)) : def; };
            m_nrIntensity = num(L"NeuralIntensity", 1.0f); m_nrTone = num(L"NeuralTone", 1.0f); m_nrStructure = num(L"NeuralStructure", 1.0f);
            GetPrivateProfileStringW(L"RTOptions", L"NeuralStyle", L"natural", buf, 64, ini.c_str());
            m_nrStyle.clear(); for (const wchar_t* c = buf; *c; ++c) m_nrStyle += static_cast<char>(*c);
        }
        const wchar_t* paths[1] = {g_dir.c_str()};
        NVSDK_NGX_FeatureCommonInfo fi = {};
        fi.PathListInfo.Path   = paths;
        fi.PathListInfo.Length = 1;
        const NVSDK_NGX_Result ir = NVSDK_NGX_D3D12_Init_with_ProjectID("6c1f7a3e-4b2d-4e8a-9f10-b0115d3a7c21", NVSDK_NGX_ENGINE_TYPE_CUSTOM, "1.0",
                                                                         g_dir.c_str(), m_device.Get(), &fi);
        int avail = 0, needsDriver = 0;
        if (NVSDK_NGX_SUCCEED(ir) && NVSDK_NGX_SUCCEED(NVSDK_NGX_D3D12_GetCapabilityParameters(&m_ngxParams)) && m_ngxParams) {
            NVSDK_NGX_Parameter_GetI(m_ngxParams, NVSDK_NGX_Parameter_SuperSamplingDenoising_Available, &avail);
            NVSDK_NGX_Parameter_GetI(m_ngxParams, NVSDK_NGX_Parameter_SuperSamplingDenoising_NeedsUpdatedDriver, &needsDriver);
        }
        m_rrReady = avail != 0 && !getenv("BULLYRT_NO_RR");
        m_rrNow   = m_rrReady && m_rrWanted;
        Log("DLSS Ray Reconstruction: init 0x%08x, available %d, needs newer driver %d -> denoiser %s", static_cast<unsigned>(ir), avail, needsDriver,
            m_rrNow ? "NVIDIA DLSS Ray Reconstruction" : "NVIDIA NRD");
    }

    D3D12_COMMAND_QUEUE_DESC qd = {};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(m_device->CreateCommandQueue(&qd, IID_PPV_ARGS(&m_queue)))) { Fail("CreateCommandQueue failed"); return false; }
    m_queue->GetTimestampFrequency(&m_timestampFreq);
    m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_internalFence));
    m_internalEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    auto openFence = [&](const wchar_t* what, ComPtr<ID3D12Fence>& out) {
        HANDLE hnd = nullptr;
        if (FAILED(m_device->OpenSharedHandleByName(Name(what).c_str(), GENERIC_ALL, &hnd))) return false;
        const HRESULT hr = m_device->OpenSharedHandle(hnd, IID_PPV_ARGS(&out));
        CloseHandle(hnd);
        return SUCCEEDED(hr);
    };
    if (!openFence(L"fenceGame", m_fenceGame) || !openFence(L"fenceBridge", m_fenceBridge)) {
        Fail("cannot open shared fences");
        return false;
    }

    D3D12_DESCRIPTOR_RANGE ranges[7]            = {};
    ranges[0].RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors                    = 7;
    ranges[0].BaseShaderRegister                = 1;
    ranges[0].OffsetInDescriptorsFromTableStart = 0;
    ranges[1].RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors                    = 8;
    ranges[1].BaseShaderRegister                = 0;
    ranges[1].OffsetInDescriptorsFromTableStart = 7;
    ranges[2].RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[2].NumDescriptors                    = 1;
    ranges[2].BaseShaderRegister                = 9;
    ranges[2].OffsetInDescriptorsFromTableStart = 15;
    D3D12_DESCRIPTOR_RANGE texRange = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, UINT_MAX, 0, 1, 0};
    D3D12_DESCRIPTOR_RANGE attrRange = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, UINT_MAX, 0, 2, 0};
    D3D12_ROOT_PARAMETER params[9]                = {};
    params[0].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister           = 0;
    params[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[1].Descriptor.ShaderRegister           = 0;
    params[2].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    ranges[3].RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[3].NumDescriptors                    = 5;
    ranges[3].BaseShaderRegister                = 9;
    ranges[3].OffsetInDescriptorsFromTableStart = 16;
    ranges[4].RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[4].NumDescriptors                    = 1;
    ranges[4].BaseShaderRegister                = 10;
    ranges[4].OffsetInDescriptorsFromTableStart = 21;
    ranges[5].RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[5].NumDescriptors                    = 6;
    ranges[5].BaseShaderRegister                = 14;
    ranges[5].OffsetInDescriptorsFromTableStart = 22;
    ranges[6].RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[6].NumDescriptors                    = 5;
    ranges[6].BaseShaderRegister                = 22;
    ranges[6].OffsetInDescriptorsFromTableStart = 28;
    params[2].DescriptorTable.NumDescriptorRanges = 7;
    params[2].DescriptorTable.pDescriptorRanges   = ranges;
    params[3].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[3].DescriptorTable.NumDescriptorRanges = 1;
    params[3].DescriptorTable.pDescriptorRanges   = &texRange;
    params[4].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[4].DescriptorTable.NumDescriptorRanges = 1;
    params[4].DescriptorTable.pDescriptorRanges   = &attrRange;
    params[5].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[5].Descriptor.ShaderRegister           = 8;
    params[6].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_UAV;
    params[6].Descriptor.ShaderRegister           = 8;
    params[7].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_UAV;
    params[7].Descriptor.ShaderRegister           = 20;
    params[8].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_UAV;
    params[8].Descriptor.ShaderRegister           = 21;
    D3D12_STATIC_SAMPLER_DESC smp = {};
    smp.Filter   = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    smp.AddressU = smp.AddressV = smp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    smp.MaxLOD   = D3D12_FLOAT32_MAX;
    D3D12_ROOT_SIGNATURE_DESC rs = {9, params, 1, &smp, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> blob, err;
    if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err)) ||
        FAILED(m_device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&m_rootSig)))) {
        Fail("root signature failed");
        return false;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature                    = m_rootSig.Get();
    pd.CS                                = {g_RTX_CSTrace, sizeof(g_RTX_CSTrace)};
    if (FAILED(m_device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&m_tracePso)))) { Fail("trace PSO failed"); return false; }
    pd.CS = {g_RTX_CSComposite, sizeof(g_RTX_CSComposite)};
    if (FAILED(m_device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&m_compositePso)))) { Fail("composite PSO failed"); return false; }
    pd.CS = {g_RTX_CSSharcUpdate, sizeof(g_RTX_CSSharcUpdate)};
    if (FAILED(m_device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&m_sharcUpdatePso)))) { Fail("SHaRC update PSO failed"); return false; }
    pd.CS = {g_RTX_CSSharcResolve, sizeof(g_RTX_CSSharcResolve)};
    if (FAILED(m_device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&m_sharcResolvePso)))) { Fail("SHaRC resolve PSO failed"); return false; }
    pd.CS = {g_RTX_CSVolRestir, sizeof(g_RTX_CSVolRestir)};
    if (FAILED(m_device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&m_volRestirPso)))) { Fail("volume ReSTIR PSO failed"); return false; }
    pd.CS = {g_RTX_CSFroxel, sizeof(g_RTX_CSFroxel)};
    if (FAILED(m_device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&m_froxelPso)))) { Fail("air PSO failed"); return false; }
    pd.CS = {g_RTX_CSAirIntegrate, sizeof(g_RTX_CSAirIntegrate)};
    if (FAILED(m_device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&m_airIntegratePso)))) { Fail("air integrate PSO failed"); return false; }
    pd.CS = {g_RTX_CSExposure, sizeof(g_RTX_CSExposure)};
    if (FAILED(m_device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&m_exposurePso)))) { Fail("exposure PSO failed"); return false; }
    pd.CS = {g_RTX_CSNoisy, sizeof(g_RTX_CSNoisy)};
    if (FAILED(m_device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&m_noisyPso)))) { Fail("noisy PSO failed"); return false; }
    m_exposureBuf = CreateBuffer(m_device.Get(), 1024, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (!m_exposureBuf) { Fail("exposure buffer failed"); return false; }
    m_zeroBuf = CreateBuffer(m_device.Get(), 1024, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
    if (m_zeroBuf) { void* z = nullptr; if (SUCCEEDED(m_zeroBuf->Map(0, nullptr, &z)) && z) { memset(z, 0, 1024); m_zeroBuf->Unmap(0, nullptr); } }

    D3D12_DESCRIPTOR_HEAP_DESC hd = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kAttrBase + kMaxAttrs, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    m_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_heap));
    m_heapInc = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    m_cb = CreateBuffer(m_device.Get(), kFrameCbStride * 3, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
    if (!m_cb || FAILED(m_cb->Map(0, nullptr, reinterpret_cast<void**>(&m_cbMapped)))) { Fail("constant buffer failed"); return false; }

    {
        D3D12_SHADER_RESOURCE_VIEW_DESC nt = {};
        nt.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        nt.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        nt.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        nt.Texture2D.MipLevels = 1;
        for (UINT i = 0; i < kMaxTextures; ++i) m_device->CreateShaderResourceView(nullptr, &nt, HeapCpu(kTexBase + i));
        D3D12_SHADER_RESOURCE_VIEW_DESC nb = {};
        nb.Format = DXGI_FORMAT_R32_TYPELESS;
        nb.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        nb.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        nb.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
        for (UINT i = 0; i < kMaxAttrs; ++i) m_device->CreateShaderResourceView(nullptr, &nb, HeapCpu(kAttrBase + i));
    }
    for (UINT i = 0; i < 3; ++i) {
        m_dynUpload[i] = CreateBuffer(m_device.Get(), kMaxDynamicBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
        m_records[i]   = CreateBuffer(m_device.Get(), sizeof(GeomRecord) * (kMaxInstances + kMaxDynamicGeoms), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
        if (!m_dynUpload[i] || !m_records[i] || FAILED(m_dynUpload[i]->Map(0, nullptr, reinterpret_cast<void**>(&m_dynMapped[i]))) ||
            FAILED(m_records[i]->Map(0, nullptr, reinterpret_cast<void**>(&m_recordsMapped[i])))) {
            Fail("character buffers failed");
            return false;
        }
        CreateRawSrv(m_dynUpload[i].Get(), kMaxDynamicBytes, i);
    }
    for (UINT i = kMaxAttrs; i-- > 3;) m_freeAttrs.push_back(i);

    for (auto& l : m_lists) {
        m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&l.alloc));
        m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, l.alloc.Get(), nullptr, IID_PPV_ARGS(&l.list));
        l.list->Close();
    }

    D3D12_QUERY_HEAP_DESC qh = {D3D12_QUERY_HEAP_TYPE_TIMESTAMP, kGpuMarks, 0};
    m_device->CreateQueryHeap(&qh, IID_PPV_ARGS(&m_timestamps));
    m_timestampReadback = CreateBuffer(m_device.Get(), kGpuMarks * 8 * 3, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE,
                                       D3D12_RESOURCE_STATE_COPY_DEST);

    if (!EnsureTlas()) return false;

    strncpy_s(h.bridgeMessage, "running", _TRUNCATE);
    MemoryBarrier();
    h.bridgeState = 1;
    Log("Bridge running");
    return true;
}

void Bridge::CreateRawSrv(ID3D12Resource* buffer, UINT64 bytes, UINT attrIndex)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC v = {};
    v.Format                  = DXGI_FORMAT_R32_TYPELESS;
    v.ViewDimension           = D3D12_SRV_DIMENSION_BUFFER;
    v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    v.Buffer.NumElements      = static_cast<UINT>(bytes / 4);
    v.Buffer.Flags            = D3D12_BUFFER_SRV_FLAG_RAW;
    m_device->CreateShaderResourceView(buffer, &v, HeapCpu(kAttrBase + attrIndex));
}

bool Bridge::EnsureTlas()
{
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS in = {};
    in.Type        = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    in.Flags       = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
    in.NumDescs    = kMaxInstances + 1;
    in.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO pre = {};
    m_device->GetRaytracingAccelerationStructurePrebuildInfo(&in, &pre);
    m_tlas        = CreateBuffer(m_device.Get(), pre.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
                                 D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
    m_tlasScratch = CreateBuffer(m_device.Get(), pre.ScratchDataSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
                                 D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    bool ok = m_tlas && m_tlasScratch;
    for (auto& b : m_instances) {
        b  = CreateBuffer(m_device.Get(), sizeof(D3D12_RAYTRACING_INSTANCE_DESC) * (kMaxInstances + 1), D3D12_HEAP_TYPE_UPLOAD,
                          D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
        ok = ok && b;
    }
    if (!ok) Fail("TLAS allocation failed");
    return ok;
}

bool Bridge::OpenSharedResources()
{
    Header& h = *g_header;
    const uint32_t gen = h.resourceGeneration;
    if (gen == 0 || gen == m_generation) return gen != 0;

    m_internalValue++;
    m_queue->Signal(m_internalFence.Get(), m_internalValue);
    m_internalFence->SetEventOnCompletion(m_internalValue, m_internalEvent);
    WaitForSingleObject(m_internalEvent, 5000);

    ComPtr<ID3D12Resource> depth[2], output[2], opaque[2], gNormal[2], gAlbedo[2], color[2];
    for (int i = 0; i < 2; ++i) {
        wchar_t what[64];
        auto open = [&](const wchar_t* fmt, ComPtr<ID3D12Resource>& out) {
            swprintf_s(what, fmt, gen, i);
            HANDLE hnd = nullptr;
            if (FAILED(m_device->OpenSharedHandleByName(Name(what).c_str(), GENERIC_ALL, &hnd))) return false;
            const HRESULT hr = m_device->OpenSharedHandle(hnd, IID_PPV_ARGS(&out));
            CloseHandle(hnd);
            return SUCCEEDED(hr);
        };
        if (!open(L"g%u_depth%d", depth[i]) || !open(L"g%u_output%d", output[i]) || !open(L"g%u_opaque%d", opaque[i]) || !open(L"g%u_normal%d", gNormal[i]) ||
            !open(L"g%u_albedo%d", gAlbedo[i]) || !open(L"g%u_color%d", color[i])) {
            Log("Shared textures of generation %u not available yet", gen);
            return false;
        }
    }

    if (!m_settingsRead) ReadSettings();
    const UINT w = (h.resourceWidth + m_rtScale - 1) / m_rtScale, hh = (h.resourceHeight + m_rtScale - 1) / m_rtScale;
    {
        const UINT blocksX = (w + 15) / 16, blocksY = (hh + 15) / 16;
        m_restirRowPitch = blocksX * 256;
        m_restirArrayPitch = m_restirRowPitch * blocksY;
        Retire(std::move(m_restirReservoirs));
        Retire(std::move(m_restirSurfaces));
        m_restirReservoirs = CreateBuffer(m_device.Get(), static_cast<UINT64>(m_restirArrayPitch) * 2 * 32, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        m_restirSurfaces = CreateBuffer(m_device.Get(), static_cast<UINT64>(w) * hh * 2 * 32, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        if (!m_restirReservoirs || !m_restirSurfaces) { Fail("RTXDI buffers failed"); return false; }
    }
    auto local = [&](DXGI_FORMAT fmt, ComPtr<ID3D12Resource>& out) {
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type                  = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd   = {};
        rd.Dimension             = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width                 = w;
        rd.Height                = hh;
        rd.DepthOrArraySize      = 1;
        rd.MipLevels             = 1;
        rd.Format                = fmt;
        rd.SampleDesc.Count      = 1;
        rd.Flags                 = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        out.Reset();
        return SUCCEEDED(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                           nullptr, IID_PPV_ARGS(&out)));
    };
    auto fullRes = [&](DXGI_FORMAT fmt, ComPtr<ID3D12Resource>& out) {
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = h.resourceWidth; rd.Height = h.resourceHeight;
        rd.DepthOrArraySize = 1; rd.MipLevels = 1; rd.Format = fmt; rd.SampleDesc.Count = 1;
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        out.Reset();
        return SUCCEEDED(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&out)));
    };
    if (!local(DXGI_FORMAT_R16G16B16A16_FLOAT, m_rrColor) || !local(DXGI_FORMAT_R16G16B16A16_FLOAT, m_rrDiff) || !local(DXGI_FORMAT_R16G16B16A16_FLOAT, m_rrSpec) ||
        !local(DXGI_FORMAT_R16G16B16A16_FLOAT, m_rrNormal) || !local(DXGI_FORMAT_R16G16_FLOAT, m_rrMv) || !fullRes(DXGI_FORMAT_R16G16B16A16_FLOAT, m_rrOut)) {
        Fail("Ray Reconstruction target allocation failed");
        return false;
    }
    {
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type                  = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd   = {};
        rd.Dimension             = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
        rd.Width                 = 160;
        rd.Height                = 90;
        rd.DepthOrArraySize      = 48;
        rd.MipLevels             = 1;
        rd.Format                = DXGI_FORMAT_R16G16B16A16_FLOAT;
        rd.SampleDesc.Count      = 1;
        rd.Flags                 = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if (FAILED(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&m_airGrid)))) { Fail("air grid failed"); return false; }
        for (auto& lit : m_airLit)
            if (FAILED(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&lit)))) { Fail("air history failed"); return false; }
        for (auto& loc : m_airLocal)
            if (FAILED(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&loc)))) { Fail("air lamps failed"); return false; }
        if (FAILED(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&m_airTrans)))) { Fail("air transmittance failed"); return false; }
        D3D12_RESOURCE_DESC vr = rd;
        vr.Width = 80; vr.Height = 45; vr.DepthOrArraySize = 24;
        vr.Format = DXGI_FORMAT_R32G32B32A32_UINT;
        for (auto& res : m_volRes)
            if (FAILED(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &vr, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&res)))) { Fail("volume reservoirs failed"); return false; }
        D3D12_RESOURCE_DESC bd = {};
        bd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Height           = 1;
        bd.DepthOrArraySize = 1;
        bd.MipLevels        = 1;
        bd.Format           = DXGI_FORMAT_UNKNOWN;
        bd.SampleDesc.Count = 1;
        bd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        bd.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        const UINT64 sharcStride[3] = {8, 16, 16};
        for (int b = 0; b < 3; ++b) {
            bd.Width = sharcStride[b] * kSharcEntries;
            if (FAILED(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&m_sharcBuf[b])))) { Fail("SHaRC buffers failed"); return false; }
        }
    }
    if (!local(DXGI_FORMAT_R16G16B16A16_FLOAT, m_volumetric) || !local(DXGI_FORMAT_R32_FLOAT, m_viewZ) || !local(DXGI_FORMAT_R10G10B10A2_UNORM, m_normalRough) ||
        !local(DXGI_FORMAT_R16G16_FLOAT, m_motion) || !local(DXGI_FORMAT_R16_FLOAT, m_penumbra) ||
        !local(DXGI_FORMAT_R16G16B16A16_FLOAT, m_diffIn) || !local(DXGI_FORMAT_R16G16B16A16_FLOAT, m_specIn) ||
        !local(DXGI_FORMAT_R16_FLOAT, m_sigmaOut) || !local(DXGI_FORMAT_R16G16B16A16_FLOAT, m_diffOut) ||
        !local(DXGI_FORMAT_R16G16B16A16_FLOAT, m_specOut)) {
        Fail("RTX target allocation failed (%ux%u)", w, hh);
        return false;
    }

    for (int i = 0; i < 2; ++i) {
        m_depth[i]   = depth[i];
        m_output[i]  = output[i];
        m_opaque[i] = opaque[i];
        m_gNormal[i] = gNormal[i];
        m_gAlbedo[i] = gAlbedo[i];
        m_color[i]   = color[i];
        D3D12_CPU_DESCRIPTOR_HANDLE cpu = m_heap->GetCPUDescriptorHandleForHeapStart();
        cpu.ptr += static_cast<SIZE_T>(i * 33) * m_heapInc;
        auto srv = [&](ID3D12Resource* r) {
            D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
            sv.Format                  = r->GetDesc().Format;
            sv.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
            sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            sv.Texture2D.MipLevels     = 1;
            m_device->CreateShaderResourceView(r, &sv, cpu);
            cpu.ptr += m_heapInc;
        };
        auto uav = [&](ID3D12Resource* r) {
            D3D12_UNORDERED_ACCESS_VIEW_DESC uv = {};
            uv.Format        = r->GetDesc().Format;
            uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            if (r->GetDesc().Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D) {
                uv.ViewDimension   = D3D12_UAV_DIMENSION_TEXTURE3D;
                uv.Texture3D.WSize = r->GetDesc().DepthOrArraySize;
            }
            m_device->CreateUnorderedAccessView(r, nullptr, &uv, cpu);
            cpu.ptr += m_heapInc;
        };
        srv(m_depth[i].Get()); srv(m_gNormal[i].Get()); srv(m_gAlbedo[i].Get()); srv(m_color[i].Get());
        srv(m_sigmaOut.Get()); srv(m_diffOut.Get()); srv(m_specOut.Get());
        uav(m_viewZ.Get()); uav(m_normalRough.Get()); uav(m_penumbra.Get()); uav(m_diffIn.Get()); uav(m_specIn.Get());
        uav(m_output[i].Get()); uav(m_motion.Get()); uav(m_volumetric.Get()); srv(m_opaque[i].Get());
        uav(m_rrColor.Get()); uav(m_rrDiff.Get()); uav(m_rrSpec.Get()); uav(m_rrNormal.Get()); uav(m_rrMv.Get()); srv(m_rrOut.Get()); uav(m_airGrid.Get()); uav(m_airLit[0].Get()); uav(m_airLit[1].Get());
        for (int b = 0; b < 3; ++b) {
            D3D12_UNORDERED_ACCESS_VIEW_DESC sv = {};
            sv.Format                     = DXGI_FORMAT_UNKNOWN;
            sv.ViewDimension              = D3D12_UAV_DIMENSION_BUFFER;
            sv.Buffer.NumElements         = kSharcEntries;
            sv.Buffer.StructureByteStride = b == 0 ? 8 : 16;
            m_device->CreateUnorderedAccessView(m_sharcBuf[b].Get(), nullptr, &sv, cpu);
            cpu.ptr += m_heapInc;
        }
        uav(m_airLocal[0].Get()); uav(m_airLocal[1].Get()); uav(m_airTrans.Get()); uav(m_volRes[0].Get()); uav(m_volRes[1].Get());
    }

    const nrd::DenoiserDesc denoisers[] = {{kSigma, nrd::Denoiser::SIGMA_SHADOW}, {kReblur, nrd::Denoiser::REBLUR_DIFFUSE_SPECULAR}};
    nrd::InstanceCreationDesc inst = {};
    inst.denoisers    = denoisers;
    inst.denoisersNum = 2;
    nrd::IntegrationCreationDesc idesc = {};
    strncpy_s(idesc.name, "BullyRTX", _TRUNCATE);
    idesc.resourceWidth  = static_cast<uint16_t>(w);
    idesc.resourceHeight = static_cast<uint16_t>(hh);
    idesc.queuedFrameNum = 3;
    idesc.autoWaitForIdle = true;
    ID3D12CommandQueue* queues[] = {m_queue.Get()};
    nri::QueueFamilyD3D12Desc family = {};
    family.d3d12Queues = queues;
    family.queueNum    = 1;
    family.queueType   = nri::QueueType::GRAPHICS;
    nri::DeviceCreationD3D12Desc ddesc = {};
    ddesc.d3d12Device    = m_device.Get();
    ddesc.queueFamilies  = &family;
    ddesc.queueFamilyNum = 1;
    m_nrdReady = false;
    {
        if (m_nrd.RecreateD3D12(idesc, inst, ddesc) != nrd::Result::SUCCESS) {
            Fail("NRD integration creation failed");
            return false;
        }
        m_nrdReady = true;
        Log("NRD ready: SIGMA + REBLUR at %ux%u, %.1f MB; denoiser in use: %s", w, hh, m_nrd.GetTotalMemoryUsageInMb(), m_rrNow ? "DLSS Ray Reconstruction" : "NRD");
    }
    m_historyReset = true;

    m_width      = h.resourceWidth;
    m_height     = h.resourceHeight;
    m_generation = gen;
    MemoryBarrier();
    h.bridgeGeneration = gen;
    Log("Opened shared textures generation %u (%ux%u)", gen, m_width, m_height);
    return true;
}

void Bridge::CollectRetired()
{
    const uint64_t done = m_internalFence->GetCompletedValue();
    while (!m_retired.empty() && m_retired.front().fence <= done) m_retired.pop_front();
}

bool Bridge::ProcessSlot(FrameSlot& s)
{
    Header&        h    = *g_header;
    if (h.reserved0 != m_settingsGen) { m_settingsGen = h.reserved0; m_settingsRead = false; }
    const uint32_t slot = s.slotIndex & 1;
    const uint8_t* data = m_view + h.slotDataOffset[slot];

    for (uint32_t i = 0; i < std::min(s.meshOpCount, kMaxMeshOps); ++i) {
        const MeshOp& op = s.meshOps[i];
        if (op.type == kMeshDestroy) {
            auto it = m_meshes.find(op.meshId);
            if (it != m_meshes.end()) {
                Retire(std::move(it->second.blas));
                Retire(std::move(it->second.attr));
                if (it->second.attrIndex) m_freedAttrs.push_back({it->second.attrIndex, m_internalValue + 1});
                m_meshes.erase(it);
            }
            m_cpuMeshes.erase(op.meshId);
            continue;
        }
        if (op.type != kMeshCreate || op.vertexCount == 0 || op.indexCount < 3) continue;
        const uint64_t bytes = static_cast<uint64_t>(op.vertexCount) * 24 + static_cast<uint64_t>(op.indexCount) * 4;
        if (op.dataOffset + bytes > kSlotDataBytes) continue;
        ComPtr<ID3D12Resource> up = CreateBuffer(m_device.Get(), bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                                                 D3D12_RESOURCE_STATE_GENERIC_READ);
        void* dst = nullptr;
        if (!up || FAILED(up->Map(0, nullptr, &dst))) continue;
        memcpy(dst, data + op.dataOffset, static_cast<size_t>(bytes));
        up->Unmap(0, nullptr);
        {
            CpuMesh& cm = m_cpuMeshes[op.meshId];
            const float* src = reinterpret_cast<const float*>(data + op.dataOffset);
            cm.pos.assign(src, src + op.vertexCount * 3);
            cm.uv.assign(src + op.vertexCount * 3, src + op.vertexCount * 5);
            const uint32_t* vc = reinterpret_cast<const uint32_t*>(src + op.vertexCount * 5);
            cm.col.assign(vc, vc + op.vertexCount);
            const uint32_t* ix = reinterpret_cast<const uint32_t*>(data + op.dataOffset + static_cast<uint64_t>(op.vertexCount) * 24);
            cm.idx.assign(ix, ix + op.indexCount);
        }
        m_pending.push_back({op.meshId, up, op.vertexCount, op.indexCount, bytes});
    }

    for (uint32_t i = 0; i < std::min(s.texOpCount, kMaxTexOps); ++i) {
        const TexOp& op = s.texOps[i];
        if (op.dataOffset + static_cast<uint64_t>(op.rowBytes) * op.rows <= kSlotDataBytes)
        {
            CpuTex& ct = m_cpuTex[op.id];
            DecodeTexture(op, data + op.dataOffset, ct);
            if (!m_materialsLoaded) LoadMaterials();
            uint32_t hs = 2166136261u;
            for (uint8_t byte : ct.rgba) hs = (hs ^ byte) * 16777619u;
            {
                uint8_t maxAlpha = 0;
                for (size_t ai = 3; ai < ct.rgba.size(); ai += 4) maxAlpha = std::max(maxAlpha, ct.rgba[ai]);
                ct.veil = !ct.rgba.empty() && maxAlpha < 100;
            }
            ct.hash = hs;
            auto mat = m_materials.find(hs);
            if (mat != m_materials.end()) {
                ct.emissive = mat->second.emissive; ct.lampPower = mat->second.lampPower;
                memcpy(ct.lampCol, mat->second.col, sizeof(ct.lampCol));
                ct.glass = mat->second.glass == 1;
                ct.mirror = mat->second.glass == 2;
                ct.night = mat->second.night != 0;
                if (ct.glass && !ct.rgba.empty()) {
                    double sum[3] = {0, 0, 0}, n = 0;
                    for (size_t px = 0; px + 3 < ct.rgba.size(); px += 4) {
                        const int r = ct.rgba[px], g = ct.rgba[px + 1], b = ct.rgba[px + 2];
                        if (r + g + b < 3 * 110) continue;
                        sum[0] += r; sum[1] += g; sum[2] += b; n += 1;
                    }
                    const double top = std::max(sum[0], std::max(sum[1], sum[2]));
                    if (n > 0 && top > 0) for (int k = 0; k < 3; ++k) ct.glassTint[k] = static_cast<float>(sum[k] / top);
                    Log("Glass %08x: colour %.2f %.2f %.2f", hs, ct.glassTint[0], ct.glassTint[1], ct.glassTint[2]);
                }
                Log("Material: texture %u (%08x) emits %.1f, lamp %.1f, glass %d", op.id, hs, ct.emissive, ct.lampPower, ct.glass ? 1 : 0);
            }
            if (g_header && op.id < kMaxTexMaterials) g_header->texEmissive[op.id] = ct.glass ? static_cast<uint8_t>(255) : ct.mirror ? static_cast<uint8_t>(252) : static_cast<uint8_t>(std::min(ct.emissive * 8.0f, 250.0f));
        }
        if (m_textures.size() >= kMaxTextures || op.dataOffset + static_cast<uint64_t>(op.rowBytes) * op.rows > kSlotDataBytes) continue;
        D3D12_RESOURCE_DESC td = {};
        td.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width            = op.width;
        td.Height           = op.height;
        td.DepthOrArraySize = 1;
        td.MipLevels        = 1;
        td.Format           = static_cast<DXGI_FORMAT>(op.dxgiFormat);
        td.SampleDesc.Count = 1;
        PendingTexture pt = {};
        pt.id = op.id;
        UINT64 total = 0;
        m_device->GetCopyableFootprints(&td, 0, 1, 0, &pt.footprint, nullptr, nullptr, &total);
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        if (FAILED(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&pt.texture))))
            continue;
        pt.upload = CreateBuffer(m_device.Get(), total, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
        uint8_t* dst = nullptr;
        if (!pt.upload || FAILED(pt.upload->Map(0, nullptr, reinterpret_cast<void**>(&dst)))) continue;
        for (UINT r = 0; r < op.rows; ++r)
            memcpy(dst + pt.footprint.Offset + static_cast<UINT64>(r) * pt.footprint.Footprint.RowPitch,
                   data + op.dataOffset + static_cast<UINT64>(r) * op.rowBytes, op.rowBytes);
        pt.upload->Unmap(0, nullptr);
        const int32_t slotIndex = static_cast<int32_t>(m_textures.size());
        D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
        sv.Format                  = td.Format;
        sv.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.Texture2D.MipLevels     = 1;
        m_device->CreateShaderResourceView(pt.texture.Get(), &sv, HeapCpu(kTexBase + slotIndex));
        m_textures.push_back(pt.texture);
        m_textureSlots[op.id] = slotIndex;
        m_pendingTextures.push_back(std::move(pt));
    }

    const UINT cur = m_listIndex;
    std::vector<DynGeom> dyn(s.dynamicGeoms, s.dynamicGeoms + std::min(s.dynamicGeomCount, kMaxDynamicGeoms));
    {
        uint64_t last = 0;
        for (const DynGeom& g : dyn)
            last = std::max(last, g.dataOffset + static_cast<uint64_t>(g.vertexCount) * 24 + static_cast<uint64_t>(g.indexCount) * 4);
        if (last > kMaxDynamicBytes) dyn.clear();
        else if (last) {
            if (m_internalFence->GetCompletedValue() < m_lists[cur].fence) {
                m_internalFence->SetEventOnCompletion(m_lists[cur].fence, m_internalEvent);
                WaitForSingleObject(m_internalEvent, 5000);
            }
            memcpy(m_dynMapped[cur], data, static_cast<size_t>(last));
        }
    }

    struct Inst { uint32_t meshId; float t[3][4]; uint32_t textureId, flags; float alphaRef; };
    std::vector<Inst> instances;
    instances.reserve(std::min(s.instanceCount, kMaxInstances));
    for (uint32_t i = 0; i < std::min(s.instanceCount, kMaxInstances); ++i) {
        Inst in;
        in.meshId = s.instances[i].meshId;
        memcpy(in.t, s.instances[i].transform, sizeof(in.t));
        in.textureId = s.instances[i].textureId;
        in.flags     = s.instances[i].flags;
        in.alphaRef  = s.instances[i].alphaRef;
        in.flags &= ~kInstanceGlass;
        if (in.textureId) {
            auto ti = m_cpuTex.find(in.textureId);
            if (ti != m_cpuTex.end() && ti->second.glass) in.flags |= kInstanceGlass;
            if (ti != m_cpuTex.end() && ti->second.mirror) in.flags |= kInstanceMirror;
            if (ti != m_cpuTex.end() && ti->second.emissive > 0.f) in.flags |= static_cast<uint32_t>(std::min(ti->second.emissive * 8.0f, 255.0f)) << kInstanceEmissiveShift;
        }
        instances.push_back(in);
    }

    {
        const bool down = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_MENU) & 0x8000) && (GetAsyncKeyState(VK_F12) & 0x8000);
        const bool fileReq = GetFileAttributesW((g_dir + L"dump.request").c_str()) != INVALID_FILE_ATTRIBUTES;
        if ((down && !m_dumpKeyDown) || fileReq) {
            DeleteFileW((g_dir + L"dump.request").c_str());
            {
                HANDLE req = CreateFileW((g_dir + L"frame.request").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (req != INVALID_HANDLE_VALUE) CloseHandle(req);
            }
            SYSTEMTIME st; GetLocalTime(&st);
            wchar_t name[64];
            swprintf_s(name, L"world_%02u%02u%02u", st.wHour, st.wMinute, st.wSecond);
            CreateDirectoryW((g_dir + L"textures").c_str(), nullptr);
            auto texName = [&](uint32_t id) -> std::string {
                auto it = m_cpuTex.find(id);
                if (!id || it == m_cpuTex.end() || it->second.rgba.empty()) return std::string();
                char n[32];
                sprintf_s(n, "tex%u", id);
                if (!it->second.written)
                    it->second.written = WriteTga(g_dir + L"textures" + std::wstring(1, L'\\') + std::wstring(n, n + strlen(n)) + L".tga", it->second.w, it->second.h, it->second.rgba);
                return n;
            };
            FILE* mtl = _wfopen((g_dir + name + L".mtl").c_str(), L"w");
            std::unordered_map<uint32_t, bool> mtlDone;
            auto useTex = [&](FILE* of, uint32_t id) {
                const std::string n = texName(id);
                if (n.empty()) { fprintf(of, "usemtl plain\n"); return; }
                if (mtl && !mtlDone[id]) {
                    mtlDone[id] = true;
                    fprintf(mtl, "newmtl %s\nmap_Kd textures/%s.tga\nmap_d textures/%s.tga\n", n.c_str(), n.c_str(), n.c_str());
                }
                fprintf(of, "usemtl %s\n", n.c_str());
            };
            if (mtl) fprintf(mtl, "newmtl plain\nKd 0.6 0.6 0.6\n");
            FILE* f = _wfopen((g_dir + name + L".obj").c_str(), L"w");
            if (f) { char mn[64]; WideCharToMultiByte(CP_ACP, 0, name, -1, mn, sizeof(mn), nullptr, nullptr); fprintf(f, "mtllib %s.mtl\n", mn); }
            FILE* g = _wfopen((g_dir + name + L".txt").c_str(), L"w");
            if (f && g) {
                const float* c = s.cameraPos;
                fprintf(g, "frame %llu\ncamera %.3f %.3f %.3f\nsun %.4f %.4f %.4f\n", static_cast<unsigned long long>(s.frameId), c[0], c[1], c[2], s.sunDirection[0], s.sunDirection[1], s.sunDirection[2]);
                for (uint32_t li = 0; li < std::min(s.lightCount, kMaxLights); ++li)
                    fprintf(g, "light %.3f %.3f %.3f radius %.2f colour %.3f %.3f %.3f att %.4g %.4g %.4g\n", s.lightPos[li][0], s.lightPos[li][1], s.lightPos[li][2], s.lightPos[li][3],
                            s.lightCol[li][0], s.lightCol[li][1], s.lightCol[li][2], s.lightAtt[li][0], s.lightAtt[li][1], s.lightAtt[li][2]);
                fprintf(g, "viewproj");
                for (int r = 0; r < 4; ++r) for (int k = 0; k < 4; ++k) fprintf(g, " %.6g", s.viewProj[r][k]);
                fprintf(g, "\n");
                uint64_t base = 1, written = 0;
                for (size_t i = 0; i < instances.size(); ++i) {
                    const Inst& in = instances[i];
                    const float dx = in.t[0][3] - c[0], dy = in.t[1][3] - c[1], dz = in.t[2][3] - c[2];
                    auto it = m_cpuMeshes.find(in.meshId);
                    if (it == m_cpuMeshes.end()) continue;
                    const CpuMesh& cm = it->second;
                    bool isNear = dx * dx + dy * dy + dz * dz < 120.f * 120.f;
                    const size_t nv = cm.pos.size() / 3;
                    for (size_t v = 0; v < nv && !isNear; v += std::max<size_t>(1, nv / 64)) {
                        const float* q = &cm.pos[v * 3];
                        const float wx = in.t[0][0] * q[0] + in.t[0][1] * q[1] + in.t[0][2] * q[2] + in.t[0][3] - c[0];
                        const float wy = in.t[1][0] * q[0] + in.t[1][1] * q[1] + in.t[1][2] * q[2] + in.t[1][3] - c[1];
                        isNear = wx * wx + wy * wy < 120.f * 120.f;
                    }
                    if (!isNear) continue;
                    fprintf(f, "g inst%zu_mesh%u_flags%u_tex%u\n", i, in.meshId, in.flags, in.textureId);
                    useTex(f, in.textureId);
                    fprintf(g, "inst %zu mesh %u flags %u tex %u verts %zu tris %zu origin %.2f %.2f %.2f\n", i, in.meshId, in.flags, in.textureId, nv, cm.idx.size() / 3, in.t[0][3], in.t[1][3], in.t[2][3]);
                    for (size_t v = 0; v < nv; ++v) {
                        const float* q = &cm.pos[v * 3];
                        fprintf(f, "v %.3f %.3f %.3f\n", in.t[0][0] * q[0] + in.t[0][1] * q[1] + in.t[0][2] * q[2] + in.t[0][3],
                                in.t[1][0] * q[0] + in.t[1][1] * q[1] + in.t[1][2] * q[2] + in.t[1][3],
                                in.t[2][0] * q[0] + in.t[2][1] * q[1] + in.t[2][2] * q[2] + in.t[2][3]);
                    }
                    for (size_t v = 0; v < nv; ++v) fprintf(f, "vt %.5f %.5f\n", cm.uv.size() > v * 2 + 1 ? cm.uv[v * 2] : 0.f, cm.uv.size() > v * 2 + 1 ? 1.0f - cm.uv[v * 2 + 1] : 0.f);
                    for (size_t v = 0; v < nv; ++v) fprintf(f, "vc %08x\n", cm.col.size() > v ? cm.col[v] : 0xFFFFFFFFu);
                    for (size_t k = 0; k + 2 < cm.idx.size(); k += 3) {
                        const unsigned long long a = base + cm.idx[k], b2 = base + cm.idx[k + 1], c2 = base + cm.idx[k + 2];
                        fprintf(f, "f %llu/%llu %llu/%llu %llu/%llu\n", a, a, b2, b2, c2, c2);
                    }
                    base += nv;
                    ++written;
                }
                for (size_t i = 0; i < dyn.size(); ++i) {
                    const DynGeom& dg = dyn[i];
                    const float* q = reinterpret_cast<const float*>(data + dg.dataOffset);
                    const uint32_t* ix = reinterpret_cast<const uint32_t*>(data + dg.dataOffset + static_cast<uint64_t>(dg.vertexCount) * 24);
                    fprintf(f, "g char%zu_tex%u\n", i, dg.textureId);
                    useTex(f, dg.textureId);
                    const float* quv = q + dg.vertexCount * 3;
                    for (uint32_t v = 0; v < dg.vertexCount; ++v) fprintf(f, "v %.3f %.3f %.3f\n", q[v * 3], q[v * 3 + 1], q[v * 3 + 2]);
                    for (uint32_t v = 0; v < dg.vertexCount; ++v) fprintf(f, "vt %.5f %.5f\n", quv[v * 2], 1.0f - quv[v * 2 + 1]);
                    for (uint32_t v = 0; v < dg.vertexCount; ++v) fprintf(f, "vc %08x\n", reinterpret_cast<const uint32_t*>(quv + dg.vertexCount * 2)[v]);
                    for (uint32_t k = 0; k + 2 < dg.indexCount; k += 3) {
                        const unsigned long long a = base + ix[k], b2 = base + ix[k + 1], c2 = base + ix[k + 2];
                        fprintf(f, "f %llu/%llu %llu/%llu %llu/%llu\n", a, a, b2, b2, c2, c2);
                    }
                    base += dg.vertexCount;
                }
                for (const auto& kv : m_cpuTex) fprintf(g, "texture %u hash %08x\n", kv.first, kv.second.hash);
                Log("World dump %ls: %llu instances near the camera, %zu characters", name, static_cast<unsigned long long>(written), dyn.size());
            }
            if (f) fclose(f);
            if (g) fclose(g);
            if (mtl) fclose(mtl);
        }
        m_dumpKeyDown = down;
        const bool dbg = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && ((GetAsyncKeyState(VK_MENU) & 0x8000) || (GetAsyncKeyState(VK_SHIFT) & 0x8000)) &&
                         (GetAsyncKeyState(VK_F11) & 0x8000);
        if (dbg && !m_debugKeyDown) {
            m_debugMode = (m_debugMode + 1) % 9;
            static const char* const kModes[9] = {"normal image", "1 sun ray before SIGMA", "2 sun shadow after SIGMA", "3 normals",
                                                  "4 indirect after REBLUR", "5 indirect before REBLUR", "6 albedo", "7 all light received", "8 only the air"};
            Log("Debug view: %s", kModes[m_debugMode]);
            static const char* const kNames[9] = {"0 IMAGEM FINAL (RT)", "1 RAIO DO SOL (1 RAIO)", "2 LUZ (DLSS RAY RECONSTRUCTION)", "3 NORMAIS",
                                                  "4 LUZ REBATIDA CRUA (1 RAIO)", "5 LUZ REBATIDA CRUA (1 RAIO)", "6 TEXTURA (ALBEDO)",
                                                  "7 TODA A LUZ CRUA (SOL + REBATIDA)", "8 SO O AR (VOLUMETRICO)"};
            ShowText(kNames[m_debugMode]);
        }
        m_debugKeyDown = dbg;
    }

    FrameConstants fc = {};
    memcpy(fc.viewProj, s.viewProj, sizeof(fc.viewProj));
    if (!Invert4x4(s.viewProj, fc.invViewProj)) memset(fc.invViewProj, 0, sizeof(fc.invViewProj));
    memcpy(fc.prevViewProj, m_havePrev ? m_prevViewProj : s.viewProj, sizeof(fc.prevViewProj));
    memcpy(fc.cameraPos, s.cameraPos, sizeof(fc.cameraPos));
    memcpy(fc.sunDir, s.sunDirection, sizeof(fc.sunDir));
    fc.sunDir[3] = s.tanSunRadius;
    memcpy(fc.sunColor, s.sunColor, sizeof(fc.sunColor));
    memcpy(fc.skyColor, s.skyColor, sizeof(fc.skyColor));
    memcpy(fc.strength, s.strength, sizeof(fc.strength));
    fc.hitBaked = s.hitBakedRadiance;
    memcpy(fc.atmosphere, s.atmosphere, sizeof(fc.atmosphere));
    memcpy(fc.trueSun, s.trueSun, sizeof(fc.trueSun));
    memcpy(fc.clouds, s.clouds, sizeof(fc.clouds));
    fc.lightCount = static_cast<float>(std::min(s.lightCount, kMaxLights));
    memcpy(fc.lightPos, s.lightPos, sizeof(fc.lightPos));
    memcpy(fc.lightCol, s.lightCol, sizeof(fc.lightCol));
    memcpy(fc.lightDir, s.lightDir, sizeof(fc.lightDir));
    memcpy(fc.lightAtt, s.lightAtt, sizeof(fc.lightAtt));
    constexpr uint32_t kMaxLampsTraced = 32;
    {
        if (!m_settingsRead) ReadSettings();
        struct Fix { float d2, p[3], c[3], dir[3]; bool window, glow, emit; float reach, rad; };
        std::vector<Fix> fx;
        struct Portal { float c[3], u[3], v[3], tint[3], area, score; };
        std::vector<Portal> portals;
        fc.roomAmbient[3] = 0.0f;
        for (const Inst& in : instances) {
            if (!(in.flags & kInstanceGameLamp) || m_emitLampPower <= 0.0f) continue;
            const uint32_t c565 = in.flags >> 16;
            const float col[3] = {((c565 >> 11) & 31u) / 31.0f, ((c565 >> 5) & 63u) / 63.0f, (c565 & 31u) / 31.0f};
            const float strength = std::clamp(((in.flags >> kInstanceEmissiveShift) & 255u) / 8.0f / std::max(m_emitGlowSent, 1e-3f), 0.0f, 1.5f);
            for (const LampCluster& lc : LampClustersOf(in.meshId, 0)) {
                float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
                for (int k = 0; k < 8; ++k) {
                    const float o[3] = {(k & 1) ? lc.hi[0] : lc.lo[0], (k & 2) ? lc.hi[1] : lc.lo[1], (k & 4) ? lc.hi[2] : lc.lo[2]};
                    for (int r = 0; r < 3; ++r) {
                        const float w = in.t[r][0] * o[0] + in.t[r][1] * o[1] + in.t[r][2] * o[2] + in.t[r][3];
                        lo[r] = std::min(lo[r], w); hi[r] = std::max(hi[r], w);
                    }
                }
                Fix f = {};
                float wn[3];
                for (int r = 0; r < 3; ++r) wn[r] = in.t[r][0] * lc.n[0] + in.t[r][1] * lc.n[1] + in.t[r][2] * lc.n[2];
                const float nl = std::sqrt(wn[0] * wn[0] + wn[1] * wn[1] + wn[2] * wn[2]);
                for (int r = 0; r < 3; ++r) f.p[r] = (lo[r] + hi[r]) * 0.5f;
                if (nl > 1e-5f && std::fabs(wn[2]) / nl < 0.7f) {
                    const float side = wn[0] * (s.cameraPos[0] - f.p[0]) + wn[1] * (s.cameraPos[1] - f.p[1]) + wn[2] * (s.cameraPos[2] - f.p[2]);
                    for (int r = 0; r < 3; ++r) f.p[r] += wn[r] / nl * (side >= 0.0f ? 0.15f : -0.15f);
                } else {
                    f.p[2] = lo[2] - 0.12f;
                }
                const float dx = f.p[0] - s.cameraPos[0], dy = f.p[1] - s.cameraPos[1], dz = f.p[2] - s.cameraPos[2];
                f.d2 = dx * dx + dy * dy + dz * dz;
                if (f.d2 > m_lampReach * m_lampReach) continue;
                bool dup = false;
                for (const Fix& o : fx) dup = dup || (std::fabs(o.p[0] - f.p[0]) + std::fabs(o.p[1] - f.p[1]) + std::fabs(o.p[2] - f.p[2]) < 0.6f);
                if (dup) continue;
                const float size = std::clamp(std::sqrt(lc.area / 0.5f), 0.5f, 2.0f);
                for (int j = 0; j < 3; ++j) f.c[j] = col[j] * m_emitLampPower * strength * size;
                f.emit = true;
                fx.push_back(f);
            }
        }
        if (!m_catalogRead) LoadLampCatalog();
        std::vector<Fix> fixtures;
        if (m_catalogPower > 0.0f && !m_catalog.empty()) {
            const uint32_t state = s.reserved2;
            const int area = m_gameArea >= 0.0f ? static_cast<int>(m_gameArea) : static_cast<int>(state & 0xff) - 1;
            const int hour = m_gameHour >= 0.0f ? static_cast<int>(m_gameHour) : static_cast<int>((state >> 8) & 0xff);
            const bool night = m_lampsOn > m_lampsOff ? (hour >= m_lampsOn || hour < m_lampsOff) : (hour >= m_lampsOn && hour < m_lampsOff);
            const uint64_t now = GetTickCount64();
            const uint32_t sentLamps = ((state >> 24) & 1u) ? std::min<uint32_t>(s.lightCount, kMaxLights) : 0u;
            bool replaced[kMaxLights] = {};
            for (CatalogLamp& l : m_catalog) {
                if (area >= 0 && l.area != area) continue;
                if ((l.flags & 1u) && !night) continue;
                const float dx = l.pos[0] - s.cameraPos[0], dy = l.pos[1] - s.cameraPos[1], dz = l.pos[2] - s.cameraPos[2];
                const float d2 = dx * dx + dy * dy + dz * dz;
                if (d2 > m_lampReach * m_lampReach) continue;
                if (l.flags & 2u) {
                    if (now >= l.flickAt) { l.flickOn = !l.flickOn; l.flickAt = now + (l.flickOn ? 300 + rand() % 2500 : 40 + rand() % 200); }
                    if (!l.flickOn) continue;
                }
                for (uint32_t g = 0; g < sentLamps; ++g) {
                    const float ex = s.lightPos[g][0] - l.pos[0], ey = s.lightPos[g][1] - l.pos[1], ez = s.lightPos[g][2] - l.pos[2];
                    if (ex * ex + ey * ey + ez * ez < 4.0f && s.lightCol[g][3] < -1.5f) replaced[g] = true;
                }
                Fix f = {};
                for (int r = 0; r < 3; ++r) { f.p[r] = l.pos[r]; f.dir[r] = l.n[r]; }
                f.d2 = d2;
                for (int j = 0; j < 3; ++j) f.c[j] = l.col[j] / (4.0f * 3.14159265f) * kCandelaToRadiance * m_catalogPower;
                f.emit = true;
                const uint32_t kr = (l.flags >> 8) & 255u;
                f.reach = std::clamp((kr ? static_cast<float>(kr) : 15.0f) * m_fixtureReach / 15.0f, 1.0f, 40.0f);
                f.rad = std::clamp(static_cast<float>(l.model) / 100.0f, 0.05f, 0.9f);
                fixtures.push_back(f);
            }
            for (uint32_t g = 0; g < sentLamps; ++g)
                if (replaced[g]) for (int j = 0; j < 3; ++j) fc.lightCol[g][j] = 0.0f;
        }
        if (!fixtures.empty() && !m_emitLogged) { m_emitLogged = true; Log("Light fixtures lit near the camera: %zu (catalog of %zu)", fixtures.size(), m_catalog.size()); }
        m_emitNear.clear();
        for (const Fix& f : fixtures) if (f.d2 < 40.0f * 40.0f) m_emitNear.push_back({f.p[0], f.p[1], f.p[2]});
        {
            constexpr int kNx = 16, kNy = 16, kNz = 4, kMaxFix = 768, kPerCell = 48, kListMax = 8192;
            const float cell = 12.0f;
            std::sort(fixtures.begin(), fixtures.end(), [](const Fix& a, const Fix& b) { return a.d2 < b.d2; });
            if (fixtures.size() > static_cast<size_t>(kMaxFix)) fixtures.resize(kMaxFix);
            float corner[3];
            const int half[3] = {kNx / 2, kNy / 2, kNz / 2};
            for (int r = 0; r < 3; ++r) corner[r] = (std::floor(s.cameraPos[r] / cell) - static_cast<float>(half[r])) * cell;
            memset(fc.fixCells, 0, sizeof(fc.fixCells));
            memset(fc.fixList, 0, sizeof(fc.fixList));
            const uint32_t nf = static_cast<uint32_t>(fixtures.size());
            for (uint32_t i = 0; i < nf; ++i) {
                const Fix& f = fixtures[i];
                for (int r = 0; r < 3; ++r) fc.fixPos[i][r] = f.p[r];
                fc.fixPos[i][3] = std::floor(f.reach) + std::min(f.rad, 0.99f);
                for (int r = 0; r < 3; ++r) fc.fixCol[i][r] = f.c[r];
                float n[3] = {f.dir[0], f.dir[1], f.dir[2]};
                const float l1 = std::fabs(n[0]) + std::fabs(n[1]) + std::fabs(n[2]);
                if (l1 < 1e-6f) { n[0] = 0; n[1] = 0; n[2] = -1; } else for (float& v : n) v /= l1;
                float ex = n[0], ey = n[1];
                if (n[2] < 0.0f) { ex = (1.0f - std::fabs(n[1])) * (n[0] >= 0.0f ? 1.0f : -1.0f); ey = (1.0f - std::fabs(n[0])) * (n[1] >= 0.0f ? 1.0f : -1.0f); }
                const uint32_t qx = static_cast<uint32_t>(std::clamp((ex * 0.5f + 0.5f) * 65535.0f + 0.5f, 0.0f, 65535.0f));
                const uint32_t qy = static_cast<uint32_t>(std::clamp((ey * 0.5f + 0.5f) * 65535.0f + 0.5f, 0.0f, 65535.0f));
                const uint32_t packed = qx | (qy << 16);
                memcpy(&fc.fixCol[i][3], &packed, 4);
            }
            std::vector<std::vector<std::pair<float, uint16_t>>> cells(kNx * kNy * kNz);
            for (uint32_t i = 0; i < nf; ++i) {
                const Fix& f = fixtures[i];
                const float lum = f.c[0] * 0.3f + f.c[1] * 0.6f + f.c[2] * 0.1f;
                const float reach = f.reach;
                int lo[3], hi[3];
                const int dims[3] = {kNx, kNy, kNz};
                for (int r = 0; r < 3; ++r) {
                    lo[r] = std::max(0, static_cast<int>(std::floor((f.p[r] - reach - corner[r]) / cell)));
                    hi[r] = std::min(dims[r] - 1, static_cast<int>(std::floor((f.p[r] + reach - corner[r]) / cell)));
                }
                for (int z = lo[2]; z <= hi[2]; ++z) for (int y = lo[1]; y <= hi[1]; ++y) for (int x = lo[0]; x <= hi[0]; ++x) {
                    const int c[3] = {x, y, z};
                    float d2 = 0.0f, mid2 = 0.0f;
                    for (int r = 0; r < 3; ++r) {
                        const float a = corner[r] + c[r] * cell, b = a + cell;
                        const float q = std::clamp(f.p[r], a, b);
                        d2 += (f.p[r] - q) * (f.p[r] - q);
                        const float m = (a + b) * 0.5f - f.p[r];
                        mid2 += m * m;
                    }
                    if (d2 >= reach * reach) continue;
                    cells[(z * kNy + y) * kNx + x].push_back({lum / (1.0f + 0.05f * mid2), static_cast<uint16_t>(i)});
                }
            }
            uint32_t used = 0, longest = 0;
            std::vector<uint32_t> order;
            order.reserve(cells.size());
            for (uint32_t ci = 0; ci < cells.size(); ++ci) if (!cells[ci].empty()) order.push_back(ci);
            const auto cellD2 = [&](uint32_t ci) {
                const int x = static_cast<int>(ci % kNx), y = static_cast<int>((ci / kNx) % kNy), z = static_cast<int>(ci / (kNx * kNy));
                const float c[3] = {corner[0] + (x + 0.5f) * cell - s.cameraPos[0], corner[1] + (y + 0.5f) * cell - s.cameraPos[1], corner[2] + (z + 0.5f) * cell - s.cameraPos[2]};
                return c[0] * c[0] + c[1] * c[1] + c[2] * c[2];
            };
            std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return cellD2(a) < cellD2(b); });
            for (size_t oi = 0; oi < order.size(); ++oi) {
                const uint32_t ci = order[oi];
                auto& v = cells[ci];
                std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
                const uint32_t cellsLeft = static_cast<uint32_t>(order.size() - oi);
                const uint32_t fair = std::max<uint32_t>(8u, (kListMax - used) / cellsLeft);
                const uint32_t n = std::min<uint32_t>(static_cast<uint32_t>(v.size()), std::min<uint32_t>(std::min<uint32_t>(kPerCell, fair), kListMax - used));
                if (n == 0) break;
                for (uint32_t k = 0; k < n; ++k) {
                    const uint32_t e = used + k;
                    fc.fixList[e >> 1] |= static_cast<uint32_t>(v[k].second) << ((e & 1u) * 16u);
                }
                fc.fixCells[ci] = used | (n << 13);
                used += n;
                longest = std::max(longest, static_cast<uint32_t>(v.size()));
            }
            fc.fixInfo[0] = m_fixtureReach > 0.0f ? static_cast<float>(nf) : 0.0f;
            for (int r = 0; r < 3; ++r) fc.fixInfo[1 + r] = corner[r];
            fc.fixGrid[0] = cell; fc.fixGrid[1] = static_cast<float>(kNx); fc.fixGrid[2] = static_cast<float>(kNy); fc.fixGrid[3] = static_cast<float>(kNz);
            static uint64_t s_gridLog = 0;
            if (nf && (s_gridLog++ % 1800) == 0) Log("Fixtures traced: %u lit around the camera, %u grid entries, up to %u reach one cell (%d kept)", nf, used, longest, kPerCell);
        }
        const bool realLights = UseGameLights(fc, s);
        const uint32_t gameLights = static_cast<uint32_t>(fc.lightCount);
        for (const Inst& in : instances) {
            if (!in.textureId) continue;
            auto ti = m_cpuTex.find(in.textureId);
            const bool glow = (in.flags & kInstanceGlow) != 0;
            if (ti == m_cpuTex.end() || (ti->second.lampPower <= 0.f && !ti->second.glass && !glow)) continue;
            const bool isWindow = ti->second.glass && !glow;
            if (!isWindow && m_textureLamps <= 0.0f) continue;
            if (!isWindow && realLights && !glow) continue;
            if (glow) {
                CpuTex& gt = ti->second;
                gt.glowTex = true;
                if (gt.glowCol[0] < 0.0f) {
                    double sum[3] = {};
                    for (size_t pi = 0; pi + 3 < gt.rgba.size(); pi += 4) {
                        const int lum = gt.rgba[pi] * 3 + gt.rgba[pi + 1] * 6 + gt.rgba[pi + 2];
                        if (lum < 400) continue;
                        for (int ch = 0; ch < 3; ++ch) sum[ch] += gt.rgba[pi + ch];
                    }
                    const double peak = std::max(std::max(sum[0], sum[1]), std::max(sum[2], 1.0));
                    for (int ch = 0; ch < 3; ++ch) gt.glowCol[ch] = static_cast<float>(sum[ch] / peak);
                }
            }
            const float daylight = 1.0f - s.atmosphere[3];
            if (isWindow && (daylight <= 0.01f || m_windowPower <= 0.f)) continue;
            const float lampOn = ti->second.night ? std::clamp((s.atmosphere[3] - 0.15f) / 0.45f, 0.0f, 1.0f) : 1.0f;
            if (lampOn <= 0.0f) continue;
            for (const LampCluster& lc : LampClustersOf(in.meshId, in.textureId)) {
                if (glow && lc.area < 0.5f) continue;
                float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
                for (int k = 0; k < 8; ++k) {
                    const float o[3] = {(k & 1) ? lc.hi[0] : lc.lo[0], (k & 2) ? lc.hi[1] : lc.lo[1], (k & 4) ? lc.hi[2] : lc.lo[2]};
                    for (int r = 0; r < 3; ++r) {
                        const float w = in.t[r][0] * o[0] + in.t[r][1] * o[1] + in.t[r][2] * o[2] + in.t[r][3];
                        lo[r] = std::min(lo[r], w); hi[r] = std::max(hi[r], w);
                    }
                }
                if (isWindow) {
                    float wn[3];
                    for (int r = 0; r < 3; ++r) wn[r] = in.t[r][0] * lc.n[0] + in.t[r][1] * lc.n[1] + in.t[r][2] * lc.n[2];
                    const float hl = std::sqrt(wn[0] * wn[0] + wn[1] * wn[1]);
                    if (hl < 1e-4f) continue;
                    const float ux = -wn[1] / hl, uy = wn[0] / hl;
                    Portal po = {};
                    for (int r = 0; r < 3; ++r) po.c[r] = (lo[r] + hi[r]) * 0.5f;
                    const float halfW = 0.5f * (std::fabs(ux) * (hi[0] - lo[0]) + std::fabs(uy) * (hi[1] - lo[1])), halfH = 0.5f * (hi[2] - lo[2]);
                    po.u[0] = ux * halfW; po.u[1] = uy * halfW; po.v[2] = halfH;
                    po.area = 4.0f * halfW * halfH;
                    memcpy(po.tint, ti->second.glassTint, sizeof(po.tint));
                    const float cx = po.c[0] - s.cameraPos[0], cy = po.c[1] - s.cameraPos[1], cz = po.c[2] - s.cameraPos[2];
                    const float d2 = cx * cx + cy * cy + cz * cz;
                    if (po.area < 0.05f || d2 > 120.f * 120.f) continue;
                    bool same = false;
                    for (const Portal& o : portals) same = same || (std::fabs(o.c[0] - po.c[0]) + std::fabs(o.c[1] - po.c[1]) + std::fabs(o.c[2] - po.c[2]) < 0.3f);
                    if (same) continue;
                    po.score = po.area / std::max(d2, 1.0f);
                    portals.push_back(po);
                    continue;
                }
                Fix f = {};
                f.p[0] = (lo[0] + hi[0]) * 0.5f; f.p[1] = (lo[1] + hi[1]) * 0.5f; f.p[2] = lo[2] - 0.15f;
                f.window = isWindow;
                f.glow = glow;
                f.dir[0] = f.dir[1] = f.dir[2] = 0.f;
                if (isWindow) {
                    float wn[3];
                    for (int r = 0; r < 3; ++r) wn[r] = in.t[r][0] * lc.n[0] + in.t[r][1] * lc.n[1] + in.t[r][2] * lc.n[2];
                    const float nl = std::sqrt(wn[0] * wn[0] + wn[1] * wn[1] + wn[2] * wn[2]);
                    if (nl < 1e-5f) continue;
                    for (float& v : wn) v /= nl;
                    f.p[2] = (lo[2] + hi[2]) * 0.5f;
                    const float side = wn[0] * (s.cameraPos[0] - f.p[0]) + wn[1] * (s.cameraPos[1] - f.p[1]) + wn[2] * (s.cameraPos[2] - f.p[2]);
                    const float sgn = side >= 0.f ? 1.f : -1.f;
                    for (int r = 0; r < 3; ++r) { f.dir[r] = wn[r] * sgn; f.p[r] += f.dir[r] * 0.35f; }
                }
                const float dx = f.p[0] - s.cameraPos[0], dy = f.p[1] - s.cameraPos[1], dz = f.p[2] - s.cameraPos[2];
                f.d2 = dx * dx + dy * dy + dz * dz;
                if (f.d2 > 80.f * 80.f) continue;
                bool dup = false;
                for (const Fix& o : fx) dup = dup || (std::fabs(o.p[0] - f.p[0]) + std::fabs(o.p[1] - f.p[1]) + std::fabs(o.p[2] - f.p[2]) < 0.8f);
                for (uint32_t g = 0; g < gameLights && !dup; ++g)
                    dup = std::fabs(fc.lightPos[g][0] - f.p[0]) + std::fabs(fc.lightPos[g][1] - f.p[1]) + std::fabs(fc.lightPos[g][2] - f.p[2]) < 1.5f;
                if (dup) continue;
                const float size = std::clamp(std::sqrt(lc.area / 0.5f), 0.6f, 1.6f);
                for (int j = 0; j < 3; ++j) f.c[j] = isWindow ? m_windowPower * daylight * size : glow ? ti->second.glowCol[j] * m_decalLight * size : ti->second.lampCol[j] * ti->second.lampPower * size * lampOn;
                fx.push_back(f);
            }
        }
        std::sort(portals.begin(), portals.end(), [](const Portal& a, const Portal& b) { return a.score > b.score; });
        {
            const uint32_t np = static_cast<uint32_t>(std::min<size_t>(portals.size(), kMaxPortals));
            static size_t s_portalsLogged = 0;
            if (portals.size() != s_portalsLogged) {
                s_portalsLogged = portals.size();
                float areaAll = 0.f, areaUsed = 0.f;
                for (size_t i = 0; i < portals.size(); ++i) { areaAll += portals[i].area; if (i < np) areaUsed += portals[i].area; }
                Log("Windows: %zu near the camera, %u used, %.0f of %.0f of their area", portals.size(), np, areaUsed, areaAll);
                if (getenv("BULLYRT_LOG_WINDOWS"))
                    for (const Portal& w : portals) Log("  window at %.1f %.1f %.1f, half width %.2f %.2f, half height %.2f, area %.1f, tint %.2f %.2f %.2f", w.c[0], w.c[1], w.c[2], w.u[0], w.u[1], w.v[2], w.area, w.tint[0], w.tint[1], w.tint[2]);
            }
            portals.resize(np);
            const auto mixed = [](const Portal& p) {
                uint32_t h = 2166136261u;
                for (int r = 0; r < 3; ++r) { h ^= static_cast<uint32_t>(static_cast<int32_t>(std::floor(p.c[r] * 2.0f))); h *= 16777619u; h ^= h >> 13; }
                return h;
            };
            std::sort(portals.begin(), portals.end(), [&](const Portal& a, const Portal& b) { return mixed(a) < mixed(b); });
            fc.portalInfo[0] = static_cast<float>(np);
            fc.portalInfo[1] = m_windowPower;
            fc.portalInfo[2] = m_windowsPerRay;
            for (uint32_t i = 0; i < np; ++i) {
                memcpy(fc.portalC[i], portals[i].c, sizeof(portals[i].c));
                memcpy(fc.portalU[i], portals[i].u, sizeof(portals[i].u));
                memcpy(fc.portalV[i], portals[i].v, sizeof(portals[i].v));
                fc.portalU[i][3] = portals[i].tint[0]; fc.portalV[i][3] = portals[i].tint[1]; fc.portalC[i][3] = portals[i].tint[2];
                {
                    const float* pu = portals[i].u; const float* pv = portals[i].v;
                    float pn[3] = {pu[1] * pv[2] - pu[2] * pv[1], pu[2] * pv[0] - pu[0] * pv[2], pu[0] * pv[1] - pu[1] * pv[0]};
                    const float len = std::max(std::sqrt(pn[0] * pn[0] + pn[1] * pn[1] + pn[2] * pn[2]), 1e-6f);
                    for (int r = 0; r < 3; ++r) fc.portalN[i][r] = pn[r] / len;
                    fc.portalN[i][3] = 4.0f * len;
                }
            }
        }
        std::sort(fx.begin(), fx.end(), [](const Fix& a, const Fix& b) { return a.d2 < b.d2; });
        uint32_t n = gameLights;
        for (const Fix& f : fx) {
            if (n >= kMaxLights) break;
            fc.lightPos[n][0] = f.p[0]; fc.lightPos[n][1] = f.p[1]; fc.lightPos[n][2] = f.p[2]; fc.lightPos[n][3] = f.glow ? 14.0f : m_lampReach;
            fc.lightCol[n][0] = f.c[0]; fc.lightCol[n][1] = f.c[1]; fc.lightCol[n][2] = f.c[2]; fc.lightCol[n][3] = f.window ? -0.1f : -2.0f;
            fc.lightDir[n][0] = f.dir[0]; fc.lightDir[n][1] = f.dir[1]; fc.lightDir[n][2] = f.dir[2]; fc.lightDir[n][3] = f.window ? 0.5f : -2.0f;
            fc.lightAtt[n][0] = 1; fc.lightAtt[n][1] = 0; fc.lightAtt[n][2] = f.glow ? 0.25f : 0.1f; fc.lightAtt[n][3] = f.glow ? 1.0f : 0.0f;
            ++n;
        }
        if (fx.size() && !m_fixturesLogged) { m_fixturesLogged = true; Log("Lamps made from materials: %zu near the camera, %u slots used", fx.size(), n); }
        m_lastFixtures = static_cast<uint32_t>(fx.size());
        if (n > kMaxLampsTraced) {
            struct Pick { float score; uint32_t i; };
            std::vector<Pick> picks(n);
            for (uint32_t i = 0; i < n; ++i) {
                const float dx = fc.lightPos[i][0] - s.cameraPos[0], dy = fc.lightPos[i][1] - s.cameraPos[1], dz = fc.lightPos[i][2] - s.cameraPos[2];
                const bool  ahead = s.viewProj[3][0] * dx + s.viewProj[3][1] * dy + s.viewProj[3][2] * dz > 0.f;
                picks[i] = {(dx * dx + dy * dy + dz * dz) * (ahead ? 1.0f : 3.0f), i};
            }
            std::sort(picks.begin(), picks.end(), [](const Pick& a, const Pick& b) { return a.score < b.score; });
            float pos[kMaxLampsTraced][4], col[kMaxLampsTraced][4], dir[kMaxLampsTraced][4], att[kMaxLampsTraced][4];
            for (uint32_t k = 0; k < kMaxLampsTraced; ++k) {
                memcpy(pos[k], fc.lightPos[picks[k].i], 16); memcpy(col[k], fc.lightCol[picks[k].i], 16);
                memcpy(dir[k], fc.lightDir[picks[k].i], 16); memcpy(att[k], fc.lightAtt[picks[k].i], 16);
            }
            memcpy(fc.lightPos, pos, sizeof(pos)); memcpy(fc.lightCol, col, sizeof(col));
            memcpy(fc.lightDir, dir, sizeof(dir)); memcpy(fc.lightAtt, att, sizeof(att));
            n = kMaxLampsTraced;
        }
        fc.lightCount = static_cast<float>(n);
        if (!realLights && m_textureLamps > 0.0f) for (uint32_t i = 0; i < n; ++i)
            for (int j = 0; j < 3; ++j) fc.lightCol[i][j] *= m_lampTint[j];
        memcpy(fc.exposure, m_expoSettings, sizeof(fc.exposure));
        memcpy(fc.windowLight, m_windowLight, sizeof(m_windowLight));
        for (int j = 0; j < 3; ++j) fc.sunColor[j] *= 1.0f + (m_sunTint[j] - 1.0f) * (1.0f - s.atmosphere[3]);
        UpdateOptions();
        fc.options = (m_optReflections ? 1u : 0u) | (m_optSunShadows ? 2u : 0u) | (m_optLocalShadows ? 4u : 0u) | (static_cast<uint32_t>(m_optLighting) << 3) | (m_optRT ? 32u : 0u) | (m_optVolumetric ? 64u : 0u);
        fc.osd[0] = GetTickCount64() < m_osdUntil ? 1.0f : 0.0f;
        fc.osd[1] = m_roomFill;
        memcpy(fc.fog, m_fog, sizeof(fc.fog));
        memcpy(fc.weatherTop, s.weatherTop, sizeof(fc.weatherTop));
        memcpy(fc.weatherBot, s.weatherBot, sizeof(fc.weatherBot));
        for (int j = 0; j < 3; ++j) fc.prevCam[j] = m_havePrev ? m_prevCameraPos[j] : s.cameraPos[j];
        if (s.weatherFog[0] > 0.0f) {
            fc.fog[1] *= s.weatherFog[0];
            fc.fog[0] /= std::sqrt(s.weatherFog[0]);
        }
        fc.sharc[0] = m_sharcScale;
        fc.sharc[1] = m_sharcAccumulate;
        fc.sharc[2] = m_sharcStale;
        fc.sharc[3] = m_sharcTile;
        fc.sharc2[0] = m_sharcSurfaces;
        fc.sharc2[1] = m_decalGlow;
        fc.restirInfo[0] = static_cast<float>(m_restirRowPitch);
        fc.restirInfo[1] = static_cast<float>(m_restirArrayPitch);
        fc.restirInfo[2] = m_restirOn;
        fc.restirInfo[3] = m_restirSamples;
        fc.restirTune[0] = m_restirHistory; fc.restirTune[1] = m_restirRadius; fc.restirTune[2] = m_restirNeighbors; fc.restirTune[3] = m_restirPermute;
        {
            uint8_t prevToNow[160], nowToPrev[160];
            memset(prevToNow, 255, sizeof(prevToNow));
            memset(nowToPrev, 255, sizeof(nowToPrev));
            const uint32_t lamps = std::min<uint32_t>(static_cast<uint32_t>(fc.lightCount), 32u);
            const uint32_t wins = std::min<uint32_t>(static_cast<uint32_t>(fc.portalInfo[0]), kMaxPortals);
            const auto same = [](const float* a, const float* b) { return std::fabs(a[0] - b[0]) + std::fabs(a[1] - b[1]) + std::fabs(a[2] - b[2]) < 0.05f; };
            for (uint32_t i = 0; i < lamps; ++i)
                for (uint32_t j = 0; j < m_restirPrevLamps; ++j)
                    if (prevToNow[j] == 255 && same(fc.lightPos[i], m_restirPrevKey[j])) { nowToPrev[i] = static_cast<uint8_t>(j); prevToNow[j] = static_cast<uint8_t>(i); break; }
            for (uint32_t w = 0; w < wins; ++w)
                for (uint32_t j = 0; j < m_restirPrevWindows; ++j)
                    if (prevToNow[32 + j] == 255 && same(fc.portalC[w], m_restirPrevKey[32 + j])) { nowToPrev[32 + w] = static_cast<uint8_t>(32 + j); prevToNow[32 + j] = static_cast<uint8_t>(32 + w); break; }
            for (uint32_t i = 0; i < lamps; ++i) memcpy(m_restirPrevKey[i], fc.lightPos[i], 12);
            for (uint32_t w = 0; w < wins; ++w) memcpy(m_restirPrevKey[32 + w], fc.portalC[w], 12);
            m_restirPrevLamps = lamps;
            m_restirPrevWindows = wins;
            memcpy(fc.restirMap, prevToNow, 160);
            memcpy(fc.restirMap + 40, nowToPrev, 160);
        }
        fc.bounce[0] = m_bounceColor;
        fc.bounce[1] = m_beams;
        fc.bounce[2] = m_beamsOutside;
        fc.bounce[3] = m_lampFalloff;
        fc.windowLight[3] = m_imageGain;
        fc.osd[2] = (m_rrNow && !m_rrFailed) ? 4.0e9f : m_samplePeriod;
        fc.osd[3] = 0.0f;
        memset(fc.osdText, 0, sizeof(fc.osdText));
        if (GetTickCount64() < m_osdTextUntil) {
            const size_t len = std::min<size_t>(m_osdText.size(), 32);
            for (size_t i = 0; i < len; ++i) fc.osdText[i] = static_cast<uint8_t>(m_osdText[i]);
            fc.osd[3] = static_cast<float>(len);
        }
    }
    if (const char* dv = getenv("BULLYRT_DEBUGVIEW")) m_debugMode = static_cast<uint32_t>(atoi(dv)) % 10;
    fc.debugMode = m_debugMode;
    fc.pad2[0]   = s.fullRayTracing;
    fc.pad2[1]   = (m_rrNow && !m_rrFailed) ? 1u : 0u;
    fc.pad2[2]   = m_rrAlbedo ? 1u : 0u;
    const Camera cam = Decompose(s.viewProj);
    if (m_framesDone == 0)
        Log("Camera decomposition: sx %.3f sy %.3f s %.3f ndcA %.4f ndcB %.4f, axis error %.4f", cam.proj[0][0], cam.proj[1][1],
            cam.proj[3][2], cam.ndcA, cam.ndcB, cam.orthoError);
    const float denoisingRange = s.denoisingRange > 0 ? s.denoisingRange : 1000.0f;
    fc.projParams[0] = cam.ndcA;
    fc.projParams[1] = cam.ndcB;
    fc.projParams[2] = denoisingRange;
    const uint32_t frameBits = static_cast<uint32_t>(s.frameId);
    memcpy(&fc.projParams[3], &frameBits, 4);
    fc.size[0] = std::min(s.width, m_width);
    fc.size[1] = std::min(s.height, m_height);
    fc.rtScale = m_rtScale;
    {
        const auto halton = [](uint32_t i, uint32_t b) { float f = 1.0f, r = 0.0f; for (++i; i; i /= b) { f /= static_cast<float>(b); r += f * static_cast<float>(i % b); } return r; };
        const bool jitter = fc.pad2[1] != 0 && m_rtScale > 1 && m_rrJitter > 0.0f;
        const uint32_t k = static_cast<uint32_t>(s.frameId) % 32u;
        uint32_t cx = m_rtScale / 2, cy = m_rtScale / 2;
        if (jitter) {
            cx = std::min(static_cast<uint32_t>(halton(k, 2) * m_rtScale), m_rtScale - 1);
            cy = std::min(static_cast<uint32_t>(halton(k, 3) * m_rtScale), m_rtScale - 1);
        }
        fc.aoInfo[0] = m_aoReach; fc.aoInfo[1] = m_aoStrength; fc.aoInfo[2] = fc.aoInfo[3] = 0.0f;
        fc.volInfo[0] = m_volDensity; fc.volInfo[1] = m_volDensityOut; fc.volInfo[2] = m_volLocal; fc.volInfo[3] = m_volAmbient;
        fc.rtJitter[0] = cx; fc.rtJitter[1] = cy; fc.rtJitter[2] = jitter ? 1u : 0u; fc.rtJitter[3] = 0;
        m_jitterOffset[0] = jitter ? ((static_cast<float>(cx) + 0.5f) / m_rtScale - 0.5f) * m_rrJitterSign : 0.0f;
        m_jitterOffset[1] = jitter ? ((static_cast<float>(cy) + 0.5f) / m_rtScale - 0.5f) * m_rrJitterSign : 0.0f;
    }
    fc.rtSize[0] = (fc.size[0] + m_rtScale - 1) / m_rtScale;
    fc.rtSize[1] = (fc.size[1] + m_rtScale - 1) / m_rtScale;
    float viewProjCopy[4][4];
    memcpy(viewProjCopy, s.viewProj, sizeof(viewProjCopy));
    const float camMove = std::fabs(s.cameraPos[0] - m_prevCameraPos[0]) + std::fabs(s.cameraPos[1] - m_prevCameraPos[1]) +
                          std::fabs(s.cameraPos[2] - m_prevCameraPos[2]);
    const bool resetHistory = m_historyReset || s.resetHistory || !m_havePrev || camMove > 15.0f;
    memcpy(m_prevCameraPos, s.cameraPos, sizeof(m_prevCameraPos));
    float sunDirCopy[3] = {s.sunDirection[0], s.sunDirection[1], s.sunDirection[2]};
    const uint64_t frameId   = s.frameId;
    const uint64_t gameValue = s.gameFenceValue;
    const uint64_t release   = s.outputFreeValue;

    MemoryBarrier();
    s.state = kSlotFree;
    SetEvent(m_slotFreeEvent);

    FrameList& fl  = m_lists[cur];
    m_listIndex    = (m_listIndex + 1) % 3;
    if (m_internalFence->GetCompletedValue() < fl.fence) {
        const ULONGLONG waitStart = GetTickCount64();
        m_internalFence->SetEventOnCompletion(fl.fence, m_internalEvent);
        WaitForSingleObject(m_internalEvent, 5000);
        m_telListWaitMs += static_cast<double>(GetTickCount64() - waitStart);
    }
    if (fl.fence != 0) {
        const D3D12_RANGE rr = {cur * 16u, cur * 16u + 16u};
        uint8_t* ts = nullptr;
        if (SUCCEEDED(m_timestampReadback->Map(0, &rr, reinterpret_cast<void**>(&ts)))) {
            const auto* p = reinterpret_cast<const uint64_t*>(ts + rr.Begin);
            if (p[kGpuMarks - 1] > p[0] && m_timestampFreq) {
                const double toMs = 1000.0 / m_timestampFreq;
                const double total = (p[kGpuMarks - 1] - p[0]) * toMs;
                h.bridgeGpuMs = static_cast<float>(total);
                for (UINT m = 1; m < kGpuMarks; ++m) if (p[m] >= p[m - 1]) m_gpuSum[m] += (p[m] - p[m - 1]) * toMs;
                m_gpuTotal += total;
                bool partsValid = true;
                for (UINT m = 1; m < kGpuMarks; ++m) partsValid = partsValid && p[m] >= p[m - 1];
                if (partsValid) for (UINT m = 1; m < kGpuMarks; ++m) m_telGpu[m] += (p[m] - p[m - 1]) * toMs;
                else m_telGpu[kGpuMarks - 1] += total;
                m_telGpuMax = std::max(static_cast<double>(m_telGpuMax), total);
                ++m_telFrames;
                if (total > m_gpuWorst) {
                    m_gpuWorst = total;
                    for (UINT m = 1; m < kGpuMarks; ++m) m_gpuWorstParts[m] = p[m] >= p[m - 1] ? (p[m] - p[m - 1]) * toMs : 0.0;
                }
                ++m_gpuFrames;
            }
            const D3D12_RANGE none = {0, 0};
            m_timestampReadback->Unmap(0, &none);
        }
    }
    fl.alloc->Reset();
    ID3D12GraphicsCommandList4* cl = fl.list.Get();
    cl->Reset(fl.alloc.Get(), nullptr);
    cl->EndQuery(m_timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);

    for (PendingTexture& pt : m_pendingTextures) {
        D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
        dst.pResource = pt.texture.Get();
        dst.Type      = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.pResource = pt.upload.Get();
        src.Type      = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint = pt.footprint;
        cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        D3D12_RESOURCE_BARRIER b = {};
        b.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource   = pt.texture.Get();
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.StateAfter  = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        cl->ResourceBarrier(1, &b);
        Retire(std::move(pt.upload));
    }
    m_pendingTextures.clear();

    while (!m_freedAttrs.empty() && m_freedAttrs.front().fence <= m_internalFence->GetCompletedValue()) {
        m_freeAttrs.push_back(m_freedAttrs.front().index);
        m_freedAttrs.pop_front();
    }
    for (Pending& p : m_pending) {
        D3D12_RAYTRACING_GEOMETRY_DESC g      = {};
        g.Type                                = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
        g.Flags                               = D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;
        g.Triangles.VertexBuffer.StartAddress = p.upload->GetGPUVirtualAddress();
        g.Triangles.VertexBuffer.StrideInBytes = 12;
        g.Triangles.VertexFormat              = DXGI_FORMAT_R32G32B32_FLOAT;
        g.Triangles.VertexCount               = p.vertexCount;
        g.Triangles.IndexBuffer               = p.upload->GetGPUVirtualAddress() + static_cast<UINT64>(p.vertexCount) * 24;
        g.Triangles.IndexFormat               = DXGI_FORMAT_R32_UINT;
        g.Triangles.IndexCount                = p.indexCount;
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS in = {};
        in.Type           = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        in.Flags          = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
        in.NumDescs       = 1;
        in.DescsLayout    = D3D12_ELEMENTS_LAYOUT_ARRAY;
        in.pGeometryDescs = &g;
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO pre = {};
        m_device->GetRaytracingAccelerationStructurePrebuildInfo(&in, &pre);
        ComPtr<ID3D12Resource> as = CreateBuffer(m_device.Get(), pre.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
                                                 D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                                 D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
        ComPtr<ID3D12Resource> scratch = CreateBuffer(m_device.Get(), pre.ScratchDataSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
                                                      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                                      D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        if (!as || !scratch || pre.ResultDataMaxSizeInBytes == 0) continue;
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC b = {};
        b.Inputs                           = in;
        b.DestAccelerationStructureData    = as->GetGPUVirtualAddress();
        b.ScratchAccelerationStructureData = scratch->GetGPUVirtualAddress();
        cl->BuildRaytracingAccelerationStructure(&b, 0, nullptr);

        ComPtr<ID3D12Resource> attr = CreateBuffer(m_device.Get(), p.bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COMMON);
        if (!attr || m_freeAttrs.empty()) { Retire(std::move(scratch)); Retire(std::move(p.upload)); Retire(std::move(as)); continue; }
        D3D12_RESOURCE_BARRIER tb0 = {};
        tb0.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        tb0.Transition.pResource   = attr.Get();
        tb0.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        tb0.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        tb0.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
        cl->ResourceBarrier(1, &tb0);
        cl->CopyBufferRegion(attr.Get(), 0, p.upload.Get(), 0, p.bytes);
        tb0.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        tb0.Transition.StateAfter  = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        cl->ResourceBarrier(1, &tb0);
        const uint32_t attrIndex = m_freeAttrs.back();
        m_freeAttrs.pop_back();
        CreateRawSrv(attr.Get(), p.bytes, attrIndex);

        Mesh& mesh = m_meshes[p.meshId];
        mesh.blas        = as;
        mesh.attr        = attr;
        mesh.attrIndex   = attrIndex;
        mesh.vertexCount = p.vertexCount;
        Retire(std::move(scratch));
        Retire(std::move(p.upload));
    }
    m_pending.clear();
    D3D12_RESOURCE_BARRIER uav = {};
    uav.Type                   = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    cl->ResourceBarrier(1, &uav);

    bool haveDyn = false;
    if (!dyn.empty()) {
        std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> geoms(dyn.size());
        const D3D12_GPU_VIRTUAL_ADDRESS base = m_dynUpload[cur]->GetGPUVirtualAddress();
        for (size_t i = 0; i < dyn.size(); ++i) {
            D3D12_RAYTRACING_GEOMETRY_DESC& g = geoms[i];
            g = {};
            g.Type  = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
            g.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
            g.Triangles.VertexBuffer.StartAddress  = base + dyn[i].dataOffset;
            g.Triangles.VertexBuffer.StrideInBytes = 12;
            g.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
            g.Triangles.VertexCount  = dyn[i].vertexCount;
            g.Triangles.IndexBuffer  = base + dyn[i].dataOffset + static_cast<UINT64>(dyn[i].vertexCount) * 24;
            g.Triangles.IndexFormat  = DXGI_FORMAT_R32_UINT;
            g.Triangles.IndexCount   = dyn[i].indexCount;
        }
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS in = {};
        in.Type           = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        in.Flags          = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
        in.NumDescs       = static_cast<UINT>(geoms.size());
        in.DescsLayout    = D3D12_ELEMENTS_LAYOUT_ARRAY;
        in.pGeometryDescs = geoms.data();
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO pre = {};
        m_device->GetRaytracingAccelerationStructurePrebuildInfo(&in, &pre);
        if (pre.ResultDataMaxSizeInBytes > m_dynBlasSize[cur]) {
            Retire(std::move(m_dynBlas[cur]));
            m_dynBlasSize[cur] = pre.ResultDataMaxSizeInBytes + pre.ResultDataMaxSizeInBytes / 2;
            m_dynBlas[cur] = CreateBuffer(m_device.Get(), m_dynBlasSize[cur], D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                          D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
        }
        if (pre.ScratchDataSizeInBytes > m_dynScratchSize[cur]) {
            Retire(std::move(m_dynScratch[cur]));
            m_dynScratchSize[cur] = pre.ScratchDataSizeInBytes + pre.ScratchDataSizeInBytes / 2;
            m_dynScratch[cur] = CreateBuffer(m_device.Get(), m_dynScratchSize[cur], D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
        if (m_dynBlas[cur] && m_dynScratch[cur]) {
            D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC b = {};
            b.Inputs = in;
            b.DestAccelerationStructureData    = m_dynBlas[cur]->GetGPUVirtualAddress();
            b.ScratchAccelerationStructureData = m_dynScratch[cur]->GetGPUVirtualAddress();
            cl->BuildRaytracingAccelerationStructure(&b, 0, nullptr);
            cl->ResourceBarrier(1, &uav);
            haveDyn = true;
        }
    }

    auto texSlot = [&](uint32_t id) {
        auto t = m_textureSlots.find(id);
        return id && t != m_textureSlots.end() ? t->second : -1;
    };
    GeomRecord* records = m_recordsMapped[cur];
    std::unordered_map<uint64_t, std::vector<std::array<float, 12>>> placementsNow;
    uint32_t movedBoxes = 0;
    float movedLo[8][3], movedHi[8][3];
    ID3D12Resource* ib = m_instances[m_instanceRing].Get();
    m_instanceRing     = (m_instanceRing + 1) % 3;
    D3D12_RAYTRACING_INSTANCE_DESC* mapped = nullptr;
    UINT written = 0;
    if (SUCCEEDED(ib->Map(0, nullptr, reinterpret_cast<void**>(&mapped)))) {
        for (const Inst& in : instances) {
            auto it = m_meshes.find(in.meshId);
            if (it == m_meshes.end() || !it->second.blas) continue;
            if (in.flags & kInstanceGlow) continue;
            if (in.textureId) {
                auto veilTex = m_cpuTex.find(in.textureId);
                if (veilTex != m_cpuTex.end() && veilTex->second.veil && !veilTex->second.glass) continue;
            }
            const Mesh& mesh = it->second;
            GeomRecord& rec = records[written];
            rec = {};
            rec.attr      = mesh.attrIndex;
            rec.posOffset = 0;
            rec.uvOffset  = mesh.vertexCount * 12;
            rec.colorOffset = mesh.vertexCount * 20;
            rec.idxOffset = mesh.vertexCount * 24;
            rec.tex       = texSlot(in.textureId);
            rec.flags     = in.flags;
            rec.alphaRef  = in.alphaRef;
            rec.prevAttr  = 0xFFFFFFFFu;
            {
                const uint64_t placeKey = (static_cast<uint64_t>(in.meshId) << 32) | in.textureId;
                std::array<float, 12> now;
                memcpy(now.data(), in.t, sizeof(in.t));
                placementsNow[placeKey].push_back(now);
                auto last = m_prevPlacements.find(placeKey);
                if (last != m_prevPlacements.end()) {
                    const std::array<float, 12>* nearest = nullptr;
                    float bestSq = 25.0f, change = 0.0f;
                    for (const auto& was : last->second) {
                        const float dx = was[3] - now[3], dy = was[7] - now[7], dz = was[11] - now[11];
                        const float sq = dx * dx + dy * dy + dz * dz;
                        if (sq < bestSq) {
                            bestSq = sq; nearest = &was; change = 0.0f;
                            for (int m = 0; m < 12; ++m) change = std::max(change, std::fabs(was[m] - now[m]));
                            if (change < 1e-4f) break;
                        }
                    }
                    if (nearest && change >= 1e-4f) {
                        rec.moved = 1;
                        memcpy(rec.prevT, nearest->data(), sizeof(rec.prevT));
                        auto bounds = m_meshBounds.find(in.meshId);
                        if (bounds == m_meshBounds.end()) {
                            std::array<float, 6> b = {1e30f, 1e30f, 1e30f, -1e30f, -1e30f, -1e30f};
                            auto cm = m_cpuMeshes.find(in.meshId);
                            if (cm != m_cpuMeshes.end())
                                for (size_t v = 0; v + 2 < cm->second.pos.size(); v += 3)
                                    for (int a = 0; a < 3; ++a) { b[a] = std::min(b[a], cm->second.pos[v + a]); b[a + 3] = std::max(b[a + 3], cm->second.pos[v + a]); }
                            bounds = m_meshBounds.emplace(in.meshId, b).first;
                        }
                        const uint32_t boxIndex = movedBoxes < 8 ? movedBoxes++ : 7;
                        float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
                        for (int k = 0; k < 8; ++k) {
                            const float o[3] = {bounds->second[(k & 1) ? 3 : 0], bounds->second[(k & 2) ? 4 : 1], bounds->second[(k & 4) ? 5 : 2]};
                            for (int r = 0; r < 3; ++r) {
                                const float w = in.t[r][0] * o[0] + in.t[r][1] * o[1] + in.t[r][2] * o[2] + in.t[r][3];
                                lo[r] = std::min(lo[r], w - 0.5f); hi[r] = std::max(hi[r], w + 0.5f);
                            }
                        }
                        for (int r = 0; r < 3; ++r) {
                            const bool merge = boxIndex == 7 && movedBoxes == 8 && movedLo[7][r] < 1e29f && movedHi[7][r] > -1e29f && written != 0 && rec.moved == 2;
                            movedLo[boxIndex][r] = merge ? std::min(movedLo[boxIndex][r], lo[r]) : lo[r];
                            movedHi[boxIndex][r] = merge ? std::max(movedHi[boxIndex][r], hi[r]) : hi[r];
                        }
                    }
                }
            }
            D3D12_RAYTRACING_INSTANCE_DESC& d = mapped[written];
            memset(&d, 0, sizeof(d));
            memcpy(d.Transform, in.t, sizeof(d.Transform));
            d.InstanceID            = written;
            d.InstanceMask          = 0x01;
            d.Flags                 = (in.flags & (kInstanceAlphaTest | kInstanceGlass)) ? D3D12_RAYTRACING_INSTANCE_FLAG_NONE
                                                                      : D3D12_RAYTRACING_INSTANCE_FLAG_FORCE_OPAQUE;
            if (in.flags & (kInstanceTwoSided | kInstanceGlass)) d.Flags |= D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE;
            if (in.flags & kInstanceFrontCCW) d.Flags |= D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_FRONT_COUNTERCLOCKWISE;
            d.AccelerationStructure = mesh.blas->GetGPUVirtualAddress();
            ++written;
        }
        if (haveDyn) {
            for (size_t i = 0; i < dyn.size(); ++i) {
                GeomRecord& rec = records[written + i];
                rec = {};
                rec.attr      = cur;
                rec.posOffset = static_cast<uint32_t>(dyn[i].dataOffset);
                rec.uvOffset  = static_cast<uint32_t>(dyn[i].dataOffset + static_cast<uint64_t>(dyn[i].vertexCount) * 12);
                rec.colorOffset = static_cast<uint32_t>(dyn[i].dataOffset + static_cast<uint64_t>(dyn[i].vertexCount) * 20);
                rec.idxOffset = static_cast<uint32_t>(dyn[i].dataOffset + static_cast<uint64_t>(dyn[i].vertexCount) * 24);
                rec.tex       = texSlot(dyn[i].textureId);
                rec.prevAttr  = 0xFFFFFFFFu;
                for (PrevDyn& was : m_prevDyn) {
                    if (was.used || was.vertexCount != dyn[i].vertexCount || was.indexCount != dyn[i].indexCount || was.textureId != dyn[i].textureId) continue;
                    was.used = true;
                    rec.prevAttr = was.attr;
                    rec.prevPosOffset = was.dataOffset;
                    break;
                }
            }
            D3D12_RAYTRACING_INSTANCE_DESC& d = mapped[written];
            memset(&d, 0, sizeof(d));
            d.Transform[0][0] = d.Transform[1][1] = d.Transform[2][2] = 1.0f;
            d.InstanceID            = written;
            d.InstanceMask          = 0x02;
            d.Flags                 = D3D12_RAYTRACING_INSTANCE_FLAG_FORCE_OPAQUE | D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE;
            d.AccelerationStructure = m_dynBlas[cur]->GetGPUVirtualAddress();
            ++written;
        }
        ib->Unmap(0, nullptr);
    }
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC tb = {};
    tb.Inputs.Type                      = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    tb.Inputs.Flags                     = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
    tb.Inputs.NumDescs                  = written;
    tb.Inputs.DescsLayout               = D3D12_ELEMENTS_LAYOUT_ARRAY;
    tb.Inputs.InstanceDescs             = ib->GetGPUVirtualAddress();
    tb.DestAccelerationStructureData    = m_tlas->GetGPUVirtualAddress();
    tb.ScratchAccelerationStructureData = m_tlasScratch->GetGPUVirtualAddress();
    cl->BuildRaytracingAccelerationStructure(&tb, 0, nullptr);
    cl->ResourceBarrier(1, &uav);
    m_prevPlacements.swap(placementsNow);
    m_prevDyn.clear();
    if (haveDyn)
        for (const auto& g : dyn) m_prevDyn.push_back({g.vertexCount, g.indexCount, g.textureId, static_cast<uint32_t>(cur), static_cast<uint32_t>(g.dataOffset), false});
    fc.sharc2[2] = static_cast<float>(movedBoxes);
    for (uint32_t b = 0; b < movedBoxes; ++b)
        for (int r = 0; r < 3; ++r) { fc.movedMin[b][r] = movedLo[b][r]; fc.movedMax[b][r] = movedHi[b][r]; }

    auto transition = [&](ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
        D3D12_RESOURCE_BARRIER t = {};
        t.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        t.Transition.pResource   = r;
        t.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        t.Transition.StateBefore = from;
        t.Transition.StateAfter  = to;
        cl->ResourceBarrier(1, &t);
    };
    const D3D12_RESOURCE_STATES kCommon = D3D12_RESOURCE_STATE_COMMON, kSrv = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                kUav = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    ID3D12Resource* inputs[] = {m_depth[slot].Get(), m_gNormal[slot].Get(), m_gAlbedo[slot].Get(), m_color[slot].Get(), m_opaque[slot].Get()};
    for (ID3D12Resource* r : inputs) transition(r, kCommon, kSrv);
    transition(m_output[slot].Get(), kCommon, kUav);

    const UINT cbOffset = cur * kFrameCbStride;
    memcpy(m_cbMapped + cbOffset, &fc, sizeof(fc));
    auto bindOurs = [&]() {
        ID3D12DescriptorHeap* heaps[] = {m_heap.Get()};
        cl->SetDescriptorHeaps(1, heaps);
        cl->SetComputeRootSignature(m_rootSig.Get());
        cl->SetComputeRootConstantBufferView(0, m_cb->GetGPUVirtualAddress() + cbOffset);
        cl->SetComputeRootShaderResourceView(1, m_tlas->GetGPUVirtualAddress());
        D3D12_GPU_DESCRIPTOR_HANDLE g = m_heap->GetGPUDescriptorHandleForHeapStart();
        g.ptr += static_cast<UINT64>(slot * 33) * m_heapInc;
        cl->SetComputeRootDescriptorTable(2, g);
        cl->SetComputeRootDescriptorTable(3, HeapGpu(kTexBase));
        cl->SetComputeRootDescriptorTable(4, HeapGpu(kAttrBase));
        cl->SetComputeRootShaderResourceView(5, m_records[cur]->GetGPUVirtualAddress());
        cl->SetComputeRootUnorderedAccessView(6, m_exposureBuf->GetGPUVirtualAddress());
        cl->SetComputeRootUnorderedAccessView(7, m_restirReservoirs->GetGPUVirtualAddress());
        cl->SetComputeRootUnorderedAccessView(8, m_restirSurfaces->GetGPUVirtualAddress());
    };
    if (m_resetExposure && m_zeroBuf) {
        D3D12_RESOURCE_BARRIER b = {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = m_exposureBuf.Get();
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS; b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        cl->ResourceBarrier(1, &b);
        cl->CopyBufferRegion(m_exposureBuf.Get(), 0, m_zeroBuf.Get(), 0, 1024);
        std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
        cl->ResourceBarrier(1, &b);
        m_resetExposure = false;
    }
    const UINT gx = (fc.size[0] + 7) / 8, gy = (fc.size[1] + 7) / 8;

    if (m_optRT) {
    bindOurs();
    cl->EndQuery(m_timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
    cl->SetPipelineState(m_sharcUpdatePso.Get());
    {
        const UINT tile = static_cast<UINT>(std::max(fc.sharc[3], 1.0f));
        cl->Dispatch(((fc.size[0] / tile) + 7) / 8, ((fc.size[1] / tile) + 7) / 8, 1);
    }
    cl->ResourceBarrier(1, &uav);
    cl->EndQuery(m_timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2);
    cl->SetPipelineState(m_sharcResolvePso.Get());
    cl->Dispatch(kSharcEntries / 256, 1, 1);
    cl->ResourceBarrier(1, &uav);
    cl->EndQuery(m_timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 3);
    cl->SetPipelineState(m_tracePso.Get());
    cl->Dispatch((fc.rtSize[0] + 7) / 8, (fc.rtSize[1] + 7) / 8, 1);
    cl->ResourceBarrier(1, &uav);
    cl->EndQuery(m_timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 4);
    if (m_optVolumetric) {
        cl->SetPipelineState(m_volRestirPso.Get());
        cl->Dispatch((80 + 7) / 8, (45 + 7) / 8, (24 + 7) / 8);
        cl->ResourceBarrier(1, &uav);
        cl->SetPipelineState(m_froxelPso.Get());
        cl->Dispatch((160 + 7) / 8, (90 + 7) / 8, (48 + 7) / 8);
        cl->ResourceBarrier(1, &uav);
        cl->SetPipelineState(m_airIntegratePso.Get());
        cl->Dispatch((160 + 7) / 8, (90 + 7) / 8, 1);
        cl->ResourceBarrier(1, &uav);
    }
    cl->EndQuery(m_timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 5);

    nrd::CommonSettings cs = {};
    ToColumnMajor(cam.proj, cs.viewToClipMatrix);
    ToColumnMajor(m_havePrev ? m_prevCamera.proj : cam.proj, cs.viewToClipMatrixPrev);
    ToColumnMajor(cam.view, cs.worldToViewMatrix);
    ToColumnMajor(m_havePrev ? m_prevCamera.view : cam.view, cs.worldToViewMatrixPrev);
    cs.motionVectorScale[0] = 1.0f;
    cs.motionVectorScale[1] = 1.0f;
    cs.motionVectorScale[2] = 0.0f;
    cs.resourceSize[0] = cs.resourceSizePrev[0] = cs.rectSize[0] = cs.rectSizePrev[0] = static_cast<uint16_t>(fc.rtSize[0]);
    cs.resourceSize[1] = cs.resourceSizePrev[1] = cs.rectSize[1] = cs.rectSizePrev[1] = static_cast<uint16_t>(fc.rtSize[1]);
    cs.denoisingRange  = denoisingRange;
    cs.frameIndex      = frameBits;
    cs.accumulationMode = resetHistory ? nrd::AccumulationMode::CLEAR_AND_RESTART : nrd::AccumulationMode::CONTINUE;

    nrd::SigmaSettings sigma = {};
    sigma.lightDirection[0] = sunDirCopy[0];
    sigma.lightDirection[1] = sunDirCopy[1];
    sigma.lightDirection[2] = sunDirCopy[2];
    nrd::ReblurSettings reblur = {};
    reblur.maxAccumulatedFrameNum     = 30;
    reblur.maxFastAccumulatedFrameNum = 4;
    reblur.maxStabilizedFrameNum      = 8;
    reblur.antilagSettings.luminanceSigmaScale  = 1.5f;
    reblur.antilagSettings.luminanceSensitivity = 2.0f;
    reblur.diffusePrepassBlurRadius   = 6.0f;
    reblur.specularPrepassBlurRadius  = 12.0f;
    reblur.maxBlurRadius              = 10.0f;
    reblur.minBlurRadius              = 1.0f;
    reblur.minHitDistanceWeight       = 0.05f;
    reblur.planeDistanceSensitivity   = 0.01f;

    const bool rrNow = fc.pad2[1] != 0;
    if (!rrNow && m_nrdReady) {
    m_nrd.NewFrame();
    const nrd::Result r0 = m_nrd.SetCommonSettings(cs);
    const nrd::Result r1 = m_nrd.SetDenoiserSettings(kSigma, &sigma);
    const nrd::Result r2 = m_nrd.SetDenoiserSettings(kReblur, &reblur);
    if (r0 != nrd::Result::SUCCESS || r1 != nrd::Result::SUCCESS || r2 != nrd::Result::SUCCESS)
        Log("NRD settings rejected: common %d sigma %d reblur %d", static_cast<int>(r0), static_cast<int>(r1), static_cast<int>(r2));

    nrd::ResourceSnapshot snap = {};
    snap.restoreInitialState = true;
    auto setRes = [&](nrd::ResourceType type, ID3D12Resource* r) {
        nrd::Resource res = {};
        res.d3d12.resource = r;
        res.d3d12.format   = static_cast<DXGIFormat>(r->GetDesc().Format);
        res.state          = {nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::Layout::SHADER_RESOURCE_STORAGE, nri::StageBits::COMPUTE_SHADER};
        snap.SetResource(type, res);
    };
    setRes(nrd::ResourceType::IN_MV, m_motion.Get());
    setRes(nrd::ResourceType::IN_NORMAL_ROUGHNESS, m_normalRough.Get());
    setRes(nrd::ResourceType::IN_VIEWZ, m_viewZ.Get());
    setRes(nrd::ResourceType::IN_PENUMBRA, m_penumbra.Get());
    setRes(nrd::ResourceType::OUT_SHADOW_TRANSLUCENCY, m_sigmaOut.Get());
    setRes(nrd::ResourceType::IN_DIFF_RADIANCE_HITDIST, m_diffIn.Get());
    setRes(nrd::ResourceType::IN_SPEC_RADIANCE_HITDIST, m_specIn.Get());
    setRes(nrd::ResourceType::OUT_DIFF_RADIANCE_HITDIST, m_diffOut.Get());
    setRes(nrd::ResourceType::OUT_SPEC_RADIANCE_HITDIST, m_specOut.Get());

    const nrd::Identifier ids[] = {kSigma, kReblur};
    nri::CommandBufferD3D12Desc nriCb = {};
    nriCb.d3d12CommandList      = cl;
    nriCb.d3d12CommandAllocator = fl.alloc.Get();
    m_nrd.DenoiseD3D12(ids, 2, nriCb, snap);
    } else if (rrNow) {
        bindOurs();
        cl->SetPipelineState(m_noisyPso.Get());
        cl->Dispatch((fc.rtSize[0] + 7) / 8, (fc.rtSize[1] + 7) / 8, 1);
        cl->ResourceBarrier(1, &uav);
        ID3D12Resource* rrIn[] = {m_rrColor.Get(), m_rrDiff.Get(), m_rrSpec.Get(), m_rrNormal.Get(), m_rrMv.Get(), m_viewZ.Get()};
        for (ID3D12Resource* r : rrIn) transition(r, kUav, kSrv);
        if (!m_rrHandle || m_rrW != fc.size[0] || m_rrH != fc.size[1]) {
            if (m_rrHandle) { NVSDK_NGX_D3D12_ReleaseFeature(m_rrHandle); m_rrHandle = nullptr; }
            NVSDK_NGX_DLSSD_Create_Params cp = {};
            cp.InDenoiseMode        = NVSDK_NGX_DLSS_Denoise_Mode_DLUnified;
            cp.InRoughnessMode      = NVSDK_NGX_DLSS_Roughness_Mode_Packed;
            cp.InUseHWDepth         = NVSDK_NGX_DLSS_Depth_Type_Linear;
            cp.InWidth  = fc.rtSize[0];
            cp.InHeight = fc.rtSize[1];
            cp.InTargetWidth  = fc.size[0];
            cp.InTargetHeight = fc.size[1];
            cp.InPerfQualityValue   = m_rtScale > 2 ? NVSDK_NGX_PerfQuality_Value_UltraPerformance : m_rtScale > 1 ? NVSDK_NGX_PerfQuality_Value_MaxPerf : NVSDK_NGX_PerfQuality_Value_DLAA;
            cp.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
            const NVSDK_NGX_Result cr = NGX_D3D12_CREATE_DLSSD_EXT(cl, 1, 1, &m_rrHandle, m_ngxParams, &cp);
            Log("DLSS Ray Reconstruction feature %ux%u: 0x%08x", fc.size[0], fc.size[1], static_cast<unsigned>(cr));
            if (NVSDK_NGX_FAILED(cr)) { m_rrHandle = nullptr; m_rrFailed = true; }
            m_rrW = fc.size[0]; m_rrH = fc.size[1];
        }
        if (m_rrHandle) {
            NVSDK_NGX_D3D12_DLSSD_Eval_Params ep = {};
            ep.pInColor          = m_rrColor.Get();
            ep.pInOutput         = m_rrOut.Get();
            ep.pInDepth          = m_viewZ.Get();
            ep.pInMotionVectors  = m_rrMv.Get();
            ep.pInDiffuseAlbedo  = m_rrDiff.Get();
            ep.pInSpecularAlbedo = m_rrSpec.Get();
            ep.pInNormals        = m_rrNormal.Get();
            ep.pInRoughness      = m_rrNormal.Get();
            ep.InRenderSubrectDimensions = {fc.rtSize[0], fc.rtSize[1]};
            ep.InReset           = resetHistory ? 1 : 0;
            ep.InJitterOffsetX   = m_jitterOffset[0];
            ep.InJitterOffsetY   = m_jitterOffset[1];
            ep.InMVScaleX        = 1.0f;
            ep.InMVScaleY        = 1.0f;
            ep.InFrameTimeDeltaInMsec = 16.6f;
            const NVSDK_NGX_Result er = NGX_D3D12_EVALUATE_DLSSD_EXT(cl, m_rrHandle, m_ngxParams, &ep);
            if (NVSDK_NGX_FAILED(er)) { Log("DLSS Ray Reconstruction evaluate failed 0x%08x: back to NRD", static_cast<unsigned>(er)); m_rrFailed = true; }
        }
        for (ID3D12Resource* r : rrIn) transition(r, kSrv, kUav);
    }
    }

    cl->EndQuery(m_timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 6);
    ID3D12Resource* outs[] = {m_sigmaOut.Get(), m_diffOut.Get(), m_specOut.Get()};
    for (ID3D12Resource* r : outs) transition(r, kUav, kSrv);
    transition(m_rrOut.Get(), kUav, kSrv);
    bindOurs();
    cl->SetPipelineState(m_compositePso.Get());
    cl->Dispatch(gx, gy, 1);
    {
        D3D12_RESOURCE_BARRIER eb = {};
        eb.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        eb.UAV.pResource = m_exposureBuf.Get();
        cl->ResourceBarrier(1, &eb);
        cl->SetPipelineState(m_exposurePso.Get());
        cl->Dispatch(1, 1, 1);
        cl->ResourceBarrier(1, &eb);
    }
    for (ID3D12Resource* r : outs) transition(r, kSrv, kUav);
    transition(m_rrOut.Get(), kSrv, kUav);

    for (ID3D12Resource* r : inputs) transition(r, kSrv, kCommon);
    transition(m_output[slot].Get(), kUav, kCommon);

    const bool probe = m_framesDone == 5 || m_framesDone == 60 || m_framesDone == 200;
    struct Probe { const char* name; ID3D12Resource* res; UINT texel; };
    const Probe probes[] = {{"viewZ", m_viewZ.Get(), 4},   {"penumbra", m_penumbra.Get(), 2}, {"sigmaOut", m_sigmaOut.Get(), 2},
                            {"diffIn", m_diffIn.Get(), 8}, {"diffOut", m_diffOut.Get(), 8},   {"specIn", m_specIn.Get(), 8},
                            {"specOut", m_specOut.Get(), 8}, {"normal", m_normalRough.Get(), 4}};
    if (probe) {
        if (!m_probeBuf)
            m_probeBuf = CreateBuffer(m_device.Get(), 512 * 8, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
        UINT i = 0;
        for (const Probe& pr : probes) {
            transition(pr.res, kUav, D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
            dst.pResource = m_probeBuf.Get();
            dst.Type      = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            dst.PlacedFootprint.Offset = i * 512ull;
            dst.PlacedFootprint.Footprint = {pr.res->GetDesc().Format, 1, 1, 1, 256};
            src.pResource = pr.res;
            src.Type      = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            const UINT px = fc.rtSize[0] / 2, py = fc.rtSize[1] * 3 / 4;
            const D3D12_BOX box = {px, py, 0, px + 1, py + 1, 1};
            cl->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
            transition(pr.res, D3D12_RESOURCE_STATE_COPY_SOURCE, kUav);
            ++i;
        }
    }

    memcpy(m_prevViewProj, viewProjCopy, sizeof(m_prevViewProj));
    m_prevCamera   = cam;
    m_havePrev     = true;
    m_historyReset = false;

    cl->EndQuery(m_timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, kGpuMarks - 1);
    cl->ResolveQueryData(m_timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, kGpuMarks, m_timestampReadback.Get(), cur * (kGpuMarks * 8ull));
    if (FAILED(cl->Close())) {
        Fail("command list Close failed");
        return false;
    }

    m_queue->Wait(m_fenceGame.Get(), std::max(gameValue, release));
    ID3D12CommandList* lists[] = {cl};
    m_queue->ExecuteCommandLists(1, lists);
    if (!m_nrWanted || m_nrFailed) m_queue->Signal(m_fenceBridge.Get(), frameId);
    fl.fence = ++m_internalValue;
    m_queue->Signal(m_internalFence.Get(), fl.fence);
    if (m_nrWanted && !m_nrFailed) {
        m_internalFence->SetEventOnCompletion(fl.fence, m_internalEvent);
        WaitForSingleObject(m_internalEvent, 5000);
        NeuralPass(m_output[slot].Get());
        m_queue->Signal(m_fenceBridge.Get(), frameId);
        fl.fence = ++m_internalValue;
        m_queue->Signal(m_internalFence.Get(), fl.fence);
    }

    if (m_framesDone < 3) {
        const DWORD t0 = GetTickCount();
        m_internalFence->SetEventOnCompletion(fl.fence, m_internalEvent);
        const DWORD w = WaitForSingleObject(m_internalEvent, 3000);
        Log("frame %llu: waited game fence %llu (game completed %llu) -> GPU %s in %lu ms, device %08lx",
            frameId, std::max(gameValue, release), m_fenceGame->GetCompletedValue(),
            w == WAIT_OBJECT_0 ? "done" : "NOT DONE", GetTickCount() - t0, m_device->GetDeviceRemovedReason());
    }
    if (probe) {
        m_internalFence->SetEventOnCompletion(fl.fence, m_internalEvent);
        WaitForSingleObject(m_internalEvent, 3000);
        uint8_t* mem = nullptr;
        if (SUCCEEDED(m_probeBuf->Map(0, nullptr, reinterpret_cast<void**>(&mem)))) {
            const auto half = [](uint16_t v) {
                const int e = (v >> 10) & 31, m = v & 1023;
                const float f = e == 0 ? m / 16777216.0f : e == 31 ? 65536.0f : (1.0f + m / 1024.0f) * std::ldexp(1.0f, e - 15);
                return (v & 0x8000) ? -f : f;
            };
            char line[256];
            UINT i = 0;
            for (const Probe& pr : probes) {
                const uint8_t* q = mem + i * 512;
                const auto* u16 = reinterpret_cast<const uint16_t*>(q);
                if (pr.texel == 2) snprintf(line, sizeof(line), "%g", half(u16[0]));
                else if (pr.texel == 8) snprintf(line, sizeof(line), "%g %g %g %g", half(u16[0]), half(u16[1]), half(u16[2]), half(u16[3]));
                else if (i == 0) snprintf(line, sizeof(line), "%g", *reinterpret_cast<const float*>(q));
                else snprintf(line, sizeof(line), "0x%08x", *reinterpret_cast<const uint32_t*>(q));
                Log("probe frame %llu %s = %s", frameId, pr.name, line);
                ++i;
            }
            const D3D12_RANGE none = {0, 0};
            m_probeBuf->Unmap(0, &none);
        }
    }
    MemoryBarrier();
    h.completedFrame          = frameId;
    h.bridgeMeshCount         = static_cast<uint32_t>(m_meshes.size());
    h.bridgeLastInstanceCount = written;
    m_telInstances = written;

    ++m_framesDone;
    if (m_framesDone % 600 == 1)
        Log("frame %llu: %u meshes, %u instances, %.2f ms GPU", frameId, h.bridgeMeshCount, written, h.bridgeGpuMs);
    {
        const uint64_t paceNow = GetTickCount64();
        if (m_paceLast) {
            const uint64_t gap = paceNow - m_paceLast;
            m_paceWorst = std::max(m_paceWorst, gap);
            if (gap > 40) ++m_paceOver40;
            if (gap > 100) ++m_paceOver100;
        } else m_paceStart = paceNow;
        m_paceLast = paceNow;
        if (m_gpuFrames >= 600) {
            const double n = m_gpuFrames;
            Log("Time: %.1f fps, frames over 40 ms %u, over 100 ms %u, worst %llu ms | GPU avg %.2f worst %.2f: build %.2f, SHaRC update %.2f, resolve %.2f, image rays %.2f, air %.2f, denoiser %.2f, composite %.2f | %u instances, %u lights",
                n * 1000.0 / std::max<double>(static_cast<double>(paceNow - m_paceStart), 1.0), m_paceOver40, m_paceOver100, m_paceWorst, m_gpuTotal / n, m_gpuWorst,
                m_gpuSum[1] / n, m_gpuSum[2] / n, m_gpuSum[3] / n, m_gpuSum[4] / n, m_gpuSum[5] / n, m_gpuSum[6] / n, m_gpuSum[7] / n, written, static_cast<uint32_t>(fc.lightCount));
            {
                PROCESS_MEMORY_COUNTERS_EX own = {}, game = {};
                K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&own), sizeof(own));
                if (m_gameProcess) K32GetProcessMemoryInfo(m_gameProcess, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&game), sizeof(game));
                DXGI_QUERY_VIDEO_MEMORY_INFO vram = {};
                ComPtr<IDXGIFactory4> memFactory;
                ComPtr<IDXGIAdapter3> memAdapter;
                const LUID memLuid = m_device->GetAdapterLuid();
                if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&memFactory))) && SUCCEEDED(memFactory->EnumAdapterByLuid(memLuid, IID_PPV_ARGS(&memAdapter))))
                    memAdapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &vram);
                Log("Memory: bridge %llu MB, game %llu MB, bridge video memory %llu MB of %llu MB, %zu meshes", static_cast<unsigned long long>(own.PrivateUsage >> 20), static_cast<unsigned long long>(game.PrivateUsage >> 20),
                    static_cast<unsigned long long>(vram.CurrentUsage >> 20), static_cast<unsigned long long>(vram.Budget >> 20), m_meshes.size());
            }
            Log("Worst GPU frame %.2f: build %.2f, SHaRC update %.2f, resolve %.2f, image rays %.2f, air %.2f, denoiser %.2f, composite %.2f", m_gpuWorst,
                m_gpuWorstParts[1], m_gpuWorstParts[2], m_gpuWorstParts[3], m_gpuWorstParts[4], m_gpuWorstParts[5], m_gpuWorstParts[6], m_gpuWorstParts[7]);
            memset(m_gpuSum, 0, sizeof(m_gpuSum));
            m_gpuTotal = m_gpuWorst = 0.0;
            m_gpuFrames = 0; m_paceOver40 = m_paceOver100 = 0; m_paceWorst = 0; m_paceStart = paceNow;
        }
    }
    CollectRetired();
    return true;
}

DWORD WINAPI Bridge::TelemetryThread(void* self) { static_cast<Bridge*>(self)->TelemetryLoop(); return 0; }

void Bridge::TelemetryLoop()
{
    FILE* f = _wfopen((g_dir + L"..\\telemetry_bridge.csv").c_str(), L"w");
    if (!f) return;
    fprintf(f, "time,rt_on,gpu_frames,gpu_avg_ms,gpu_worst_ms,build,sharc_update,resolve,image_rays,air,denoiser,composite,slots,slot_cpu_ms,slot_worst_ms,list_wait_ms,instances,pstate,clock_mhz,mem_mhz,power_w,temp_c,gpu_util,throttle,video_mb,video_budget_mb,bridge_private_mb,game_private_mb,ram_free_mb\n");
    fflush(f);
    typedef int (*PFN_Init)();
    typedef int (*PFN_Handle)(unsigned, void**);
    typedef int (*PFN_PState)(void*, int*);
    typedef int (*PFN_Clock)(void*, int, unsigned*);
    typedef int (*PFN_Power)(void*, unsigned*);
    typedef int (*PFN_Temp)(void*, int, unsigned*);
    struct Util { unsigned gpu, memory; };
    typedef int (*PFN_Util)(void*, Util*);
    typedef int (*PFN_Throttle)(void*, unsigned long long*);
    HMODULE nvml = LoadLibraryW(L"nvml.dll");
    void* device = nullptr;
    PFN_PState pstateOf = nullptr; PFN_Clock clockOf = nullptr; PFN_Power powerOf = nullptr; PFN_Temp tempOf = nullptr; PFN_Util utilOf = nullptr; PFN_Throttle throttleOf = nullptr;
    if (nvml) {
        auto init = reinterpret_cast<PFN_Init>(GetProcAddress(nvml, "nvmlInit_v2"));
        auto handle = reinterpret_cast<PFN_Handle>(GetProcAddress(nvml, "nvmlDeviceGetHandleByIndex_v2"));
        pstateOf = reinterpret_cast<PFN_PState>(GetProcAddress(nvml, "nvmlDeviceGetPerformanceState"));
        clockOf = reinterpret_cast<PFN_Clock>(GetProcAddress(nvml, "nvmlDeviceGetClockInfo"));
        powerOf = reinterpret_cast<PFN_Power>(GetProcAddress(nvml, "nvmlDeviceGetPowerUsage"));
        tempOf = reinterpret_cast<PFN_Temp>(GetProcAddress(nvml, "nvmlDeviceGetTemperature"));
        utilOf = reinterpret_cast<PFN_Util>(GetProcAddress(nvml, "nvmlDeviceGetUtilizationRates"));
        throttleOf = reinterpret_cast<PFN_Throttle>(GetProcAddress(nvml, "nvmlDeviceGetCurrentClocksThrottleReasons"));
        if (!init || !handle || init() != 0 || handle(0, &device) != 0) device = nullptr;
    }
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter3> adapter;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) factory->EnumAdapterByLuid(m_device->GetAdapterLuid(), IID_PPV_ARGS(&adapter));
    for (;;) {
        Sleep(1000);
        const uint32_t n = m_telFrames, slots = m_telSlots;
        double parts[kGpuMarks] = {}, sum = 0.0;
        for (UINT m = 1; m < kGpuMarks; ++m) { parts[m] = n ? m_telGpu[m] / n : 0.0; sum += parts[m]; m_telGpu[m] = 0.0; }
        const double worst = m_telGpuMax, slotAvg = slots ? m_telSlotMs / slots : 0.0, slotMax = m_telSlotMax, listWait = m_telListWaitMs;
        m_telFrames = 0; m_telSlots = 0; m_telGpuMax = 0.0; m_telSlotMs = 0.0; m_telSlotMax = 0.0; m_telListWaitMs = 0.0;
        int pstate = -1;
        unsigned clock = 0, memClock = 0, power = 0, temp = 0;
        Util util = {};
        unsigned long long throttle = 0;
        if (device) {
            if (pstateOf) pstateOf(device, &pstate);
            if (clockOf) { clockOf(device, 0, &clock); clockOf(device, 2, &memClock); }
            if (powerOf) powerOf(device, &power);
            if (tempOf) tempOf(device, 0, &temp);
            if (utilOf) utilOf(device, &util);
            if (throttleOf) throttleOf(device, &throttle);
        }
        DXGI_QUERY_VIDEO_MEMORY_INFO vram = {};
        if (adapter) adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &vram);
        PROCESS_MEMORY_COUNTERS_EX own = {}, game = {};
        K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&own), sizeof(own));
        if (m_gameProcess) K32GetProcessMemoryInfo(m_gameProcess, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&game), sizeof(game));
        MEMORYSTATUSEX ms = {sizeof(ms)};
        GlobalMemoryStatusEx(&ms);
        SYSTEMTIME st;
        GetLocalTime(&st);
        fprintf(f, "%02d:%02d:%02d,%d,%u,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%u,%.2f,%.2f,%.0f,%u,P%d,%u,%u,%.1f,%u,%u,0x%llx,%llu,%llu,%llu,%llu,%llu\n", st.wHour, st.wMinute, st.wSecond, m_optRT ? 1 : 0, n, sum, worst,
                parts[1], parts[2], parts[3], parts[4], parts[5], parts[6], parts[7], slots, slotAvg, slotMax, listWait, static_cast<uint32_t>(m_telInstances), pstate, clock, memClock, power / 1000.0, temp, util.gpu, throttle,
                static_cast<unsigned long long>(vram.CurrentUsage >> 20), static_cast<unsigned long long>(vram.Budget >> 20), static_cast<unsigned long long>(own.PrivateUsage >> 20), static_cast<unsigned long long>(game.PrivateUsage >> 20), static_cast<unsigned long long>(ms.ullAvailPhys >> 20));
        fflush(f);
    }
}

void Bridge::Run()
{
    Header& h = *g_header;
    {
        HANDLE telemetry = CreateThread(nullptr, 0, &Bridge::TelemetryThread, this, 0, nullptr);
        if (telemetry) CloseHandle(telemetry);
    }
    for (;;) {
        WaitForSingleObject(m_frameEvent, 100);
        h.bridgeHeartbeat++;
        if (h.shutdownRequested) break;
        if (m_gameProcess && WaitForSingleObject(m_gameProcess, 0) == WAIT_OBJECT_0) {
            Log("Game process exited");
            break;
        }
        if (m_pendingScale) {
            Log("Ray resolution: 1/%u -> 1/%u, remaking its targets", m_rtScale, m_pendingScale);
            m_rtScale = m_pendingScale; m_pendingScale = 0;
            m_generation = 0; m_rrW = 0; m_rrFailed = false; m_historyReset = true; m_resetExposure = true;
        }
        if (h.resourceGeneration != m_generation && !OpenSharedResources()) continue;
        if (m_generation == 0) continue;

        for (;;) {
            FrameSlot* next = nullptr;
            for (FrameSlot& s : h.slots)
                if (s.state == kSlotReady && (!next || s.frameId < next->frameId)) next = &s;
            if (!next) break;
            next->state = kSlotProcessing;
            MemoryBarrier();
            LARGE_INTEGER slotStart, slotEnd, slotFreq;
            QueryPerformanceFrequency(&slotFreq);
            QueryPerformanceCounter(&slotStart);
            if (!ProcessSlot(*next)) return;
            QueryPerformanceCounter(&slotEnd);
            {
                const double slotMs = (slotEnd.QuadPart - slotStart.QuadPart) * 1000.0 / slotFreq.QuadPart;
                m_telSlotMs += slotMs;
                m_telSlotMax = std::max(static_cast<double>(m_telSlotMax), slotMs);
                ++m_telSlots;
            }
        }
        if (m_device->GetDeviceRemovedReason() != S_OK) {
            Fail("D3D12 device removed (0x%08lX)", m_device->GetDeviceRemovedReason());
            return;
        }
    }
    h.bridgeState = 3;
}

void Bridge::UpdateOptions()
{
        if (getenv("BULLYRT_TEST_NOKEYS")) return;
    const bool mods = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_MENU) & 0x8000);
    int* const flags[4] = {&m_optRT, &m_optReflections, &m_optSunShadows, &m_optLocalShadows};
    static const wchar_t* const names[5] = {L"RayTracing", L"Reflections", L"SunShadows", L"LocalShadows", L"Lighting"};
    const std::wstring ini = g_dir + L"..\\..\\BullyDX12.ini";
    const bool f9 = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
    if (f9 && !m_f9Down) {
        m_optRT = !m_optRT;
        WritePrivateProfileStringW(L"RTOptions", L"RayTracing", m_optRT ? L"1" : L"0", ini.c_str());
        m_historyReset = true;
        m_osdUntil = GetTickCount64() + 3000;
        Log("Option RayTracing = %d (F9)", m_optRT);
        ShowText(m_optRT ? "RT LIGADO" : "RT DESLIGADO - JOGO ORIGINAL");
    }
    m_f9Down = f9;
    if (g_header) g_header->rtDisabled = m_optRT ? 0u : 1u;
    const int volDown = mods && (GetAsyncKeyState('6') & 0x8000) ? 1 : 0;
    if (volDown && !m_optVolKey) {
        m_optVolumetric = !m_optVolumetric;
        WritePrivateProfileStringW(L"RTOptions", L"Volumetric", m_optVolumetric ? L"1" : L"0", ini.c_str());
        Log("Option Volumetric = %d", m_optVolumetric);
        ShowText(m_optVolumetric ? "LUZ VOLUMETRICA: ON" : "LUZ VOLUMETRICA: OFF");
    }
    m_optVolKey = volDown;
    const bool rrDown = mods && (GetAsyncKeyState('7') & 0x8000);
    if (rrDown && !m_rrKey) {
        ShowText(m_rrReady ? (m_rrFailed ? "DLSS RAY RECONSTRUCTION FALHOU" : "DENOISER: DLSS RAY RECONSTRUCTION") : "DENOISER: NVIDIA NRD (SEM RR)");
    }
    m_rrKey = rrDown;
    for (int i = 0; i < 5; ++i) {
        const bool down = mods && (GetAsyncKeyState('1' + i) & 0x8000);
        if (down && !m_optKeyDown[i]) {
            int value;
            if (i < 4) { *flags[i] = !*flags[i]; value = *flags[i]; }
            else { m_optLighting = (m_optLighting + 1) % 4; value = m_optLighting; }
            WritePrivateProfileStringW(L"RTOptions", names[i], std::to_wstring(value).c_str(), ini.c_str());
            m_historyReset = true;
            Log("Option %ls = %d", names[i], value);
            static const char* const kOpt[4] = {"RAY TRACING", "REFLEXOS RT", "SOMBRA DO SOL RT", "SOMBRAS LOCAIS RT"};
            static const char* const kLevel[4] = {"OFF", "MEDIUM", "ULTRA", "PSYCHO"};
            ShowText(i < 4 ? std::string(kOpt[i]) + (value ? ": ON" : ": OFF") : std::string("ILUMINACAO RT: ") + kLevel[value]);
        }
        if (down) m_osdUntil = GetTickCount64() + 3000;
        m_optKeyDown[i] = down;
    }
}

void Bridge::LoadMaterials()
{
    m_materialsLoaded = true;
    FILE* f = _wfopen((g_dir + L"materials.ini").c_str(), L"r");
    if (!f) { Log("materials.ini not found: every material is the default one"); return; }
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        unsigned hash = 0;
        Material m;
        if (line[0] == '#' || sscanf_s(line, "%x %f %f %f %f %f %d %d", &hash, &m.emissive, &m.lampPower, &m.col[0], &m.col[1], &m.col[2], &m.glass, &m.night) < 3) continue;
        m_materials[hash] = m;
    }
    fclose(f);
    Log("materials.ini: %zu materials", m_materials.size());
}

void Bridge::ReadSettings()
{
    m_settingsRead = true;
    const std::wstring ini = g_dir + L"..\\..\\BullyDX12.ini";
    auto read = [&](const wchar_t* sec, const wchar_t* key, float def) {
        wchar_t buf[64] = {};
        if (!GetPrivateProfileStringW(sec, key, L"", buf, 64, ini.c_str()) || !buf[0]) return def;
        return static_cast<float>(_wtof(buf));
    };
    m_expoSettings[0] = read(L"Exposure", L"Key", m_expoSettings[0]);
    m_expoSettings[1] = read(L"Exposure", L"Min", m_expoSettings[1]);
    m_expoSettings[2] = read(L"Exposure", L"Max", m_expoSettings[2]);
    m_expoSettings[3] = read(L"Exposure", L"Speed", m_expoSettings[3]);
    {
        uint32_t scale = read(L"RaytracingBridge", L"HalfResolution", 1.f) != 0.f ? 2u : 1u;
        scale = static_cast<uint32_t>(std::clamp(read(L"RaytracingBridge", L"RtScale", static_cast<float>(scale)), 1.f, 3.f));
        if (m_generation != 0 && scale != m_rtScale) m_pendingScale = scale;
        else m_rtScale = scale;
    }
    m_optRT           = read(L"RTOptions", L"RayTracing", 1.f) != 0.f;
    m_optReflections  = read(L"RTOptions", L"Reflections", 1.f) != 0.f;
    m_optSunShadows   = read(L"RTOptions", L"SunShadows", 1.f) != 0.f;
    m_optLocalShadows = read(L"RTOptions", L"LocalShadows", 1.f) != 0.f;
    m_optVolumetric   = read(L"RTOptions", L"Volumetric", 1.f) != 0.f;
    m_optLighting     = std::clamp(static_cast<int>(read(L"RTOptions", L"Lighting", 2.f)), 0, 3);
    m_windowPower = read(L"Light", L"WindowPower", m_windowPower);
    m_imageGain = read(L"Light", L"Brightness", m_imageGain);
    m_fog[0] = read(L"Light", L"FogStart", m_fog[0]);
    m_fog[1] = read(L"Light", L"FogDensity", m_fog[1]);
    m_bounceColor = read(L"Light", L"BounceColor", m_bounceColor);
    m_beams = read(L"Light", L"VolumetricStrength", m_beams);
    m_beamsOutside = read(L"Light", L"VolumetricOutdoor", m_beamsOutside);
    m_lampFalloff = read(L"Light", L"LampFalloff", m_lampFalloff);
    m_sharcScale = read(L"Light", L"SharcScale", m_sharcScale);
    m_sharcTile = read(L"Light", L"SharcTile", m_sharcTile);
    m_sharcAccumulate = read(L"Light", L"SharcAccumulate", m_sharcAccumulate);
    m_windowsPerRay = read(L"Light", L"WindowsPerRay", m_windowsPerRay);
    m_sharcStale = read(L"Light", L"SharcStale", m_sharcStale);
    m_sharcSurfaces = read(L"Light", L"SharcSurfaces", m_sharcSurfaces);
    m_lampReach = read(L"Light", L"LampReach", m_lampReach);
    m_textureLamps = read(L"Light", L"TextureLamps", m_textureLamps);
    m_emitLampPower = read(L"Light", L"EmitLampPower", m_emitLampPower);
    m_emitGlowSent = read(L"Light", L"EmitGlow", m_emitGlowSent);
    m_gsaNearLamp = read(L"Light", L"GameLightNearLamp", m_gsaNearLamp);
    m_catalogPower = read(L"Light", L"FixturePower", m_catalogPower);
    {
        const bool want = read(L"RTOptions", L"Denoiser", m_rrWanted ? 1.f : 0.f) != 0.f;
        const bool now = m_rrReady && want;
        if (now != m_rrNow) { m_rrWanted = want; m_rrNow = now; m_historyReset = true; m_resetExposure = true; m_rrW = 0; m_rrFailed = false; Log("Denoiser: %s", now ? "DLSS Ray Reconstruction" : "NRD"); }
    }
    m_fixtureReach = read(L"Light", L"FixtureReach", m_fixtureReach);
    m_lampsOn = static_cast<int>(read(L"Light", L"LampsOnHour", static_cast<float>(m_lampsOn)));
    m_lampsOff = static_cast<int>(read(L"Light", L"LampsOffHour", static_cast<float>(m_lampsOff)));
    if (m_lampsOn <= 0) { m_lampsOn = 19; m_lampsOff = 7; }
    m_gameArea = read(L"Light", L"GameArea", m_gameArea);
    m_gameHour = read(L"Light", L"GameHour", m_gameHour);
    m_gameLightPower = read(L"Light", L"GameLightPower", m_gameLightPower);
    m_gameDaylight = read(L"Light", L"GameDaylightLights", m_gameDaylight);
    m_roomSunOn = read(L"Light", L"RoomSun", m_roomSunOn);
    m_roomSunPower = read(L"Light", L"RoomSunPower", m_roomSunPower);
    m_decalGlow = read(L"Light", L"DecalGlow", m_decalGlow);
    m_restirOn = read(L"Light", L"Restir", m_restirOn);
    m_restirSamples = read(L"Light", L"RestirSamples", m_restirSamples);
    m_restirHistory = read(L"Light", L"RestirHistory", m_restirHistory);
    m_restirRadius = read(L"Light", L"RestirRadius", m_restirRadius);
    m_restirNeighbors = read(L"Light", L"RestirNeighbors", m_restirNeighbors);
    m_restirPermute = read(L"Light", L"RestirPermute", m_restirPermute);
    m_rrJitter = read(L"Light", L"RrJitter", m_rrJitter);
    m_aoReach = read(L"Light", L"AoReach", m_aoReach);
    m_aoStrength = read(L"Light", L"AoStrength", m_aoStrength);
    m_volDensity = read(L"Light", L"VolumetricDensity", m_volDensity);
    m_volDensityOut = read(L"Light", L"VolumetricDensityOutdoor", m_volDensityOut);
    m_volLocal = read(L"Light", L"VolumetricScattering", m_volLocal);
    m_volAmbient = read(L"Light", L"VolumetricAmbient", m_volAmbient);
    m_rrJitterSign = read(L"Light", L"RrJitterSign", m_rrJitterSign);
    m_decalLight = read(L"Light", L"DecalLight", m_decalLight);
    m_gameLightAmbient = read(L"Light", L"GameLightAmbient", m_gameLightAmbient);
    m_roomFill = read(L"Light", L"RoomFill", m_roomFill);
    m_samplePeriod = read(L"Light", L"SamplePeriod", m_samplePeriod);
    m_lampTint[0] = read(L"Light", L"LampTintR", m_lampTint[0]);
    m_lampTint[1] = read(L"Light", L"LampTintG", m_lampTint[1]);
    m_lampTint[2] = read(L"Light", L"LampTintB", m_lampTint[2]);
    m_windowLight[0] = read(L"Light", L"WindowLightR", m_windowLight[0]);
    m_windowLight[1] = read(L"Light", L"WindowLightG", m_windowLight[1]);
    m_windowLight[2] = read(L"Light", L"WindowLightB", m_windowLight[2]);
    m_sunTint[0] = read(L"Light", L"SunTintR", m_sunTint[0]);
    m_sunTint[1] = read(L"Light", L"SunTintG", m_sunTint[1]);
    m_sunTint[2] = read(L"Light", L"SunTintB", m_sunTint[2]);
    Log("Exposure key %.2f, %.2f..%.2f; lamp tint %.2f %.2f %.2f", m_expoSettings[0], m_expoSettings[1], m_expoSettings[2], m_lampTint[0], m_lampTint[1], m_lampTint[2]);
}

void Bridge::LoadLampCatalog()
{
    m_catalogRead = true;
    FILE* f = _wfopen((g_dir + L"lamps.dat").c_str(), L"rb");
    if (!f) { Log("lamps.dat not found: no light fixtures (run tools/lampcatalog.py)"); return; }
    char magic[4] = {};
    uint32_t version = 0, count = 0;
    if (fread(magic, 4, 1, f) == 1 && memcmp(magic, "BLMP", 4) == 0 && fread(&version, 4, 1, f) == 1 && version == 1 && fread(&count, 4, 1, f) == 1 && count < 1000000) {
        m_catalog.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            struct Rec { float pos[3], n[3], area, col[3]; int32_t areaId; uint32_t flags, model; } r;
            static_assert(sizeof(Rec) == 52, "lamps.dat record");
            if (fread(&r, sizeof(r), 1, f) != 1) break;
            CatalogLamp l = {};
            memcpy(l.pos, r.pos, 12); memcpy(l.n, r.n, 12); memcpy(l.col, r.col, 12);
            l.area_m2 = r.area; l.area = r.areaId; l.flags = r.flags; l.model = r.model;
            l.flickOn = true; l.flickAt = 0;
            m_catalog.push_back(l);
        }
    }
    fclose(f);
    Log("lamps.dat: %zu light fixtures", m_catalog.size());
}

void Bridge::LoadGameLights()
{
    m_gameLightsRead = true;
    FILE* f = _wfopen((g_dir + L"..\\..\\Graphics\\gsa_info.dat").c_str(), L"rb");
    if (!f) { Log("Graphics/gsa_info.dat not found: no light components of the game"); return; }
    int files = 0, total = 0;
    if (fread(&files, 4, 1, f) != 1) files = 0;
    for (int i = 0; i < files; ++i) {
        int area = -1, count = 0;
        char path[64];
        if (fread(&area, 4, 1, f) != 1 || fread(path, 64, 1, f) != 1 || fread(&count, 4, 1, f) != 1 || count < 0) break;
        std::vector<GameLight> day;
        struct Night { char suffix[64]; float dif[3], amb[3], dim; };
        std::vector<Night> nights;
        for (int k = 0; k < count; ++k) {
            uint8_t r[224];
            if (fread(r, 224, 1, f) != 1) break;
            const auto F = [&](size_t o) { float v; memcpy(&v, r + o, 4); return v; };
            int type = 0;
            memcpy(&type, r + 92, 4);
            if (type != 1 && type != 2) continue;
            r[91] = 0;
            const char* name = reinterpret_cast<const char*>(r + 28);
            if (r[0x19] == 0) {
                const char* mark = strstr(name, "_N_");
                if (!mark) continue;
                Night ni = {};
                strncpy_s(ni.suffix, mark + 3, _TRUNCATE);
                for (int c = 0; c < 3; ++c) { ni.dif[c] = F(108 + c * 4); ni.amb[c] = F(96 + c * 4); }
                ni.dim = F(196);
                nights.push_back(ni);
                continue;
            }
            GameLight g = {};
            strncpy_s(g.name, name, _TRUNCATE);
            for (int c = 0; c < 3; ++c) {
                g.dif[c] = g.nightDif[c] = F(108 + c * 4);
                g.amb[c] = g.nightAmb[c] = F(96 + c * 4);
                g.pos[c] = F(168 + c * 4);
                g.att[c] = F(184 + c * 4);
                g.dir[c] = F(132 + c * 12);
            }
            g.dim = g.nightDim = F(196);
            g.zone = F(0);
            g.spot = type == 1;
            const float rad = 3.14159265f / 180.0f;
            g.cosOuter = g.spot ? std::cos(std::min(F(200) * 1.25f, 89.0f) * rad) : -2.0f;
            g.cosInner = g.spot ? std::cos(F(204) * 0.4f * rad) : -2.0f;
            g.cutscene = r[0x11] != 0;
            g.flicker = r[0x12] != 0;
            g.flickOn = true;
            g.daylight = strstr(g.name, "_D_") != nullptr && g.spot && g.dir[2] <= -0.05f && g.dir[2] >= -0.95f;
            day.push_back(g);
        }
        for (const Night& ni : nights)
            for (GameLight& g : day)
                if (strstr(g.name, ni.suffix)) {
                    memcpy(g.nightDif, ni.dif, sizeof(g.nightDif)); memcpy(g.nightAmb, ni.amb, sizeof(g.nightAmb)); g.nightDim = ni.dim;
                    const bool fill = g.spot && (g.dir[2] < -0.95f || g.dir[2] > -0.05f);
                    g.daylight = !fill;
                    break;
                }
        total += static_cast<int>(day.size());
        if (area >= 0 && area < 64) m_gameLights[area] = std::move(day);
    }
    fclose(f);
    Log("Game lights: %d components in %d areas", total, files);
}

bool Bridge::UseGameLights(FrameConstants& fc, const FrameSlot& s)
{
    if (!m_gameLightsRead) LoadGameLights();
    const uint32_t state = s.reserved2;
    const int area = m_gameArea >= 0.0f ? static_cast<int>(m_gameArea) : static_cast<int>(state & 0xff) - 1;
    const int hour = m_gameHour >= 0.0f ? static_cast<int>(m_gameHour) : static_cast<int>((state >> 8) & 0xff);
    const bool cutscene = ((state >> 16) & 1) != 0;
    const bool outdoors = area == 0 || area == 1 || area == 22 || area == 31 || area == 42 || area == 43;
    const bool use = area >= 0 && area < 64 && !outdoors && !m_gameLights[area].empty();
    if (area != m_gameLightsArea) {
        m_gameLightsArea = area;
        m_roomSunLight = nullptr;
        m_haveRoomSun = false;
        Log("Game area %d, hour %d: %s, %zu light components", area, hour, use ? "using them" : "not used", area >= 0 && area < 64 ? m_gameLights[area].size() : size_t(0));
        Log("Sun colour %.2f %.2f %.2f, clouds %.2f, sky %.2f %.2f %.2f, strength %.2f %.2f %.2f %.2f", fc.sunColor[0], fc.sunColor[1], fc.sunColor[2], s.clouds[0], fc.skyColor[0], fc.skyColor[1], fc.skyColor[2], fc.strength[0], fc.strength[1], fc.strength[2], fc.strength[3]);
    }
    if (!use) return false;
    const bool night = hour >= 19 || hour < 7;
    const uint64_t now = GetTickCount64();
    struct Pick { float power; GameLight* g; };
    std::vector<Pick> picks;
    for (GameLight& g : m_gameLights[area]) {
        if (g.cutscene && !cutscene) continue;
        picks.push_back({(g.dif[0] * 0.3f + g.dif[1] * 0.6f + g.dif[2] * 0.1f + g.amb[0] * 0.3f + g.amb[1] * 0.6f + g.amb[2] * 0.1f) * g.dim, &g});
    }
    std::stable_sort(picks.begin(), picks.end(), [](const Pick& a, const Pick& b) { return a.power > b.power; });
    fc.roomAmbient[3] = m_gameLightAmbient;
    const uint32_t sent = ((state >> 24) & 1u) ? std::min<uint32_t>(s.lightCount, kMaxLights) : 0u;
    const uint32_t keep = std::min<uint32_t>(sent, 8u);
    const uint32_t n = static_cast<uint32_t>(std::min<size_t>(picks.size(), kMaxLights - keep));
    for (uint32_t i = 0; i < n; ++i) {
        GameLight& g = *picks[i].g;
        if (g.flicker && now >= g.flickAt) {
            g.flickOn = !g.flickOn;
            const uint64_t span = g.flickOn ? 200 + rand() % 1800 : 30 + rand() % 270;
            g.flickAt = now + span / (1 + rand() % 4);
        }
        const float power = (night ? g.nightDim : g.dim) * m_gameLightPower * (g.flicker && !g.flickOn ? 0.25f : 1.0f);
        float reach = m_lampReach;
        if (g.att[2] > 1e-6f) reach = std::min(reach, std::sqrt(100.0f / g.att[2]));
        else if (g.att[1] > 1e-6f) reach = std::min(reach, 100.0f / g.att[1]);
        for (int c = 0; c < 3; ++c) {
            fc.lightPos[i][c] = g.pos[c];
            fc.lightCol[i][c] = (night ? g.nightDif[c] : g.dif[c]) * power;
            fc.lightDir[i][c] = g.dir[c];
            fc.lightAtt[i][c] = g.att[c];
        }
        fc.lightPos[i][3] = reach;
        for (const auto& e : m_emitNear) {
            const float ex = e[0] - g.pos[0], ey = e[1] - g.pos[1], ez = e[2] - g.pos[2];
            if (ex * ex + ey * ey + ez * ez < 36.0f) { for (int c = 0; c < 3; ++c) fc.lightCol[i][c] *= m_gsaNearLamp; break; }
        }
        if (g.daylight && m_gameDaylight <= 0.0f) {
            for (int c = 0; c < 3; ++c) fc.lightCol[i][c] = 0.0f;
            fc.lightPos[i][3] = 0.001f;
        }
        fc.lightCol[i][3] = g.cosOuter;
        fc.lightDir[i][3] = g.cosInner;
        const float ambientPower = night ? g.nightDim : g.dim;
        const auto q = [](float v) { return static_cast<uint32_t>(std::clamp(static_cast<int>(v * 32.0f + 0.5f), 0, 63)); };
        const float* ambientColour = night ? g.nightAmb : g.amb;
        const uint32_t zone = g.zone <= 1.0f ? 0u : static_cast<uint32_t>(std::clamp(static_cast<int>(std::ceil(g.zone / 2.0f)), 1, 63));
        fc.lightAtt[i][3] = static_cast<float>(1u + (q(ambientColour[0] * ambientPower) | (q(ambientColour[1] * ambientPower) << 6) | (q(ambientColour[2] * ambientPower) << 12) | (zone << 18)));
    }
    uint32_t total = n;
    for (uint32_t j = 0; j < sent && total < kMaxLights; ++j) {
        bool same = false;
        for (uint32_t i = 0; i < n && !same; ++i) {
            const float dx = fc.lightPos[i][0] - s.lightPos[j][0], dy = fc.lightPos[i][1] - s.lightPos[j][1], dz = fc.lightPos[i][2] - s.lightPos[j][2];
            same = dx * dx + dy * dy + dz * dz < 4.0f;
        }
        if (same) continue;
        memcpy(fc.lightPos[total], s.lightPos[j], 16); memcpy(fc.lightCol[total], s.lightCol[j], 16);
        memcpy(fc.lightDir[total], s.lightDir[j], 16); memcpy(fc.lightAtt[total], s.lightAtt[j], 16);
        ++total;
    }
    fc.lightCount = static_cast<float>(total);
    if (m_roomSunOn > 0.0f) {
        const auto distance2 = [&](const GameLight& g) {
            const float dx = g.pos[0] - s.cameraPos[0], dy = g.pos[1] - s.cameraPos[1], dz = g.pos[2] - s.cameraPos[2];
            const bool named = strstr(g.name, "Sun") || strstr(g.name, "sun") || strstr(g.name, "SUN");
            return (dx * dx + dy * dy + dz * dz) * (named ? 0.01f : 1.0f);
        };
        const GameLight* best = nullptr;
        float bestD2 = 1e30f;
        for (const GameLight& g : m_gameLights[area]) {
            if (!g.daylight || !g.spot || g.cutscene || g.dir[2] > -0.05f || g.dir[2] < -0.95f) continue;
            const float d2 = distance2(g);
            if (d2 < bestD2) { bestD2 = d2; best = &g; }
        }
        if (best && (!m_roomSunLight || bestD2 < distance2(*m_roomSunLight) * 0.5f)) m_roomSunLight = best;
        if (m_roomSunLight) {
            const float* to = m_roomSunLight->dir;
            const float blend = m_haveRoomSun ? 0.08f : 1.0f;
            float len = 0.0f;
            for (int c = 0; c < 3; ++c) { m_roomSun[c] += (-to[c] - m_roomSun[c]) * blend; len += m_roomSun[c] * m_roomSun[c]; }
            len = std::sqrt(std::max(len, 1e-8f));
            for (int c = 0; c < 3; ++c) { m_roomSun[c] /= len; fc.sunDir[c] = m_roomSun[c]; fc.sunColor[c] *= m_roomSunPower; }
            m_haveRoomSun = true;
        }
    }
    return true;
}

const std::vector<Bridge::LampCluster>& Bridge::LampClustersOf(uint32_t meshId, uint32_t textureId)
{
    const uint64_t key = (static_cast<uint64_t>(meshId) << 32) | textureId;
    auto found = m_lampClusters.find(key);
    if (found != m_lampClusters.end()) return found->second;
    static const std::vector<LampCluster> kNone;
    const bool whole = textureId == 0;
    auto mi = m_cpuMeshes.find(meshId);
    auto ti = m_cpuTex.find(textureId);
    if (mi == m_cpuMeshes.end() || (!whole && (ti == m_cpuTex.end() || ti->second.rgba.empty()))) return kNone;
    std::vector<LampCluster>& out = m_lampClusters[key];
    const CpuMesh& cm = mi->second;
    static const CpuTex kNoTex;
    const CpuTex&  ct = whole ? kNoTex : ti->second;
    for (size_t k = 0; k + 2 < cm.idx.size(); k += 3) {
        const uint32_t i0 = cm.idx[k], i1 = cm.idx[k + 1], i2 = cm.idx[k + 2];
        if (static_cast<size_t>(std::max({i0, i1, i2})) * 3 + 2 >= cm.pos.size()) continue;
        if (!whole && static_cast<size_t>(std::max({i0, i1, i2})) * 2 + 1 >= cm.uv.size()) continue;
        const float* p0 = &cm.pos[i0 * 3]; const float* p1 = &cm.pos[i1 * 3]; const float* p2 = &cm.pos[i2 * 3];
        const float e1[3] = {p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]}, e2[3] = {p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2]};
        const float cx = e1[1] * e2[2] - e1[2] * e2[1], cy = e1[2] * e2[0] - e1[0] * e2[2], cz = e1[0] * e2[1] - e1[1] * e2[0];
        const float area = std::sqrt(cx * cx + cy * cy + cz * cz) * 0.5f;
        if (area < 1e-4f) continue;
        if (!whole) {
            const float u = (cm.uv[i0 * 2] + cm.uv[i1 * 2] + cm.uv[i2 * 2]) / 3.f, v = (cm.uv[i0 * 2 + 1] + cm.uv[i1 * 2 + 1] + cm.uv[i2 * 2 + 1]) / 3.f;
            const uint32_t tx = static_cast<uint32_t>((u - std::floor(u)) * ct.w) % ct.w, ty = static_cast<uint32_t>((v - std::floor(v)) * ct.h) % ct.h;
            const uint8_t* c = &ct.rgba[(static_cast<size_t>(ty) * ct.w + tx) * 4];
            if (!ct.glass && !ct.glowTex && c[0] * 0.3f + c[1] * 0.6f + c[2] * 0.1f < 170.f) continue;
        }
        if (ct.glass && std::fabs(cz) > 2.0f * area * 0.6f) continue;
        const float m[3] = {(p0[0] + p1[0] + p2[0]) / 3.f, (p0[1] + p1[1] + p2[1]) / 3.f, (p0[2] + p1[2] + p2[2]) / 3.f};
        LampCluster* into = nullptr;
        for (LampCluster& lc : out) {
            const float dx = (lc.lo[0] + lc.hi[0]) * 0.5f - m[0], dy = (lc.lo[1] + lc.hi[1]) * 0.5f - m[1], dz = (lc.lo[2] + lc.hi[2]) * 0.5f - m[2];
            if (dx * dx + dy * dy + dz * dz < 2.25f) { into = &lc; break; }
        }
        if (!into) {
            out.push_back({{1e30f, 1e30f, 1e30f}, {-1e30f, -1e30f, -1e30f}, 0.f, {0.f, 0.f, 0.f}});
            into = &out.back();
        }
        into->area += area;
        const float align = into->n[0] * cx + into->n[1] * cy + into->n[2] * cz < 0.f ? -1.f : 1.f;
        into->n[0] += cx * align; into->n[1] += cy * align; into->n[2] += cz * align;
        for (const float* p : {p0, p1, p2})
            for (int r = 0; r < 3; ++r) { into->lo[r] = std::min(into->lo[r], p[r]); into->hi[r] = std::max(into->hi[r], p[r]); }
    }
    return out;
}

}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR cmdLine, int)
{
    int    argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    (void)cmdLine;

    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring logPath(exe);
    g_dir   = logPath.substr(0, logPath.find_last_of(L'\\') + 1);
    logPath = g_dir + L"BullyRTBridge.log";
    g_log = _wfsopen(logPath.c_str(), L"w", _SH_DENYWR);

    if (argc < 2) {
        Log("usage: BullyRTBridge.exe <mapping name>");
        return 2;
    }
    Log("BullyRTBridge starting, mapping %ls", argv[1]);
    SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);

    Bridge b;
    if (!b.Init(argv[1])) {
        if (g_header && g_header->bridgeState != 2) {
            strncpy_s(g_header->bridgeMessage, "initialization failed, see BullyRTBridge.log", _TRUNCATE);
            g_header->bridgeState = 2;
        }
        return 1;
    }
    b.Run();
    Log("BullyRTBridge exiting");
    if (g_log) fclose(g_log);
    return 0;
}
