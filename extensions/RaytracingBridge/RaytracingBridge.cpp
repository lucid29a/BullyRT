// BullyDX12 RaytracingBridge extension (runs inside the 32-bit game).

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "FrameCapture.h"
#include <windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "BullyDX12/BullyDX12.h"
#include "../../bridge/BridgeProtocol.h"
#include "ShaderPatcher.h"
#include "GameCulling.h"
#include "DrawDistance.h"
#include "Pools.h"
#include "GameLights.h"
#include "Menu.h"
#include "RtCycle.h"
#include "GameErrors.h"

#include "Composite_VSMain.h"
#include "Composite_PSDepth.h"
#include "Composite_PSComposite.h"
#include "Composite_PSVis.h"

using Microsoft::WRL::ComPtr;
using namespace bdx12;
namespace br = bdx12::bridge;

namespace {

DXGI_FORMAT TypedFormat(DXGI_FORMAT f)
{
    switch (f) {
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:     return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:     return DXGI_FORMAT_B8G8R8X8_UNORM;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:     return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:  return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default:                                return f;
    }
}

struct MeshKey {
    IDirect3DVertexBuffer9* vb;
    IDirect3DIndexBuffer9*  ib;
    UINT  vbOffset, stride, posOffset;
    INT   baseVertex;
    UINT  firstVertex, numVertices, startIndex, primCount, primType;
    bool operator==(const MeshKey& o) const { return memcmp(this, &o, sizeof(MeshKey)) == 0; }
};
struct MeshKeyHash {
    size_t operator()(const MeshKey& k) const
    {
        const auto* p = reinterpret_cast<const uint8_t*>(&k);
        uint32_t h = 2166136261u;
        for (size_t i = 0; i < sizeof(MeshKey); ++i) { h ^= p[i]; h *= 16777619u; }
        return h;
    }
};
struct MeshEntry {
    uint32_t             id = 0;
    bool                 sent = false;
    uint64_t             lastUsed = 0;
    uint32_t             vertexCount = 0, indexCount = 0;
    std::vector<uint8_t> data;
    float                bmin[3] = {1e30f, 1e30f, 1e30f}, bmax[3] = {-1e30f, -1e30f, -1e30f};
    struct Placed {
        float    transform[3][4];
        uint64_t lastSeen;
        uint64_t firstSeen;
        uint32_t hiddenInView;
        uint32_t textureId, flags;
        float    alphaRef;
    };
    std::vector<Placed>  placed;
    bool                 moves = false;
    uint32_t             moveCount = 0;
    uint64_t             moveSeen = 0;
};

struct SkinMesh {
    std::vector<float>    pos, uv, weights;
    std::vector<uint32_t> colors;
    std::vector<uint16_t> bones;
    std::vector<uint32_t> tris;
    uint64_t              lastUsed = 0;
    bool                  bad = false;
};

struct VsInfo {
    int  world = -1, viewProj = -1, camera = -1, bone = -1, boneCount = 0;
    bool skinned = false;
    bool outPos1 = false, outNrm4 = false;
};

bool CameraFromViewProj(const float m[4][4], float c[3]);

struct PsShadowInfo {
    int sampler = -1, matrix = -1, bias = -1, lightPos = -1, sunColor = -1, ambient = -1, base = -1;
    bool Valid() const { return sampler >= 0 && sampler < 8 && matrix >= 0 && bias >= 0; }
};

enum CaptureSkip {
    kSkipNoWorld, kSkipSkinned, kSkipFew, kSkipNoZWrite, kSkipBlend, kSkipAlphaTest, kSkipPrimType, kSkipUP,
    kSkipNoDecl, kSkipPosType, kSkipBudget, kSkipVbPool, kSkipIbPool, kSkipRange, kSkipLock, kSkipIndex, kSkipEmpty,
    kSkipTooBig, kSkipFull, kSkipNoColor, kSkipCount
};
const char* const kSkipNames[kSkipCount] = {
    "no mWorld", "skinned", "<MinPrimitives", "no Z write", "alpha blend", "alpha test", "prim type", "UP draw",
    "no decl", "pos format", "budget/frame", "VB pool", "IB pool", "range", "lock", "bad index", "degenerate",
    "too big", "instances full", "no colour write"};

struct Params {
    float aoStrength, shadowStrength, shadowAmbient, debug;
    float targetWidth, targetHeight, pad0, pad1;
};

class RaytracingBridge final : public IExtension {
public:
    const char* GetName() const override { return "RaytracingBridge"; }
    int         GetPriority() const override { return 40; }

    bool OnInitialize(IHost* host) override;
    void OnShutdown() override;
    void OnDeviceCreated(const DeviceInfo& info) override { m_bbWidth = info.presentParams.BackBufferWidth; m_bbHeight = info.presentParams.BackBufferHeight; }
    void OnDeviceReset(const DeviceInfo& info) override { OnDeviceCreated(info); }
    void OnDeviceLost() override;
    DrawResult OnDraw(const DrawCallInfo& d) override;
    void OnPresent(const FrameInfo& frame) override;
    void OnClear(DWORD flags, IDirect3DSurface9* renderTarget0, IDirect3DSurface9* depthStencil) override;
    void OnCopySurface(IDirect3DSurface9* source, IDirect3DSurface9* dest) override;

private:
    bool StartBridge();
    bool CreateSharedResources(UINT w, UINT h);
    bool EnsurePipelines(DXGI_FORMAT targetFormat);
    bool EnsureDepthCopy(const D3D12_RESOURCE_DESC& dsDesc);
    std::wstring Name(const std::wstring& what) const { return m_mappingName + L"_" + what; }
    HANDLE ShareObject(ID3D12DeviceChild* obj, const std::wstring& what);

    const VsInfo& VsInfoFor(uint64_t hash);
    int           SunRegisterFor(uint64_t psHash);
    const PsShadowInfo& PsShadowFor(uint64_t psHash);
    bool          EnsureVisTexture(UINT w, UINT h);
    bool          ChooseResources();
    struct PatchedVSInfo {
        ComPtr<IDirect3DVertexShader9> shader;
        bool                           hasNormal = false;
    };
    const PatchedVSInfo&    PatchedVS(uint64_t hash, int worldReg);
    IDirect3DPixelShader9*  PatchedPS(uint64_t hash, bool skinned);
    bool                    EnsureGBuffer();
    void                    ReleaseGBuffer();
    void                    ClearGBuffer(IDirect3DSurface9* sceneSoFar);
    bool          DrawLit(const DrawCallInfo& d, const VsInfo& vi);
    bool          InView(const MeshEntry& m, const MeshEntry::Placed& pl) const;
    std::unordered_map<uint64_t, bool> m_psTexkill;
    uint32_t      m_texkillShaders = 0;
    bool          PsUsesTexkill(uint64_t hash)
    {
        if (!hash) return false;
        auto it = m_psTexkill.find(hash);
        if (it != m_psTexkill.end()) return it->second;
        bool k = false;
        auto code = m_psCode.find(hash);
        rtpatch::ShaderUsage u;
        if (code != m_psCode.end() && rtpatch::Analyze(code->second.data(), &u)) k = u.usesTexkill;
        if (k && m_texkillShaders++ < 20) m_host->Log(LogLevel::Info, "RaytracingBridge: pixel shader %016llx cuts itself out (texkill): its surfaces let rays through where transparent", static_cast<unsigned long long>(hash));
        return m_psTexkill.emplace(hash, k).first->second;
    }
    uint64_t      m_lodDropped = 0;
    bool             m_ownCull = false;
    bool             m_cullRaster = true;
    int              m_retainOwn = 8;
    const MeshEntry* m_drawMesh = nullptr;
    float            m_drawWorld[4][4] = {};
    uint64_t         m_cullSkipped = 0, m_cullKept = 0;
    bool          BoxOnScreen(const MeshEntry& m, const float w[4][4], const float vp[4][4]) const;
    bool          IndependentMasks();
    int           m_masksCap = -1;
    uint64_t      m_statsBlendedGBuffer = 0;
    uint64_t      m_charExtraPasses = 0;
    std::unordered_set<uint64_t> m_charBodies;
    uint64_t      m_charBodiesFrame = ~0ull;
    uint64_t CharKey(const DrawCallInfo& d) const { return (reinterpret_cast<uint64_t>(d.indexBuffer) * 31u + d.startIndexOrVertex) * 131u + d.primitiveCount * 7u + static_cast<uint64_t>(d.baseVertexIndex); }
    bool IsSecondCharPass(const DrawCallInfo& d) {
        if (m_charBodiesFrame != d.frameIndex) { m_charBodies.clear(); m_charBodiesFrame = d.frameIndex; }
        const bool hasBase = d.pixelShaderHash && PsShadowFor(d.pixelShaderHash).base >= 0;
        if (hasBase) { m_charBodies.insert(CharKey(d)); return false; }
        return m_charBodies.count(CharKey(d)) != 0;
    }
public:
    const DWORD*  OnCreateShader(ShaderStage stage, uint64_t hash, const DWORD* code, size_t size) override;
private:
    void          ReadMatrix(const float* regs, float out[4][4]) const;
    void          CaptureInstance(const DrawCallInfo& d, const VsInfo& vi);
    MeshEntry*    GetOrCreateMesh(const DrawCallInfo& d, const MeshKey& k, const D3DVERTEXELEMENT9& pos, const D3DVERTEXELEMENT9* uv, const D3DVERTEXELEMENT9* color);
    void          CaptureSkinned(const DrawCallInfo& d, const VsInfo& vi);
    uint32_t      TextureIdFor(IDirect3DBaseTexture9* t);

    bool Composite(IDirect3DSurface9* target, IDirect3DSurface9* depth);
    void Watchdog();
    void Disable(const char* why);
    void EndFrame();

    IHost*            m_host = nullptr;
    std::atomic<bool> m_enabled{false};
    std::atomic<bool> m_disabling{false};
    HANDLE            m_watchdogThread = nullptr;
    HANDLE            m_watchdogStop = nullptr;
    static DWORD WINAPI WatchdogThread(void* self);

    bool  m_ao = true, m_shadows = true, m_sunFromShader = true, m_invertSun = true;
    int   m_minSceneDraws = 20, m_aoSamples = 4, m_debug = 0, m_maxMeshesPerFrame = 64;
    float m_aoRadius = 1.5f, m_shadowMax = 200.0f, m_normalBias = 0.05f;
    float m_fallbackSun[3] = {0.3f, 0.4f, 0.85f};
    Params m_params = {0.8f, 0.6f, 0.45f, 0.0f, 0, 0, 0, 0};
    uint64_t m_evictAfter = 900;
    UINT  m_minPrims = 2;

    std::wstring       m_mappingName;
    HANDLE             m_mapping = nullptr;
    br::Header*        m_header = nullptr;
    uint8_t*           m_view = nullptr;
    HANDLE             m_frameEvent = nullptr;
    HANDLE             m_slotFreeEvent = nullptr;
    HANDLE             m_job = nullptr;
    HANDLE             m_process = nullptr;
    ComPtr<ID3D12Fence> m_fenceGame, m_fenceBridge;
    HANDLE             m_fenceGameHandle = nullptr, m_fenceBridgeHandle = nullptr;
    uint64_t           m_gameValue = 0;
    uint64_t           m_outputFree[2] = {};
    std::atomic<uint64_t> m_frameId{0};
    std::atomic<uint64_t> m_lastProgressTick{0};
    uint64_t           m_lastCompleted = 0;
    bool               m_bridgeRunningLogged = false;

    uint32_t               m_generation = 0;
    UINT                   m_width = 0, m_height = 0;
    ComPtr<ID3D12Resource> m_sharedDepth[2], m_sharedOutput[2];
    ComPtr<ID3D12Resource> m_sharedNormal[2], m_sharedAlbedo[2], m_sharedColor[2], m_sharedOpaque[2];
    HANDLE                 m_sharedHandles[12] = {};
    DXGI_FORMAT            m_colorFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
    uint64_t               m_lastConsumed = 0;
    float                  m_sunIntensity = 1.0f, m_hitBaked = 0.5f, m_skyIntensity = 1.0f, m_giStrength = 1.0f, m_reflStrength = 1.0f;
    float                  m_denoisingRange = 1000.0f;
    float                  m_gameSunColor[3] = {}, m_gameSkyColor[3] = {};
    bool                   m_haveGameLight = false;
    ComPtr<ID3D12Resource> m_depthCopy;
    D3D12_RESOURCE_DESC    m_depthCopyDesc = {};

    ComPtr<ID3D12RootSignature>  m_rootSig;
    ComPtr<ID3D12PipelineState>  m_depthPso;
    std::unordered_map<UINT, ComPtr<ID3D12PipelineState>> m_compositePsos;
    ComPtr<ID3D12DescriptorHeap> m_srvHeap, m_rtvHeap;
    UINT                         m_srvInc = 0, m_rtvInc = 0;
    UINT                         m_ring = 0;

    std::unordered_map<uint64_t, VsInfo> m_vsInfo;
    std::unordered_map<uint64_t, int>    m_psSun;
    std::unordered_map<uint64_t, PsShadowInfo> m_psShadow;

    bool                      m_internal = true;
    ComPtr<IDirect3DTexture9> m_visTex;
    UINT                      m_visW = 0, m_visH = 0;
    bool                      m_visValid = false;
    float                     m_lightPos[3] = {};
    bool                      m_haveLightPos = false;
    ComPtr<ID3D12PipelineState> m_visPso;
    DXGI_FORMAT               m_visFormat = DXGI_FORMAT_UNKNOWN;
    uint64_t                  m_statsRtShadowDraws = 0;
    std::unordered_map<uint64_t, std::vector<DWORD>> m_vsCode, m_psCode;
    std::unordered_map<uint64_t, PatchedVSInfo> m_patchedVS;
    std::unordered_map<uint64_t, ComPtr<IDirect3DPixelShader9>>  m_patchedPS;
    uint32_t                  m_gameSamplers = 0;
    uint8_t                   m_gameConsts[256] = {};
    int                       m_patchConst = -1;
    uint64_t                  m_patchedOk = 0, m_patchedFail = 0;
    ComPtr<IDirect3DTexture9> m_gbufTex[3];
    ComPtr<IDirect3DSurface9> m_gbufSurf[3];
    UINT                      m_gbufW = 0, m_gbufH = 0;
    bool                      m_gbufFailed = false;
    uint64_t                  m_statsGBufferDraws = 0;
    float                     m_prevViewProj[4][4] = {};
    bool                      m_havePrevViewProj = false;
    bool                      m_debugGBuffer = false;
    float                     m_sunAngle = 0.01f;
    uint64_t                  m_skip[kSkipCount] = {};
    uint64_t                  m_captured = 0;
    int                       m_meshFail = kSkipCount;
    bool                      m_lightLogged = false;
    int  m_orientation = -1;
    bool m_orientationCalibrated = false;

    std::unordered_map<MeshKey, MeshEntry, MeshKeyHash> m_meshes;
    std::vector<uint32_t>                               m_pendingDestroy;
    uint32_t                                            m_nextMeshId = 1;
    UINT                                                m_meshesCreatedThisFrame = 0;

    UINT                 m_bbWidth = 0, m_bbHeight = 0;
    int                  m_sceneDraws = 0;
    bool                 m_composited = false;
    uint32_t             m_drawIndex = 0;
    uint32_t             m_runs = 0, m_runsAtLastScene = 0, m_prevRunsAtLastScene = 0;
    bool                 m_worldEnded = false;
    bool                 m_inRun = false;
    enum Fail { kFailCamera, kFailBridge, kFailGeneration, kFailResources, kFailSlot, kFailHandoff, kFailLate, kFailStretch, kFailCount };
    uint64_t             m_fail[kFailCount] = {};
    bool                 m_tracing = false;
    uint32_t             m_traceCount = 0, m_traceSceneRun = 0;
    int                  m_traceMax = 0;
    IDirect3DSurface9*   m_traceSurface = nullptr;
    void                 TraceFlush();
    void                 TracePixels(const char* when, IDirect3DSurface9* surface);
    uint64_t             m_copyComposites = 0;
    uint64_t             m_clearComposites = 0;
    uint64_t             m_lateScene = 0;
    void                 NoteFail(Fail f);
    struct SunCandidate { float dir[3]; float forward[3]; float swing; uint32_t stable; uint64_t seen; };
    struct SpriteClip { uint64_t key; float clip[4]; };
    std::unordered_map<uint64_t, int>          m_vsWvp;
    std::unordered_map<uint64_t, SunCandidate> m_sunCandidates;
    std::vector<SpriteClip>                    m_spriteClips;
    uint64_t             m_sunSprite = 0;
    void                 NoteSunSprite(const DrawCallInfo& d);
    void                 UpdateSunFromSprites();
    uint64_t             m_traceFrame = 0;
    IDirect3DSurface9*   m_sceneRT = nullptr;
    IDirect3DSurface9*   m_sceneDS = nullptr;
    float                m_viewProj[4][4] = {};
    float                m_camera[3] = {};
    float                m_sun[3] = {};
    bool                 m_haveCamera = false, m_haveSun = false;
    struct Pass {
        float               viewProj[4][4];
        float               camera[3];
        IDirect3DSurface9*  rt;
        IDirect3DSurface9*  ds;
        uint64_t            prims;
        uint32_t            draws;
    };
    std::vector<Pass>    m_passes;
    IDirect3DSurface9*   m_mainRT = nullptr;
    uint64_t             m_rejectedMatrices = 0;
    double               m_handPos = 0, m_handNeg = 0;
    uint64_t             m_mirroredDraws = 0;
    bool                 IsMirrored(const DrawCallInfo& d, const VsInfo& vi);
    uint64_t             m_mirrorFrame = 0;
    bool                 m_drawIsMirrorFloor = false;
    ComPtr<IDirect3DPixelShader9> m_mirrorPS;
    bool                 m_mirrorPSFailed = false;
    uint64_t             m_mirrorDraws = 0;
    uint64_t             m_mirrorFloors = 0;
    bool                 m_skipMirrored = true;
    void                 RecordPass(const DrawCallInfo& d, const VsInfo& vi);
    const Pass*          MainPass() const;
    bool                 m_sunFromSprite = false;
    struct LightRegs { int pos, col, att, dir, spot; bool pixel; };
    struct WorldLight { float pos[3], col[3], att[3], dir[3], cosOuter, cosInner; uint64_t lastSeen; };
    std::unordered_map<uint64_t, std::vector<LightRegs>> m_lightRegs;
    std::vector<WorldLight> m_worldLights;
    float                m_lightIntensity = 1.0f, m_lightMaxRadius = 40.0f;
    bool                 m_gameLights = true, m_blendedGBuffer = false, m_dropHiddenInView = false;
    uint64_t             m_litSkip[6] = {};
    float                m_bakedPools = 0.0f;
    bool                 m_fullRT = true;
    int                  m_gpuFailures = 0;
    void                 GpuListFailed();
    uint32_t             m_lightsSent = 0;
    uint64_t             m_lightDraws = 0;
    const std::vector<LightRegs>& LightRegsFor(ShaderStage stage, uint64_t hash);
    void                 NoteLights(const DrawCallInfo& d);
    void                 FillLights(br::FrameSlot& s);
    bool                 FillGameLamps(br::FrameSlot& s);
    void                 WriteGameClock(float hours);
    float                m_menuTime = -1.0f;
    void                 ReloadLive();
    float                m_emitLampMin = 0.5f, m_emitGlow = 2.0f;
    std::vector<uint32_t> m_lampKeys;
    bool                 m_ownLight = true, m_clockPaused = false;
    float                m_hours = 15.0f, m_dayMinutes = 24.0f, m_azimuth = 35.0f, m_tilt = 35.0f;
    float                m_cycleAmbient = 1.0f;
    float                m_cloudCoverage = 0.45f, m_cloudBottom = 900.0f, m_cloudTop = 1700.0f, m_moonIntensity = 1.0f, m_baseMinimumLight = 0.15f;
    int                  m_cloudSteps = 16;
    uint64_t             m_clockTick = 0;
    float                m_seconds = 0.0f;
    bool                 m_keyWasDown[6] = {};
    void                 UpdateClock();
    bool                 ReadGameClock(float& hours) const;
    void                 CaptureFrameBegin(uint64_t frameIndex);
    void                 CaptureFrameDraw(const DrawCallInfo& d, const VsInfo& vi);
    bool                 CaptureFrameDrawGuarded(const DrawCallInfo& d, const VsInfo& vi);
    void                 CaptureFrameAbort();
    std::wstring         m_capRequest;
    void                 CaptureFrameEnd(const FrameInfo& frame);
    std::string          CaptureTexture(IDirect3DBaseTexture9* base);
    FILE*                m_capObj = nullptr;
    FILE*                m_capTxt = nullptr;
    FILE*                m_capMtl = nullptr;
    std::wstring         m_capDir;
    std::unordered_map<IDirect3DBaseTexture9*, std::string> m_capTex;
    uint32_t             m_capDraw = 0, m_capWritten = 0, m_capMissing = 0, m_capVerts = 0, m_capNextTex = 1;
    bool                 m_capRunning = false, m_capKeyDown = false;
    struct TimeRow { float top[3], bot[3], core; };
    bool                 LoadTimeCycle();
    bool                 ReadGameWeather(int& season, int& from, int& to, float& blend) const;
    uint32_t             ReadGameLightState() const;
    void                 ApplyWeather(br::FrameSlot& s, float night);
    std::vector<TimeRow> m_timeCycle;
    int                  m_timeCycleState = 0;
    int                  m_weatherLogged[3] = {-1, -1, -1};
    int                  m_weatherOverride = -1, m_seasonOverride = -1;
    bool                 m_followWeather = true;
    bool                 m_followGameClock = true;
    int                  m_lastLoggedHour = -1;
    void                 FillOwnLight(br::FrameSlot& s);
    float                m_sunRaw[3] = {}, m_sunMin[3] = {9, 9, 9}, m_sunMax[3] = {-9, -9, -9};
    bool                 m_haveSunRaw = false;
    float                m_sunMaxRate = 0.5f;
    uint64_t             m_sunTick = 0, m_sunFarSince = 0;
    float                m_smoothSunColor[3] = {}, m_smoothSkyColor[3] = {};
    bool                 m_haveSmoothLight = false;
    float                m_sunMinZ = 0.05f;
    uint64_t             m_retainFrames = 1800;
    float                m_retainRadius = 800.0f;
    std::unordered_map<MeshKey, SkinMesh, MeshKeyHash> m_skin;
    std::vector<uint8_t>     m_dynData;
    std::vector<br::DynGeom> m_dynGeoms;
    struct TexKey {
        void* tex; UINT w, h, fmt;
        bool operator==(const TexKey& o) const { return tex == o.tex && w == o.w && h == o.h && fmt == o.fmt; }
    };
    struct TexKeyHash { size_t operator()(const TexKey& k) const { return reinterpret_cast<size_t>(k.tex) ^ (k.w * 31u) ^ (k.fmt * 131u); } };
    struct PendingTex { br::TexOp op; std::vector<uint8_t> data; };
    std::unordered_map<TexKey, uint32_t, TexKeyHash> m_texIds;
    std::vector<PendingTex>  m_pendingTex;
    uint32_t                 m_nextTexId = 1;
    UINT                     m_texCreatedThisFrame = 0;
    uint64_t                 m_dynDrawsLast = 0;
    uint64_t             m_statsFrames = 0, m_statsComposited = 0, m_statsSkipped = 0;
};

bool RaytracingBridge::OnInitialize(IHost* host)
{
    m_host = host;
    gameerrors::Install([host](const std::string& line) { host->Log(LogLevel::Warning, "RaytracingBridge: %s", line.c_str()); });
    {
        auto log = [host](const std::string& line) { host->Log(LogLevel::Info, "RaytracingBridge: %s", line.c_str()); };
        const float dd = host->GetConfigFloat("Culling", "DrawDistance", 2.0f);
        drawdistance::Apply(dd, log);
        int scale = host->GetConfigInt("Culling", "PoolScale", 0);
        if (scale <= 0) scale = static_cast<int>(std::ceil(2.0f * std::max(1.0f, dd) * std::max(1.0f, dd)));
        pools::Apply(scale, log);
    }
    if (host->GetConfigInt("RaytracingBridge", "Enabled", 1) == 0) return false;
    if (host->GetConfigInt("Light", "GameLamps", 1) != 0) {
        gamelights::Configure(host->GetConfigFloat("Light", "GameLampPower", 3.0f), host->GetConfigFloat("Light", "GameLampReach", 1.5f),
                              host->GetConfigFloat("Light", "CarLightPower", 1.5f));
        gamelights::Install([host](const std::string& line) { host->Log(LogLevel::Info, "RaytracingBridge: %s", line.c_str()); });
        const float onHour = host->GetConfigFloat("Light", "LampsOnHour", 18.0f);
        if (gamelights::Active() && onHour > 0.0f)
            gamelights::SetLampHours(onHour, host->GetConfigFloat("Light", "LampsOffHour", 7.0f),
                                     [host](const std::string& line) { host->Log(LogLevel::Info, "RaytracingBridge: %s", line.c_str()); });
        rtcycle::Load(std::wstring(host->GetGameDirectory()) + L"rtcycle.txt");
        menu::Init(host->GetGameDirectory(), menu::Hooks{
            [this]() { ReloadLive(); if (m_header) ++m_header->reserved0; },
            [this]() { m_timeCycleState = 0; m_timeCycle.clear(); if (m_header) ++m_header->reserved0; },
            [this]() {
                const gamelights::Stats gs = gamelights::LastStats();
                char b[256];
                snprintf(b, sizeof(b), "game lamps: %u lit, %u traced\nmeshes %u, instances %u\nRT GPU %.1f ms", gs.lamps, m_lightsSent,
                         m_header ? m_header->bridgeMeshCount : 0u, m_header ? m_header->bridgeLastInstanceCount : 0u, m_header ? m_header->bridgeGpuMs : 0.0f);
                return std::string(b);
            }});
        if (const char* t = getenv("BULLYRT_TEST_LAMPS"); t && gamelights::InjectForTest(t))
            host->Log(LogLevel::Info, "RaytracingBridge: game lights: %zu test lamps", gamelights::Lamps().size());
    }
    if (host->GetBackend() != Backend::D3D9On12 || !host->GetD3D12Device()) {
        host->Log(LogLevel::Warning, "RaytracingBridge: requires Backend=D3D9On12, disabled");
        return false;
    }

    m_ao               = host->GetConfigInt("RaytracingBridge", "AmbientOcclusion", 1) != 0;
    m_shadows          = host->GetConfigInt("RaytracingBridge", "SunShadows", 1) != 0;
    m_aoSamples        = std::clamp(host->GetConfigInt("RaytracingBridge", "AoSamples", 4), 1, 32);
    m_aoRadius         = host->GetConfigFloat("RaytracingBridge", "AoRadius", 1.5f);
    m_shadowMax        = host->GetConfigFloat("RaytracingBridge", "ShadowMaxDistance", 200.0f);
    m_normalBias       = host->GetConfigFloat("RaytracingBridge", "NormalBias", 0.05f);
    m_params.aoStrength     = host->GetConfigFloat("RaytracingBridge", "AoStrength", 0.8f);
    m_params.shadowStrength = host->GetConfigFloat("RaytracingBridge", "ShadowStrength", 0.6f);
    m_params.shadowAmbient  = host->GetConfigFloat("RaytracingBridge", "ShadowAmbient", 0.45f);
    m_debug            = host->GetConfigInt("RaytracingBridge", "DebugView", 0);
    m_params.debug     = static_cast<float>(m_debug);
    m_sunFromShader    = host->GetConfigInt("RaytracingBridge", "SunFromShader", 1) != 0;
    m_invertSun        = host->GetConfigInt("RaytracingBridge", "InvertShaderSunDirection", 1) != 0;
    m_minSceneDraws    = std::max(1, host->GetConfigInt("RaytracingBridge", "MinSceneDraws", 20));
    m_maxMeshesPerFrame = std::max(1, host->GetConfigInt("RaytracingBridge", "MaxNewMeshesPerFrame", 64));
    m_evictAfter       = static_cast<uint64_t>(std::max(60, host->GetConfigInt("RaytracingBridge", "EvictAfterFrames", 3600)));
    m_retainFrames     = static_cast<uint64_t>(std::max(0, host->GetConfigInt("RaytracingBridge", "RetainFrames", 1800)));
    m_retainRadius     = host->GetConfigFloat("RaytracingBridge", "RetainRadius", 800.0f);
    m_cullRaster       = host->GetConfigInt("Culling", "OwnFrustum", 1) != 0;
    m_retainOwn        = std::max(1, host->GetConfigInt("Culling", "RetainFrames", 8));
    m_sunMinZ          = host->GetConfigFloat("RaytracingBridge", "SunMinElevation", 0.05f);
    m_sunMaxRate       = host->GetConfigFloat("RaytracingBridge", "SunMaxDegreesPerSecond", 0.5f);
    m_sunFromSprite    = host->GetConfigInt("RaytracingBridge", "SunFromSprite", 0) != 0;
    m_traceMax         = host->GetConfigInt("RaytracingBridge", "FrameTraces", 0);
    m_ownLight         = host->GetConfigInt("Light", "OwnSystem", 1) != 0;
    m_lightIntensity   = host->GetConfigFloat("Light", "GameLightIntensity", 1.2f);
    m_followWeather    = host->GetConfigInt("Light", "FollowGameWeather", 1) != 0;
    m_weatherOverride  = host->GetConfigInt("Light", "WeatherOverride", -1);
    m_seasonOverride   = host->GetConfigInt("Light", "SeasonOverride", -1);
    m_gameLights       = host->GetConfigInt("Light", "GameLights", 1) != 0;
    m_bakedPools       = host->GetConfigFloat("Light", "BakedLampPools", 0.0f);
    m_fullRT           = host->GetConfigInt("Light", "FullRayTracing", 1) != 0;
    m_blendedGBuffer   = host->GetConfigInt("RaytracingBridge", "BlendedGBuffer", 1) != 0;
    m_dropHiddenInView = host->GetConfigInt("RaytracingBridge", "DropHiddenInView", 0) != 0;
    m_lightMaxRadius   = host->GetConfigFloat("Light", "GameLightMaxRadius", 40.0f);
    m_emitLampMin      = host->GetConfigFloat("Light", "EmitLampMin", 0.5f);
    m_emitGlow         = host->GetConfigFloat("Light", "EmitGlow", 2.0f);
    m_hours            = host->GetConfigFloat("Light", "TimeOfDay", 15.0f);
    m_menuTime         = m_hours;
    m_dayMinutes       = host->GetConfigFloat("Light", "DayLengthMinutes", 24.0f);
    m_clockPaused      = host->GetConfigInt("Light", "ClockPaused", 0) != 0;
    m_followGameClock  = host->GetConfigInt("Light", "FollowGameClock", 1) != 0;
    m_skipMirrored     = host->GetConfigInt("RaytracingBridge", "SkipGameReflections", 1) != 0;
    m_azimuth          = host->GetConfigFloat("Light", "SunAzimuthDegrees", 35.0f);
    m_tilt             = host->GetConfigFloat("Light", "SunTiltDegrees", 35.0f);
    m_moonIntensity    = host->GetConfigFloat("Light", "MoonIntensity", 1.0f);
    m_cloudCoverage    = host->GetConfigFloat("Light", "CloudCoverage", 0.45f);
    m_cloudSteps       = host->GetConfigInt("Light", "CloudSteps", 16);
    m_cloudBottom      = host->GetConfigFloat("Light", "CloudBottom", 900.0f);
    m_cloudTop         = host->GetConfigFloat("Light", "CloudTop", 1700.0f);
    m_minPrims         = static_cast<UINT>(std::max(1, host->GetConfigInt("RaytracingBridge", "MinPrimitives", 2)));
    m_orientation      = host->GetConfigInt("RaytracingBridge", "MatrixOrientation", -1);
    m_orientationCalibrated = m_orientation >= 0;
    char mode[32] = {};
    host->GetConfigString("RaytracingBridge", "Mode", "Internal", mode, sizeof(mode));
    m_internal = _stricmp(mode, "Overlay") != 0;
    m_sunAngle = std::tan(std::max(0.0f, host->GetConfigFloat("RaytracingBridge", "SunAngleDegrees", 0.6f)) * 0.0174533f);
    m_params.shadowStrength = host->GetConfigFloat("RaytracingBridge", "RtLightStrength", 1.0f);
    m_params.shadowAmbient  = host->GetConfigFloat("RaytracingBridge", "MinimumLight", 0.15f);
    m_debugGBuffer = host->GetConfigInt("RaytracingBridge", "DebugGBuffer", 0) != 0;
    m_internal       = true;
    m_sunIntensity   = host->GetConfigFloat("RaytracingBridge", "SunIntensity", 1.0f);
    m_hitBaked       = host->GetConfigFloat("RaytracingBridge", "HitBakedRadiance", 0.5f);
    m_skyIntensity   = host->GetConfigFloat("RaytracingBridge", "SkyIntensity", 1.0f);
    m_giStrength     = host->GetConfigFloat("RaytracingBridge", "GiStrength", 1.0f);
    m_reflStrength   = host->GetConfigFloat("RaytracingBridge", "ReflectionStrength", 1.0f);
    m_denoisingRange = host->GetConfigFloat("RaytracingBridge", "DenoisingRange", 1000.0f);
    m_traceFrame = static_cast<uint64_t>(std::max(0, host->GetConfigInt("RaytracingBridge", "TraceFrame", 0)));
    char sun[64] = {};
    host->GetConfigString("RaytracingBridge", "SunDirection", "0.3,0.4,0.85", sun, sizeof(sun));
    sscanf_s(sun, "%f,%f,%f", &m_fallbackSun[0], &m_fallbackSun[1], &m_fallbackSun[2]);

    if (!StartBridge()) return false;
    m_enabled      = true;
    if (host->GetConfigInt("Culling", "GameFrustum", 0) == 0)
        m_ownCull = gameculling::DisableFrustum([host](const std::string& line) { host->Log(LogLevel::Info, "RaytracingBridge: %s", line.c_str()); });
    if (!m_ownCull && getenv("BULLYRT_TEST_OWNCULL")) m_ownCull = true;
    if (m_ownCull) m_retainFrames = std::min<uint64_t>(m_retainFrames, static_cast<uint64_t>(m_retainOwn));
    m_watchdogStop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    m_watchdogThread = CreateThread(nullptr, 0, &RaytracingBridge::WatchdogThread, this, 0, nullptr);
    return true;
}

HANDLE RaytracingBridge::ShareObject(ID3D12DeviceChild* obj, const std::wstring& what)
{
    HANDLE h = nullptr;
    const HRESULT hr = m_host->GetD3D12Device()->CreateSharedHandle(obj, nullptr, GENERIC_ALL, Name(what).c_str(), &h);
    if (FAILED(hr)) {
        m_host->Log(LogLevel::Error, "RaytracingBridge: CreateSharedHandle(%ls) failed 0x%08lX", what.c_str(), hr);
        return nullptr;
    }
    return h;
}

bool RaytracingBridge::StartBridge()
{
    const std::wstring exe = std::wstring(m_host->GetGameDirectory()) + L"BullyDX12\\bridge\\BullyRTBridge.exe";
    if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) {
        m_host->Log(LogLevel::Warning, "RaytracingBridge: %ls not found, disabled", exe.c_str());
        return false;
    }
    const D3D12Caps* caps = m_host->GetD3D12Caps();

    m_mappingName = L"Local\\BullyRT_" + std::to_wstring(GetCurrentProcessId());
    const uint64_t bytes = br::MappingBytes();
    m_mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, static_cast<DWORD>(bytes >> 32),
                                   static_cast<DWORD>(bytes), m_mappingName.c_str());
    if (!m_mapping) { m_host->Log(LogLevel::Error, "RaytracingBridge: CreateFileMapping failed (%lu)", GetLastError()); return false; }
    m_view = static_cast<uint8_t*>(MapViewOfFile(m_mapping, FILE_MAP_ALL_ACCESS, 0, 0, 0));
    if (!m_view) { m_host->Log(LogLevel::Error, "RaytracingBridge: MapViewOfFile failed (%lu)", GetLastError()); return false; }
    m_header = reinterpret_cast<br::Header*>(m_view);
    memset(m_header, 0, sizeof(br::Header));
    m_header->magic            = br::kMagic;
    m_header->version          = br::kProtocolVersion;
    m_header->headerBytes      = sizeof(br::Header);
    m_header->gamePid          = GetCurrentProcessId();
    m_header->adapterLuidLow   = caps->adapterLuid.LowPart;
    m_header->adapterLuidHigh  = caps->adapterLuid.HighPart;
    m_header->mappingBytes     = bytes;
    const uint64_t dataStart   = (sizeof(br::Header) + 4095) & ~4095ull;
    m_header->slotDataOffset[0] = dataStart;
    m_header->slotDataOffset[1] = dataStart + br::kSlotDataBytes;
    m_header->slots[0].slotIndex = 0;
    m_header->slots[1].slotIndex = 1;

    m_frameEvent    = CreateEventW(nullptr, FALSE, FALSE, Name(L"frame").c_str());
    m_slotFreeEvent = CreateEventW(nullptr, FALSE, FALSE, Name(L"slotfree").c_str());
    ID3D12Device* dev = m_host->GetD3D12Device();
    if (FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&m_fenceGame))) ||
        FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&m_fenceBridge)))) {
        m_host->Log(LogLevel::Error, "RaytracingBridge: shared fence creation failed");
        return false;
    }
    m_fenceGameHandle   = ShareObject(m_fenceGame.Get(), L"fenceGame");
    m_fenceBridgeHandle = ShareObject(m_fenceBridge.Get(), L"fenceBridge");
    if (!m_frameEvent || !m_slotFreeEvent || !m_fenceGameHandle || !m_fenceBridgeHandle) return false;

    m_job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li = {};
    li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (m_job) SetInformationJobObject(m_job, JobObjectExtendedLimitInformation, &li, sizeof(li));

    std::wstring cmd = L"\"" + exe + L"\" " + m_mappingName;
    STARTUPINFOW si = {sizeof(si)};
    PROCESS_INFORMATION pi = {};
    const std::wstring dir = std::wstring(m_host->GetGameDirectory()) + L"BullyDX12\\bridge";
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr,
                        dir.c_str(), &si, &pi)) {
        m_host->Log(LogLevel::Error, "RaytracingBridge: cannot start %ls (%lu)", exe.c_str(), GetLastError());
        return false;
    }
    if (m_job) AssignProcessToJobObject(m_job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    m_process          = pi.hProcess;
    m_lastProgressTick = GetTickCount64();
    m_host->Log(LogLevel::Info, "RaytracingBridge: started BullyRTBridge.exe (pid %lu), shared memory %llu KB",
                pi.dwProcessId, bytes / 1024);
    return true;
}

DWORD WINAPI RaytracingBridge::WatchdogThread(void* self)
{
    auto* me = static_cast<RaytracingBridge*>(self);
    while (WaitForSingleObject(me->m_watchdogStop, 250) == WAIT_TIMEOUT && me->m_enabled) me->Watchdog();
    return 0;
}

void RaytracingBridge::Disable(const char* why)
{
    if (m_disabling.exchange(true)) return;
    m_enabled = false;
    gameculling::Restore();
    m_ownCull = false;
    m_host->Log(LogLevel::Error, "RaytracingBridge: disabled - %s", why);
    if (m_fenceBridge) m_fenceBridge->Signal(m_frameId + 1000000);
    if (m_header) m_header->shutdownRequested = 1;
    if (m_frameEvent) SetEvent(m_frameEvent);
}

void RaytracingBridge::Watchdog()
{
    if (!m_enabled || !m_header) return;
    if (m_header->bridgeState == 2) {
        char msg[300];
        snprintf(msg, sizeof(msg), "bridge failed: %s (see BullyDX12\\bridge\\BullyRTBridge.log)", m_header->bridgeMessage);
        Disable(msg);
        return;
    }
    if (m_process && WaitForSingleObject(m_process, 0) == WAIT_OBJECT_0) {
        Disable("bridge process exited unexpectedly");
        return;
    }
    const uint64_t now = GetTickCount64();
    if (m_header->completedFrame != m_lastCompleted || m_header->completedFrame >= m_frameId) {
        m_lastCompleted    = m_header->completedFrame;
        m_lastProgressTick = now;
    } else if (now - m_lastProgressTick > 4000) {
        if (m_process) TerminateProcess(m_process, 1);
        Disable("bridge stopped responding for 4 s");
    }
}

void RaytracingBridge::OnShutdown()
{
    menu::Shutdown();
    gameculling::Restore();
    gamelights::Restore();
    m_ownCull = false;
    if (m_watchdogStop) SetEvent(m_watchdogStop);
    if (m_watchdogThread) {
        WaitForSingleObject(m_watchdogThread, 2000);
        CloseHandle(m_watchdogThread);
        m_watchdogThread = nullptr;
    }
    if (m_header) {
        m_header->shutdownRequested = 1;
        SetEvent(m_frameEvent);
    }
}

void RaytracingBridge::OnDeviceLost()
{
    menu::DeviceLost();
    m_visTex.Reset();
    m_mainRT = nullptr;
    m_passes.clear();
    ReleaseGBuffer();
    m_visW = m_visH = 0;
    m_visValid = false;
    for (auto& kv : m_meshes) {
        kv.first.vb->Release();
        if (kv.first.ib) kv.first.ib->Release();
        if (kv.second.sent) m_pendingDestroy.push_back(kv.second.id);
    }
    m_meshes.clear();
    for (auto& kv : m_skin) {
        kv.first.vb->Release();
        if (kv.first.ib) kv.first.ib->Release();
    }
    m_skin.clear();
    m_dynData.clear();
    m_dynGeoms.clear();
    m_texIds.clear();
    m_sceneRT = m_sceneDS = nullptr;
    m_sceneDraws = 0;
    m_composited = false;
}

bool RaytracingBridge::CreateSharedResources(UINT w, UINT h)
{
    ID3D12Device* dev = m_host->GetD3D12Device();
    m_host->WaitForFence(m_host->ExecuteCommandList(m_host->BeginCommandList()));

    for (auto& hnd : m_sharedHandles) if (hnd) { CloseHandle(hnd); hnd = nullptr; }
    const uint32_t gen = m_generation + 1;

    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type                  = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d    = {};
    d.Dimension              = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width                  = w;
    d.Height                 = h;
    d.DepthOrArraySize       = 1;
    d.MipLevels              = 1;
    d.SampleDesc.Count       = 1;
    auto makeShared = [&](DXGI_FORMAT fmt, D3D12_RESOURCE_FLAGS flags, ComPtr<ID3D12Resource>& out, const wchar_t* fmtName, int slot, int handle) {
        d.Format = fmt;
        d.Flags  = flags;
        out.Reset();
        if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_SHARED, &d, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&out)))) {
            m_host->Log(LogLevel::Error, "RaytracingBridge: shared texture %ls (format %d) failed", fmtName, fmt);
            return false;
        }
        wchar_t name[64];
        swprintf_s(name, L"g%u_%s%d", gen, fmtName, slot);
        m_sharedHandles[handle] = ShareObject(out.Get(), name);
        return m_sharedHandles[handle] != nullptr;
    };
    for (int i = 0; i < 2; ++i) {
        const int b = i * 6;
        if (!makeShared(DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, m_sharedDepth[i], L"depth", i, b) ||
            !makeShared(DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, m_sharedOutput[i], L"output", i, b + 1) ||
            !makeShared(DXGI_FORMAT_B8G8R8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, m_sharedNormal[i], L"normal", i, b + 2) ||
            !makeShared(DXGI_FORMAT_B8G8R8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, m_sharedAlbedo[i], L"albedo", i, b + 3) ||
            !makeShared(m_colorFormat, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, m_sharedColor[i], L"color", i, b + 4) ||
            !makeShared(DXGI_FORMAT_B8G8R8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, m_sharedOpaque[i], L"opaque", i, b + 5))
            return false;
    }
    m_width = w;
    m_height = h;
    m_generation = gen;
    m_header->resourceWidth  = w;
    m_header->resourceHeight = h;
    m_outputFree[0] = m_outputFree[1] = m_gameValue;
    m_lastConsumed = m_frameId;
    m_visValid = false;
    MemoryBarrier();
    m_header->resourceGeneration = gen;
    SetEvent(m_frameEvent);
    m_host->Log(LogLevel::Info, "RaytracingBridge: shared textures %ux%u (generation %u)", w, h, gen);
    return true;
}

bool RaytracingBridge::EnsureDepthCopy(const D3D12_RESOURCE_DESC& ds)
{
    if (m_depthCopy && m_depthCopyDesc.Width == ds.Width && m_depthCopyDesc.Height == ds.Height &&
        m_depthCopyDesc.Format == ds.Format)
        return true;
    DXGI_FORMAT typeless;
    switch (ds.Format) {
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_R24G8_TYPELESS:  typeless = DXGI_FORMAT_R24G8_TYPELESS; break;
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_R32_TYPELESS:    typeless = DXGI_FORMAT_R32_TYPELESS; break;
    case DXGI_FORMAT_D16_UNORM:
    case DXGI_FORMAT_R16_TYPELESS:    typeless = DXGI_FORMAT_R16_TYPELESS; break;
    default:
        m_host->Log(LogLevel::Error, "RaytracingBridge: unsupported depth format %d", ds.Format);
        return false;
    }
    if (m_depthCopy) m_host->DeferredRelease(m_depthCopy.Detach());
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type                  = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d    = ds;
    d.Format                 = typeless;
    d.Flags                  = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    d.Alignment              = 0;
    if (FAILED(m_host->GetD3D12Device()->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COMMON,
                                                                 nullptr, IID_PPV_ARGS(&m_depthCopy))))
        return false;
    m_depthCopyDesc = ds;
    return true;
}

bool RaytracingBridge::EnsurePipelines(DXGI_FORMAT target)
{
    ID3D12Device* dev = m_host->GetD3D12Device();
    if (!m_rootSig) {
        D3D12_DESCRIPTOR_RANGE r = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0};
        D3D12_ROOT_PARAMETER p[2] = {};
        p[0].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        p[0].Constants.Num32BitValues            = sizeof(Params) / 4;
        p[0].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;
        p[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        p[1].DescriptorTable.NumDescriptorRanges = 1;
        p[1].DescriptorTable.pDescriptorRanges   = &r;
        p[1].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC rs = {2, p, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
        ComPtr<ID3DBlob> blob, err;
        if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err)) ||
            FAILED(dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&m_rootSig))))
            return false;
        D3D12_DESCRIPTOR_HEAP_DESC hd = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 256, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
        if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_srvHeap)))) return false;
        hd = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 256, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
        if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_rtvHeap)))) return false;
        m_srvInc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        m_rtvInc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    }

    auto makePso = [&](const void* ps, size_t psSize, DXGI_FORMAT fmt, bool multiply, ComPtr<ID3D12PipelineState>& out) {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC d = {};
        d.pRootSignature                     = m_rootSig.Get();
        d.VS                                 = {g_RTB_VSMain, sizeof(g_RTB_VSMain)};
        d.PS                                 = {ps, psSize};
        D3D12_RENDER_TARGET_BLEND_DESC& b    = d.BlendState.RenderTarget[0];
        b.RenderTargetWriteMask              = multiply ? (D3D12_COLOR_WRITE_ENABLE_RED | D3D12_COLOR_WRITE_ENABLE_GREEN |
                                                           D3D12_COLOR_WRITE_ENABLE_BLUE)
                                                        : D3D12_COLOR_WRITE_ENABLE_ALL;
        if (multiply) {
            b.BlendEnable    = TRUE;
            b.SrcBlend       = D3D12_BLEND_ZERO;
            b.DestBlend      = D3D12_BLEND_SRC_COLOR;
            b.BlendOp        = D3D12_BLEND_OP_ADD;
            b.SrcBlendAlpha  = D3D12_BLEND_ZERO;
            b.DestBlendAlpha = D3D12_BLEND_ONE;
            b.BlendOpAlpha   = D3D12_BLEND_OP_ADD;
        }
        d.SampleMask                      = UINT_MAX;
        d.RasterizerState.FillMode        = D3D12_FILL_MODE_SOLID;
        d.RasterizerState.CullMode        = D3D12_CULL_MODE_NONE;
        d.RasterizerState.DepthClipEnable = TRUE;
        d.PrimitiveTopologyType           = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        d.NumRenderTargets                = 1;
        d.RTVFormats[0]                   = fmt;
        d.SampleDesc.Count                = 1;
        return SUCCEEDED(dev->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&out)));
    };

    if (!m_depthPso && !makePso(g_RTB_PSDepth, sizeof(g_RTB_PSDepth), DXGI_FORMAT_R32_FLOAT, false, m_depthPso)) {
        m_host->Log(LogLevel::Error, "RaytracingBridge: depth PSO failed");
        return false;
    }
    (void)target;
    return true;
}

const VsInfo& RaytracingBridge::VsInfoFor(uint64_t hash)
{
    auto it = m_vsInfo.find(hash);
    if (it != m_vsInfo.end()) return it->second;
    VsInfo v;
    UINT reg = 0, cnt = 0, set = 0;
    if (m_host->FindShaderConstant(ShaderStage::Vertex, hash, "mWorld", &reg, &cnt, &set) && cnt >= 3 && set == 2)
        v.world = static_cast<int>(reg);
    if (m_host->FindShaderConstant(ShaderStage::Vertex, hash, "mViewProjection", &reg, &cnt, &set) && cnt >= 4 && set == 2)
        v.viewProj = static_cast<int>(reg);
    if (m_host->FindShaderConstant(ShaderStage::Vertex, hash, "vPosition_Camera", &reg, &cnt, &set) && set == 2)
        v.camera = static_cast<int>(reg);
    if (m_host->FindShaderConstant(ShaderStage::Vertex, hash, "mBone", &reg, &cnt, &set) && set == 2) {
        v.skinned   = true;
        v.bone      = static_cast<int>(reg);
        v.boneCount = static_cast<int>(cnt);
    }
    auto code = m_vsCode.find(hash);
    if (code != m_vsCode.end()) {
        rtpatch::ShaderUsage u;
        if (rtpatch::Analyze(code->second.data(), &u)) {
            v.outPos1 = (u.usedTexcoordUsage & (1u << 1)) != 0;
            v.outNrm4 = (u.usedTexcoordUsage & (1u << 4)) != 0;
        }
    }
    return m_vsInfo.emplace(hash, v).first->second;
}

int RaytracingBridge::SunRegisterFor(uint64_t psHash)
{
    auto it = m_psSun.find(psHash);
    if (it != m_psSun.end()) return it->second;
    UINT reg = 0, cnt = 0, set = 0;
    const int r = m_host->FindShaderConstant(ShaderStage::Pixel, psHash, "vDirection_ShadowSpotLight0", &reg, &cnt, &set) &&
                          set == 2
                      ? static_cast<int>(reg)
                      : -1;
    m_psSun.emplace(psHash, r);
    return r;
}

const PsShadowInfo& RaytracingBridge::PsShadowFor(uint64_t psHash)
{
    auto it = m_psShadow.find(psHash);
    if (it != m_psShadow.end()) return it->second;
    PsShadowInfo si;
    UINT reg = 0, cnt = 0, set = 0;
    if (m_host->FindShaderConstant(ShaderStage::Pixel, psHash, "tShadowMap_ShadowSpotLight0", &reg, &cnt, &set) && set == 3)
        si.sampler = static_cast<int>(reg);
    if (m_host->FindShaderConstant(ShaderStage::Pixel, psHash, "mWorldToShadowMap_ShadowSpotLight0", &reg, &cnt, &set) &&
        set == 2 && cnt >= 4)
        si.matrix = static_cast<int>(reg);
    if (m_host->FindShaderConstant(ShaderStage::Pixel, psHash, "fShadowBias_ShadowSpotLight0", &reg, &cnt, &set) && set == 2)
        si.bias = static_cast<int>(reg);
    if (m_host->FindShaderConstant(ShaderStage::Pixel, psHash, "vPosition_ShadowSpotLight0", &reg, &cnt, &set) && set == 2)
        si.lightPos = static_cast<int>(reg);
    if (m_host->FindShaderConstant(ShaderStage::Pixel, psHash, "tBase", &reg, &cnt, &set) && set == 3)
        si.base = static_cast<int>(reg);
    if (m_host->FindShaderConstant(ShaderStage::Pixel, psHash, "cDiffuse_ShadowSpotLight0", &reg, &cnt, &set) && set == 2)
        si.sunColor = static_cast<int>(reg);
    if (m_host->FindShaderConstant(ShaderStage::Pixel, psHash, "cAmbientLight", &reg, &cnt, &set) && set == 2)
        si.ambient = static_cast<int>(reg);
    return m_psShadow.emplace(psHash, si).first->second;
}

bool RaytracingBridge::EnsureVisTexture(UINT w, UINT h)
{
    if (m_visTex && m_visW == w && m_visH == h) return true;
    m_visTex.Reset();
    m_visValid = false;
    if (FAILED(m_host->GetDevice9()->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT,
                                                   &m_visTex, nullptr))) {
        m_host->Log(LogLevel::Error, "RaytracingBridge: cannot create %ux%u final image texture", w, h);
        return false;
    }
    m_visW = w;
    m_visH = h;
    return true;
}

const DWORD* RaytracingBridge::OnCreateShader(ShaderStage stage, uint64_t hash, const DWORD* code, size_t size)
{
    if (!m_internal || !code || size < 8) return nullptr;
    auto& store = stage == ShaderStage::Vertex ? m_vsCode : m_psCode;
    if (store.count(hash)) return nullptr;
    store.emplace(hash, std::vector<DWORD>(code, code + size / 4));
    if (stage == ShaderStage::Pixel) {
        rtpatch::ShaderUsage u;
        if (rtpatch::Analyze(code, &u)) {
            m_gameSamplers |= u.usedSamplers;
            for (size_t c = 0; c < 256; ++c) if (u.usedFloatConsts[c]) m_gameConsts[c] = 1;
        }
    }
    return nullptr;
}

bool RaytracingBridge::ChooseResources()
{
    if (m_patchConst >= 0) return true;
    const int n = static_cast<int>(rtpatch::kConstCount);
    for (int c = 224 - n; c >= 0 && m_patchConst < 0; --c) {
        bool free = true;
        for (int k = 0; k < n; ++k) free = free && !m_gameConsts[c + k];
        if (free) m_patchConst = c;
    }
    if (m_patchConst < 0) {
        m_host->Log(LogLevel::Error, "RaytracingBridge: no free constants for the G-buffer");
        return false;
    }
    m_host->Log(LogLevel::Info, "RaytracingBridge: G-buffer shaders use c%d..c%d (unused by the game)", m_patchConst, m_patchConst + n - 1);
    return true;
}

const RaytracingBridge::PatchedVSInfo& RaytracingBridge::PatchedVS(uint64_t hash, int worldReg)
{
    auto it = m_patchedVS.find(hash);
    if (it != m_patchedVS.end()) return it->second;
    PatchedVSInfo info;
    auto code = m_vsCode.find(hash);
    if (code != m_vsCode.end()) {
        const std::vector<DWORD> p =
            rtpatch::PatchVertexShader(code->second.data(), static_cast<uint32_t>(worldReg), &info.hasNormal);
        if (!p.empty() && FAILED(m_host->GetDevice9()->CreateVertexShader(p.data(), &info.shader))) info.shader.Reset();
    }
    ++(info.shader ? m_patchedOk : m_patchedFail);
    return m_patchedVS.emplace(hash, info).first->second;
}

IDirect3DPixelShader9* RaytracingBridge::PatchedPS(uint64_t hash, bool skinned)
{
    const uint64_t key = hash ^ (skinned ? 0xC2B2AE3D27D4EB4Full : 0);
    auto it = m_patchedPS.find(key);
    if (it != m_patchedPS.end()) return it->second.Get();
    ComPtr<IDirect3DPixelShader9> sh;
    auto code = m_psCode.find(hash);
    if (code != m_psCode.end()) {
        rtpatch::PsPatchOptions o;
        o.constBase = static_cast<uint32_t>(m_patchConst);
        if (skinned) o.nrmUsage = 4;
        UINT reg = 0, cnt = 0, set = 0;
        if (m_host->FindShaderConstant(ShaderStage::Pixel, hash, "tBase", &reg, &cnt, &set) && set == 3)
            o.albedoSampler = static_cast<int>(reg);
        const std::vector<DWORD> p = rtpatch::PatchPixelShader(code->second.data(), o);
        if (!p.empty() && FAILED(m_host->GetDevice9()->CreatePixelShader(p.data(), &sh))) sh.Reset();
    }
    ++(sh ? m_patchedOk : m_patchedFail);
    return m_patchedPS.emplace(key, sh).first->second.Get();
}

bool RaytracingBridge::EnsureGBuffer()
{
    if (m_gbufTex[0] && m_gbufW == m_bbWidth && m_gbufH == m_bbHeight) return true;
    ReleaseGBuffer();
    if (!m_bbWidth || !m_bbHeight) return false;
    const D3DFORMAT fmt[3] = {D3DFMT_A8R8G8B8, D3DFMT_A8R8G8B8, D3DFMT_A8R8G8B8};
    IDirect3DDevice9* dev = m_host->GetDevice9();
    for (int i = 0; i < 3; ++i) {
        if (FAILED(dev->CreateTexture(m_bbWidth, m_bbHeight, 1, D3DUSAGE_RENDERTARGET, fmt[i], D3DPOOL_DEFAULT,
                                      &m_gbufTex[i], nullptr)) ||
            FAILED(m_gbufTex[i]->GetSurfaceLevel(0, &m_gbufSurf[i]))) {
            m_host->Log(LogLevel::Error, "RaytracingBridge: G-buffer target %d (%ux%u) creation failed", i, m_bbWidth, m_bbHeight);
            ReleaseGBuffer();
            m_gbufFailed = true;
            return false;
        }
    }
    m_gbufW = m_bbWidth;
    m_gbufH = m_bbHeight;
    m_host->Log(LogLevel::Info, "RaytracingBridge: G-buffer %ux%u (normal+roughness, albedo, opaque colour) written by the game's own shaders",
                m_gbufW, m_gbufH);
    return true;
}

void RaytracingBridge::ReleaseGBuffer()
{
    for (int i = 0; i < 3; ++i) { m_gbufSurf[i].Reset(); m_gbufTex[i].Reset(); }
    m_gbufW = m_gbufH = 0;
}

void RaytracingBridge::ClearGBuffer(IDirect3DSurface9* sceneSoFar)
{
    if (!m_gbufSurf[0]) return;
    IDirect3DDevice9* dev = m_host->GetDevice9();
    for (int i = 0; i < 3; ++i) dev->ColorFill(m_gbufSurf[i].Get(), nullptr, 0x00000000);
    if (sceneSoFar) dev->StretchRect(sceneSoFar, nullptr, m_gbufSurf[2].Get(), nullptr, D3DTEXF_POINT);
}

bool RaytracingBridge::DrawLit(const DrawCallInfo& d, const VsInfo& vi)
{
    if (d.kind != DrawKind::IndexedPrimitive && d.kind != DrawKind::Primitive) return false;
    if (!ChooseResources()) return false;
    IDirect3DVertexShader9* patchedVs = nullptr;
    bool hasNormal = vi.skinned && vi.outNrm4;
    if (vi.skinned) {
        if (!vi.outPos1) return false;
    } else {
        const PatchedVSInfo& pvs = PatchedVS(d.vertexShaderHash, vi.world);
        if (!pvs.shader) return false;
        patchedVs = pvs.shader.Get();
        hasNormal = pvs.hasNormal;
    }

    if (!hasNormal) ++m_litSkip[0];
    if (!d.renderStates || !hasNormal || m_gbufFailed || !EnsureGBuffer()) return false;
    if (!vi.skinned && patchedVs && m_header && !m_header->rtDisabled) {
        bool mirror = false;
        for (int s = 0; s < 8 && !mirror; ++s) {
            const uint32_t tex = d.textures[s] ? TextureIdFor(d.textures[s]) : 0;
            mirror = tex && tex < br::kMaxTexMaterials && m_header->texEmissive[tex] == 252;
        }
        if (mirror) {
            IDirect3DDevice9* dev = m_host->GetDevice9();
            if (!m_mirrorPS && !m_mirrorPSFailed) {
                const std::vector<DWORD> code = rtpatch::MirrorPixelShader(static_cast<uint32_t>(m_patchConst));
                if (FAILED(dev->CreatePixelShader(code.data(), &m_mirrorPS))) { m_mirrorPS.Reset(); m_mirrorPSFailed = true; }
                m_host->Log(LogLevel::Info, "RaytracingBridge: mirror shader %s", m_mirrorPS ? "ready: mirrors reflect the traced world" : "FAILED");
            }
            if (m_mirrorPS) {
                const float mc[rtpatch::kConstCount][4] = {{0.5f, 0.0f, 0.0f, 0.0f}, {0.0f, 0.05f + 252.0f / 255.0f * 0.35f, 0.0f, 1.0f}};
                dev->SetVertexShader(patchedVs);
                dev->SetPixelShader(m_mirrorPS.Get());
                dev->SetPixelShaderConstantF(static_cast<UINT>(m_patchConst), &mc[0][0], rtpatch::kConstCount);
                for (DWORD i = 0; i < 3; ++i) dev->SetRenderTarget(i + 1, m_gbufSurf[i].Get());
                const HRESULT hr = d.kind == DrawKind::IndexedPrimitive
                    ? dev->DrawIndexedPrimitive(d.primitiveType, d.baseVertexIndex, d.minVertexIndex, d.numVertices, d.startIndexOrVertex, d.primitiveCount)
                    : dev->DrawPrimitive(d.primitiveType, d.startIndexOrVertex, d.primitiveCount);
                for (DWORD i = 1; i <= 3; ++i) dev->SetRenderTarget(i, nullptr);
                dev->SetVertexShader(d.vertexShader);
                dev->SetPixelShader(d.pixelShader);
                dev->SetPixelShaderConstantF(static_cast<UINT>(m_patchConst), d.psConstantsF + m_patchConst * 4, rtpatch::kConstCount);
                if (m_mirrorDraws++ == 0) m_host->Log(LogLevel::Info, "RaytracingBridge: first mirror drawn into the G-buffer (%s)", SUCCEEDED(hr) ? "ok" : "draw failed");
                return SUCCEEDED(hr);
            }
        }
    }
    const bool zwrite  = d.renderStates[D3DRS_ZWRITEENABLE] != 0;
    const bool blended = d.renderStates[D3DRS_ALPHABLENDENABLE] != 0;
    if (blended && !m_blendedGBuffer) return false;
    const bool additive = blended && d.renderStates[D3DRS_DESTBLEND] == D3DBLEND_ONE && !vi.skinned;
    if (blended && !additive && d.renderStates[D3DRS_DESTBLEND] != D3DBLEND_INVSRCALPHA) { ++m_litSkip[1]; return false; }
    if (additive && (!m_header || m_header->rtDisabled)) { ++m_litSkip[1]; return false; }
    if (blended && !IndependentMasks()) { ++m_litSkip[2]; return false; }
    if (!blended && !zwrite) { ++m_litSkip[3]; return false; }
    if (vi.skinned && IsSecondCharPass(d)) {
        if (m_charExtraPasses++ == 0)
            m_host->Log(LogLevel::Info, "RaytracingBridge: character pass without a base texture (pixel shader %016llx) kept out of the material and the rays",
                        static_cast<unsigned long long>(d.pixelShaderHash));
        return false;
    }
    IDirect3DPixelShader9* ps = PatchedPS(d.pixelShaderHash, vi.skinned);
    if (!ps) { ++m_litSkip[4]; return false; }
    IDirect3DDevice9* dev = m_host->GetDevice9();

    float roughness = 0.8f;
    UINT reg = 0, cnt = 0, set = 0;
    if (m_host->FindShaderConstant(ShaderStage::Pixel, d.pixelShaderHash, "fSpecularPower_Material", &reg, &cnt, &set) && set == 2) {
        const float power = std::max(d.psConstantsF[reg * 4], 0.0f);
        roughness = std::clamp(std::sqrt(2.0f / (power + 2.0f)), 0.04f, 1.0f);
    }
    {
        UINT rReg = 0, rCnt = 0, rSet = 0;
        if (m_drawIsMirrorFloor) roughness = std::min(roughness, 0.3f);
        else if (m_host->FindShaderConstant(ShaderStage::Pixel, d.pixelShaderHash, "tReflection", &rReg, &rCnt, &rSet) && rSet == 3) roughness = std::min(roughness, 0.45f);
    }
    float c[rtpatch::kConstCount][4] = {};
    c[0][0] = 0.5f;
    c[1][0] = roughness;
    c[1][1] = d.renderStates[D3DRS_ALPHATESTENABLE] || blended ? 0.5f : 1.0f;
    {
        const int matSampler = d.pixelShaderHash ? PsShadowFor(d.pixelShaderHash).base : -1;
        const uint32_t matTex = matSampler >= 0 && matSampler < 8 ? TextureIdFor(d.textures[matSampler]) : 0;
        const uint8_t emit = (m_header && matTex && matTex < br::kMaxTexMaterials) ? m_header->texEmissive[matTex] : 0;
        if (emit) c[1][1] = 0.05f + emit / 255.0f * 0.35f;
    }
    c[1][2] = blended ? 1.0f : 0.0f;
    c[1][3] = (d.renderStates[D3DRS_ALPHATESTENABLE] || PsUsesTexkill(d.pixelShaderHash)) ? 0.0f : 1.0f;
    static const bool testAllCharacters = getenv("BULLYRT_TEST_ALLCHAR") != nullptr;
    if ((vi.skinned || testAllCharacters) && !blended) c[1][1] = 0.8f;
    if (m_drawIsMirrorFloor) { c[1][2] = 0.0f; c[1][3] = 1.0f; c[1][1] = 0.95f; }

    auto issue = [&]() {
        return d.kind == DrawKind::IndexedPrimitive
                   ? dev->DrawIndexedPrimitive(d.primitiveType, d.baseVertexIndex, d.minVertexIndex, d.numVertices, d.startIndexOrVertex, d.primitiveCount)
                   : dev->DrawPrimitive(d.primitiveType, d.startIndexOrVertex, d.primitiveCount);
    };
    auto markPass = [&](float mark, bool cutOut) {
        dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0);
        dev->SetRenderState(D3DRS_COLORWRITEENABLE1, 0);
        dev->SetRenderState(D3DRS_COLORWRITEENABLE2, D3DCOLORWRITEENABLE_ALPHA);
        dev->SetRenderState(D3DRS_COLORWRITEENABLE3, 0);
        dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        if (d.renderStates[D3DRS_ZFUNC] == D3DCMP_LESS) dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
        dev->SetRenderState(D3DRS_ALPHATESTENABLE, cutOut ? TRUE : FALSE);
        dev->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATER);
        dev->SetRenderState(D3DRS_ALPHAREF, 8);
        float mc[rtpatch::kConstCount][4];
        memcpy(mc, c, sizeof(mc));
        mc[1][1] = mark;
        if (patchedVs) dev->SetVertexShader(patchedVs);
        dev->SetPixelShader(ps);
        dev->SetPixelShaderConstantF(static_cast<UINT>(m_patchConst), &mc[0][0], rtpatch::kConstCount);
        for (DWORD i = 0; i < 3; ++i) dev->SetRenderTarget(i + 1, m_gbufSurf[i].Get());
        issue();
        for (DWORD i = 1; i <= 3; ++i) dev->SetRenderTarget(i, nullptr);
        if (patchedVs) dev->SetVertexShader(d.vertexShader);
        dev->SetPixelShader(d.pixelShader);
        dev->SetPixelShaderConstantF(static_cast<UINT>(m_patchConst), d.psConstantsF + m_patchConst * 4, rtpatch::kConstCount);
        for (D3DRENDERSTATETYPE s : {D3DRS_ALPHABLENDENABLE, D3DRS_COLORWRITEENABLE, D3DRS_COLORWRITEENABLE1, D3DRS_COLORWRITEENABLE2, D3DRS_COLORWRITEENABLE3, D3DRS_ZWRITEENABLE, D3DRS_ZFUNC, D3DRS_ALPHATESTENABLE, D3DRS_ALPHAFUNC, D3DRS_ALPHAREF})
            dev->SetRenderState(s, d.renderStates[s]);
    };
    if (additive) {
        const HRESULT drawn = issue();
        if (FAILED(drawn)) return true;
        markPass(0.905f, false);
        return true;
    }
    if (blended) {
        const bool depthLater = zwrite && !m_drawIsMirrorFloor && m_header && !m_header->rtDisabled;
        if (depthLater) dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        const HRESULT first = issue();
        if (depthLater) dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
        if (FAILED(first)) return true;
        if (!m_drawIsMirrorFloor && !vi.skinned && m_header && !m_header->rtDisabled && d.pixelShaderHash) {
            const int decalSampler = PsShadowFor(d.pixelShaderHash).base;
            if (decalSampler >= 0 && decalSampler < 8 && d.textures[decalSampler]) markPass(0.915f, true);
        }
        dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0);
        dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
        if (d.renderStates[D3DRS_ZFUNC] == D3DCMP_LESS) dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
        dev->SetRenderState(D3DRS_ALPHATESTENABLE, m_drawIsMirrorFloor ? FALSE : TRUE);
        dev->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATEREQUAL);
        dev->SetRenderState(D3DRS_ALPHAREF, zwrite ? 190 : 230);
    }

    if (patchedVs) dev->SetVertexShader(patchedVs);
    dev->SetPixelShader(ps);
    dev->SetPixelShaderConstantF(static_cast<UINT>(m_patchConst), &c[0][0], rtpatch::kConstCount);
    for (DWORD i = 0; i < 3; ++i) dev->SetRenderTarget(i + 1, m_gbufSurf[i].Get());

    const HRESULT hr = issue();

    for (DWORD i = 1; i <= 3; ++i) dev->SetRenderTarget(i, nullptr);
    if (patchedVs) dev->SetVertexShader(d.vertexShader);
    dev->SetPixelShader(d.pixelShader);
    dev->SetPixelShaderConstantF(static_cast<UINT>(m_patchConst), d.psConstantsF + m_patchConst * 4, rtpatch::kConstCount);
    if (blended) {
        for (D3DRENDERSTATETYPE s : {D3DRS_ALPHABLENDENABLE, D3DRS_COLORWRITEENABLE, D3DRS_ZWRITEENABLE, D3DRS_ZFUNC, D3DRS_ALPHATESTENABLE, D3DRS_ALPHAFUNC, D3DRS_ALPHAREF})
            dev->SetRenderState(s, d.renderStates[s]);
        if (SUCCEEDED(hr)) ++m_statsBlendedGBuffer;
        return true;
    }
    if (SUCCEEDED(hr)) {
        ++m_statsGBufferDraws;
    }
    return SUCCEEDED(hr);
}

bool CameraFromViewProj(const float m[4][4], float c[3])
{
    const int r[3] = {0, 1, 3};
    double a[3][4];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 4; ++j) a[i][j] = j < 3 ? m[r[i]][j] : -m[r[i]][3];
    for (int col = 0; col < 3; ++col) {
        int piv = col;
        for (int i = col + 1; i < 3; ++i) if (std::fabs(a[i][col]) > std::fabs(a[piv][col])) piv = i;
        if (std::fabs(a[piv][col]) < 1e-12) return false;
        for (int j = 0; j < 4; ++j) std::swap(a[piv][j], a[col][j]);
        for (int i = 0; i < 3; ++i) {
            if (i == col) continue;
            const double f = a[i][col] / a[col][col];
            for (int j = 0; j < 4; ++j) a[i][j] -= f * a[col][j];
        }
    }
    for (int i = 0; i < 3; ++i) c[i] = static_cast<float>(a[i][3] / a[i][i]);
    return true;
}

void RaytracingBridge::ReadMatrix(const float* regs, float out[4][4]) const
{
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) out[i][j] = m_orientation == 1 ? regs[j * 4 + i] : regs[i * 4 + j];
}

namespace {
void DecodeElement(const uint8_t* p, BYTE type, float out[4])
{
    out[0] = out[1] = out[2] = 0; out[3] = 1;
    switch (type) {
    case D3DDECLTYPE_FLOAT1: memcpy(out, p, 4); break;
    case D3DDECLTYPE_FLOAT2: memcpy(out, p, 8); break;
    case D3DDECLTYPE_FLOAT3: memcpy(out, p, 12); break;
    case D3DDECLTYPE_FLOAT4: memcpy(out, p, 16); break;
    case D3DDECLTYPE_D3DCOLOR:
        out[0] = p[2] / 255.0f; out[1] = p[1] / 255.0f; out[2] = p[0] / 255.0f; out[3] = p[3] / 255.0f;
        break;
    case D3DDECLTYPE_UBYTE4N:
        for (int i = 0; i < 4; ++i) out[i] = p[i] / 255.0f;
        break;
    case D3DDECLTYPE_UBYTE4:
        for (int i = 0; i < 4; ++i) out[i] = static_cast<float>(p[i]);
        break;
    default: break;
    }
}

std::vector<uint32_t> ReadTriangles(const MeshKey& k, int* failReason, bool keepWinding = false)
{
    std::vector<uint32_t> tris;
    D3DINDEXBUFFER_DESC id = {};
    const bool strip      = k.primType == D3DPT_TRIANGLESTRIP;
    const UINT srcIndices = strip ? k.primCount + 2 : k.primCount * 3;
    std::vector<uint32_t> src(srcIndices);
    if (k.ib) {
        *failReason = kSkipIbPool;
        if (FAILED(k.ib->GetDesc(&id))) return tris;
        if (!((id.Pool == D3DPOOL_MANAGED || id.Pool == D3DPOOL_SYSTEMMEM) && !(id.Usage & D3DUSAGE_DYNAMIC))) return tris;
        const UINT isz = id.Format == D3DFMT_INDEX32 ? 4u : 2u;
        const UINT64 is = static_cast<UINT64>(k.startIndex) * isz, ib = static_cast<UINT64>(srcIndices) * isz;
        *failReason = kSkipRange;
        if (is + ib > id.Size) return tris;
        void* p = nullptr;
        *failReason = kSkipLock;
        if (FAILED(k.ib->Lock(static_cast<UINT>(is), static_cast<UINT>(ib), &p, D3DLOCK_READONLY))) return tris;
        for (UINT i = 0; i < srcIndices; ++i) {
            const int64_t raw = isz == 4 ? static_cast<const uint32_t*>(p)[i] : static_cast<const uint16_t*>(p)[i];
            src[i] = static_cast<uint32_t>(raw - (static_cast<int64_t>(k.firstVertex) - k.baseVertex));
        }
        k.ib->Unlock();
    } else {
        for (UINT i = 0; i < srcIndices; ++i) src[i] = i;
    }
    tris.reserve(k.primCount * 3);
    for (UINT t = 0; t < k.primCount; ++t) {
        uint32_t a, b, c;
        if (strip) { a = src[t]; b = src[t + 1]; c = src[t + 2]; if (keepWinding && (t & 1)) std::swap(b, c); }
        else       { a = src[t * 3]; b = src[t * 3 + 1]; c = src[t * 3 + 2]; }
        if (a >= k.numVertices || b >= k.numVertices || c >= k.numVertices) { *failReason = kSkipIndex; tris.clear(); return tris; }
        if (a == b || b == c || a == c) continue;
        tris.push_back(a); tris.push_back(b); tris.push_back(c);
    }
    if (tris.empty()) *failReason = kSkipEmpty;
    return tris;
}
}

MeshEntry* RaytracingBridge::GetOrCreateMesh(const DrawCallInfo& d, const MeshKey& k, const D3DVERTEXELEMENT9& pos, const D3DVERTEXELEMENT9* uv, const D3DVERTEXELEMENT9* color)
{
    auto it = m_meshes.find(k);
    if (it != m_meshes.end()) return &it->second;
    m_meshFail = kSkipBudget;
    if (m_meshesCreatedThisFrame >= static_cast<UINT>(m_maxMeshesPerFrame)) return nullptr;
    m_meshFail = kSkipVbPool;

    D3DVERTEXBUFFER_DESC vd;
    if (FAILED(k.vb->GetDesc(&vd))) return nullptr;
    if (!((vd.Pool == D3DPOOL_MANAGED || vd.Pool == D3DPOOL_SYSTEMMEM) && !(vd.Usage & D3DUSAGE_DYNAMIC))) return nullptr;
    const UINT64 vtxStart  = k.vbOffset + static_cast<UINT64>(k.firstVertex) * k.stride;
    const UINT64 vtxBytes  = static_cast<UINT64>(k.numVertices) * k.stride;
    m_meshFail = kSkipRange;
    if (k.numVertices == 0 || k.stride == 0 || vtxStart + vtxBytes > vd.Size) return nullptr;

    const std::vector<uint32_t> tris = ReadTriangles(k, &m_meshFail);
    if (tris.empty()) return nullptr;

    MeshEntry e;
    e.vertexCount = k.numVertices;
    e.indexCount  = static_cast<uint32_t>(tris.size());
    const size_t posBytes = static_cast<size_t>(e.vertexCount) * 12, uvBytes = static_cast<size_t>(e.vertexCount) * 8,
                 colBytes = static_cast<size_t>(e.vertexCount) * 4;
    e.data.assign(posBytes + uvBytes + colBytes + tris.size() * 4, 0);
    void* vp = nullptr;
    if (FAILED(k.vb->Lock(static_cast<UINT>(vtxStart), static_cast<UINT>(vtxBytes), &vp, D3DLOCK_READONLY))) { m_meshFail = kSkipLock; return nullptr; }
    auto* outPos = reinterpret_cast<float*>(e.data.data());
    auto* outUv  = reinterpret_cast<float*>(e.data.data() + posBytes);
    auto* outCol = reinterpret_cast<uint32_t*>(e.data.data() + posBytes + uvBytes);
    for (UINT v = 0; v < k.numVertices; ++v) {
        const uint8_t* vtx = static_cast<const uint8_t*>(vp) + static_cast<size_t>(v) * k.stride;
        memcpy(outPos + v * 3, vtx + pos.Offset, 12);
        for (int c = 0; c < 3; ++c) {
            e.bmin[c] = std::min(e.bmin[c], outPos[v * 3 + c]);
            e.bmax[c] = std::max(e.bmax[c], outPos[v * 3 + c]);
        }
        if (uv) memcpy(outUv + v * 2, vtx + uv->Offset, 8);
        outCol[v] = 0xFFFFFFFF;
        if (color) memcpy(&outCol[v], vtx + color->Offset, 4);
    }
    k.vb->Unlock();
    memcpy(e.data.data() + posBytes + uvBytes + colBytes, tris.data(), tris.size() * 4);
    (void)d;

    if (e.data.size() > br::kSlotDataBytes) { m_meshFail = kSkipTooBig; return nullptr; }
    e.id = m_nextMeshId++;
    k.vb->AddRef();
    if (k.ib) k.ib->AddRef();
    ++m_meshesCreatedThisFrame;
    return &m_meshes.emplace(k, std::move(e)).first->second;
}

void RaytracingBridge::CaptureInstance(const DrawCallInfo& d, const VsInfo& vi)
{
    auto skip = [&](int r) { ++m_skip[r]; };
    if (vi.skinned) return CaptureSkinned(d, vi);
    if (vi.world < 0) return skip(kSkipNoWorld);
    const bool glowDraw = d.renderStates && d.renderStates[D3DRS_ALPHABLENDENABLE] && d.renderStates[D3DRS_DESTBLEND] == D3DBLEND_ONE;
    if (d.primitiveCount < m_minPrims && !glowDraw) return skip(kSkipFew);

    const int baseSampler = d.pixelShaderHash ? PsShadowFor(d.pixelShaderHash).base : -1;
    const uint32_t texId = baseSampler >= 0 && baseSampler < 8 ? TextureIdFor(d.textures[baseSampler]) : 0;
    if (glowDraw && !texId) return skip(kSkipBlend);
    bool  alpha    = false;
    float alphaRef = 0.5f;
    if (d.renderStates && !glowDraw) {
        if (!d.renderStates[D3DRS_ZWRITEENABLE]) return skip(kSkipNoZWrite);
        if ((d.renderStates[D3DRS_COLORWRITEENABLE] & 0x7) == 0) return skip(kSkipNoColor);
        alpha = d.renderStates[D3DRS_ALPHATESTENABLE] || d.renderStates[D3DRS_ALPHABLENDENABLE] || PsUsesTexkill(d.pixelShaderHash);
        if (alpha && texId == 0) return skip(d.renderStates[D3DRS_ALPHATESTENABLE] ? kSkipAlphaTest : kSkipBlend);
        if (d.renderStates[D3DRS_ALPHATESTENABLE])
            alphaRef = std::max(d.renderStates[D3DRS_ALPHAREF] / 255.0f, 0.2f);
        else if (d.renderStates[D3DRS_ALPHABLENDENABLE])
            alphaRef = 0.9f;
    }
    if (d.primitiveType != D3DPT_TRIANGLELIST && d.primitiveType != D3DPT_TRIANGLESTRIP) return skip(kSkipPrimType);
    if (d.kind != DrawKind::IndexedPrimitive && d.kind != DrawKind::Primitive) return skip(kSkipUP);
    if (d.kind == DrawKind::IndexedPrimitive && !d.indexBuffer) return skip(kSkipUP);
    if (!d.vertexDeclaration) return skip(kSkipNoDecl);

    D3DVERTEXELEMENT9 el[MAXD3DDECLLENGTH + 1];
    UINT n = 0;
    if (FAILED(d.vertexDeclaration->GetDeclaration(el, &n))) return skip(kSkipNoDecl);
    const D3DVERTEXELEMENT9* pos = nullptr;
    for (UINT i = 0; i < n && el[i].Stream != 0xFF; ++i)
        if (el[i].Usage == D3DDECLUSAGE_POSITION && el[i].UsageIndex == 0) { pos = &el[i]; break; }
    if (!pos || pos->Stream >= 4 || !d.streamSource[pos->Stream]) return skip(kSkipNoDecl);
    if (pos->Type != D3DDECLTYPE_FLOAT3 && pos->Type != D3DDECLTYPE_FLOAT4) {
        if (m_skip[kSkipPosType]++ == 0)
            m_host->Log(LogLevel::Info, "RaytracingBridge: first position format skipped: decl type %u", pos->Type);
        return;
    }

    MeshKey k     = {};
    k.vb          = d.streamSource[pos->Stream];
    k.vbOffset    = d.streamOffset[pos->Stream];
    k.stride      = d.streamStride[pos->Stream];
    k.posOffset   = pos->Offset;
    k.primCount   = d.primitiveCount;
    k.primType    = d.primitiveType;
    if (d.kind == DrawKind::IndexedPrimitive) {
        k.ib          = d.indexBuffer;
        k.baseVertex  = d.baseVertexIndex;
        k.firstVertex = static_cast<UINT>(d.baseVertexIndex + static_cast<INT>(d.minVertexIndex));
        k.numVertices = d.numVertices;
        k.startIndex  = d.startIndexOrVertex;
    } else {
        k.firstVertex = d.startIndexOrVertex;
        k.numVertices = d.primitiveType == D3DPT_TRIANGLESTRIP ? d.primitiveCount + 2 : d.primitiveCount * 3;
    }

    const D3DVERTEXELEMENT9* uvEl = nullptr;
    for (UINT i = 0; i < n && el[i].Stream != 0xFF; ++i)
        if (el[i].Usage == D3DDECLUSAGE_TEXCOORD && el[i].UsageIndex == 0 && el[i].Type == D3DDECLTYPE_FLOAT2 &&
            el[i].Stream == pos->Stream) { uvEl = &el[i]; break; }

    const D3DVERTEXELEMENT9* colEl = nullptr;
    for (UINT i = 0; i < n && el[i].Stream != 0xFF; ++i)
        if (el[i].Usage == D3DDECLUSAGE_COLOR && el[i].UsageIndex == 0 && el[i].Type == D3DDECLTYPE_D3DCOLOR &&
            el[i].Stream == pos->Stream) { colEl = &el[i]; break; }

    MeshEntry* m = GetOrCreateMesh(d, k, *pos, uvEl, colEl);
    if (!m) return skip(m_meshFail);
    ++m_captured;
    m->lastUsed = d.frameIndex;

    float w[4][4];
    ReadMatrix(d.vsConstantsF + vi.world * 4, w);
    m_drawMesh = m;
    memcpy(m_drawWorld, w, sizeof(w));
    MeshEntry::Placed* slot = nullptr;
    for (MeshEntry::Placed& pl : m->placed) {
        const float dx = pl.transform[0][3] - w[0][3], dy = pl.transform[1][3] - w[1][3], dz = pl.transform[2][3] - w[2][3];
        if (dx * dx + dy * dy + dz * dz < 1.0f) {
            slot = &pl;
            if (pl.lastSeen != d.frameIndex && dx * dx + dy * dy + dz * dz > 1e-4f) m->moves = true;
            break;
        }
    }
    if (!slot) {
        if (m->placed.size() >= 256) return;
        m->placed.push_back({});
        slot = &m->placed.back();
        slot->firstSeen = d.frameIndex;
    }
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 4; ++j) slot->transform[i][j] = w[i][j];
    slot->lastSeen  = d.frameIndex;
    slot->hiddenInView = 0;
    if (d.renderStates && d.renderStates[D3DRS_ALPHABLENDENABLE] && d.renderStates[D3DRS_ZWRITEENABLE] && m->bmin[0] <= m->bmax[0]) {
        const float ex[3] = {m->bmax[0] - m->bmin[0], m->bmax[1] - m->bmin[1], m->bmax[2] - m->bmin[2]};
        const float dz = std::fabs(w[2][0]) * ex[0] + std::fabs(w[2][1]) * ex[1] + std::fabs(w[2][2]) * ex[2];
        const float dx = std::fabs(w[0][0]) * ex[0] + std::fabs(w[0][1]) * ex[1] + std::fabs(w[0][2]) * ex[2];
        const float dy = std::fabs(w[1][0]) * ex[0] + std::fabs(w[1][1]) * ex[1] + std::fabs(w[1][2]) * ex[2];
        const float mid[3] = {(m->bmin[0] + m->bmax[0]) * 0.5f, (m->bmin[1] + m->bmax[1]) * 0.5f, (m->bmin[2] + m->bmax[2]) * 0.5f};
        const float height = w[2][0] * mid[0] + w[2][1] * mid[1] + w[2][2] * mid[2] + w[2][3];
        if (dz < 0.1f && dx * dy > 1.0f && height < m_camera[2]) {
            m_drawIsMirrorFloor = true;
            alpha = false;
            if (m_mirrorFloors++ == 0) m_host->Log(LogLevel::Info, "RaytracingBridge: glossy floor over a reflection found: solid for rays and light (%.1f x %.1f)", dx, dy);
        }
    }
    slot->textureId = texId;
    const DWORD cull = d.renderStates ? d.renderStates[D3DRS_CULLMODE] : static_cast<DWORD>(D3DCULL_NONE);
    UINT gReg = 0, gCnt = 0, gSet = 0;
    const bool glass = d.pixelShaderHash && m_host->FindShaderConstant(ShaderStage::Pixel, d.pixelShaderHash, "tReflection", &gReg, &gCnt, &gSet) && gSet == 3;
    slot->flags     = (glass ? br::kInstanceGlass : 0u) | (alpha ? br::kInstanceAlphaTest : 0u) | (cull == D3DCULL_NONE ? br::kInstanceTwoSided : 0u) |
                      (cull == D3DCULL_CW ? br::kInstanceFrontCCW : 0u) | (glowDraw ? br::kInstanceGlow : 0u);
    {
        float em[3] = {0.0f, 0.0f, 0.0f};
        UINT reg = 0, cnt = 0, set = 0;
        if (d.pixelShaderHash && m_host->FindShaderConstant(ShaderStage::Pixel, d.pixelShaderHash, "cEmissive_Material", &reg, &cnt, &set) && set == 2)
            memcpy(em, d.psConstantsF + reg * 4, sizeof(em));
        else if (d.vertexShaderHash && m_host->FindShaderConstant(ShaderStage::Vertex, d.vertexShaderHash, "cEmissive_Material", &reg, &cnt, &set) && set == 2)
            memcpy(em, d.vsConstantsF + reg * 4, sizeof(em));
        const float peak = std::max({em[0], em[1], em[2]});
        if (std::isfinite(peak) && peak >= m_emitLampMin && !glass) {
            const auto q = [&](float v, int bits) { return static_cast<uint32_t>(std::clamp(v / peak, 0.0f, 1.0f) * ((1 << bits) - 1) + 0.5f); };
            slot->flags |= br::kInstanceGameLamp | (q(em[0], 5) << 27) | (q(em[1], 6) << 21) | (q(em[2], 5) << 16);
            slot->flags |= static_cast<uint32_t>(std::clamp(peak * m_emitGlow * 8.0f, 1.0f, 255.0f)) << br::kInstanceEmissiveShift;
        }
    }
    slot->alphaRef  = alphaRef;
}

uint32_t RaytracingBridge::TextureIdFor(IDirect3DBaseTexture9* base)
{
    if (!base || base->GetType() != D3DRTYPE_TEXTURE) return 0;
    auto* t = static_cast<IDirect3DTexture9*>(base);
    D3DSURFACE_DESC top;
    if (FAILED(t->GetLevelDesc(0, &top))) return 0;
    const TexKey key = {t, top.Width, top.Height, static_cast<UINT>(top.Format)};
    auto it = m_texIds.find(key);
    if (it != m_texIds.end()) return it->second;
    if (m_texCreatedThisFrame >= 8) return 0;
    ++m_texCreatedThisFrame;

    uint32_t id = 0;
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
    UINT blockBytes = 0;
    bool bc = true;
    switch (top.Format) {
    case D3DFMT_DXT1:     fmt = DXGI_FORMAT_BC1_UNORM; blockBytes = 8; break;
    case D3DFMT_DXT2:
    case D3DFMT_DXT3:     fmt = DXGI_FORMAT_BC2_UNORM; blockBytes = 16; break;
    case D3DFMT_DXT4:
    case D3DFMT_DXT5:     fmt = DXGI_FORMAT_BC3_UNORM; blockBytes = 16; break;
    case D3DFMT_A8R8G8B8: fmt = DXGI_FORMAT_B8G8R8A8_UNORM; blockBytes = 4; bc = false; break;
    default: break;
    }
    if (fmt != DXGI_FORMAT_UNKNOWN && (top.Pool == D3DPOOL_MANAGED || top.Pool == D3DPOOL_SYSTEMMEM)) {
        const DWORD levels = t->GetLevelCount();
        for (DWORD l = 0; l < levels && id == 0; ++l) {
            D3DSURFACE_DESC ld;
            if (FAILED(t->GetLevelDesc(l, &ld))) break;
            if (ld.Width > 256 || ld.Height > 256) continue;
            if (bc && ((ld.Width % 4) || (ld.Height % 4))) break;
            D3DLOCKED_RECT lr;
            if (FAILED(t->LockRect(l, &lr, nullptr, D3DLOCK_READONLY))) break;
            PendingTex pt;
            pt.op.id         = m_nextTexId;
            pt.op.width      = ld.Width;
            pt.op.height     = ld.Height;
            pt.op.dxgiFormat = fmt;
            pt.op.rowBytes   = bc ? (ld.Width / 4) * blockBytes : ld.Width * 4;
            pt.op.rows       = bc ? ld.Height / 4 : ld.Height;
            pt.data.resize(static_cast<size_t>(pt.op.rowBytes) * pt.op.rows);
            for (UINT r = 0; r < pt.op.rows; ++r)
                memcpy(pt.data.data() + static_cast<size_t>(r) * pt.op.rowBytes,
                       static_cast<const uint8_t*>(lr.pBits) + static_cast<size_t>(r) * lr.Pitch, pt.op.rowBytes);
            t->UnlockRect(l);
            m_pendingTex.push_back(std::move(pt));
            id = m_nextTexId++;
        }
    }
    m_texIds.emplace(key, id);
    return id;
}

void RaytracingBridge::CaptureSkinned(const DrawCallInfo& d, const VsInfo& vi)
{
    auto skip = [&](int r) { ++m_skip[r]; };
    if (d.kind != DrawKind::IndexedPrimitive || !d.indexBuffer || !d.vertexDeclaration) return skip(kSkipUP);
    if (d.primitiveType != D3DPT_TRIANGLELIST && d.primitiveType != D3DPT_TRIANGLESTRIP) return skip(kSkipPrimType);
    if (m_dynGeoms.size() >= br::kMaxDynamicGeoms) return skip(kSkipFull);

    D3DVERTEXELEMENT9 el[MAXD3DDECLLENGTH + 1];
    UINT n = 0;
    if (FAILED(d.vertexDeclaration->GetDeclaration(el, &n))) return skip(kSkipNoDecl);
    const D3DVERTEXELEMENT9* pos = nullptr, *wgt = nullptr, *idx = nullptr, *uvEl = nullptr, *colEl = nullptr;
    for (UINT i = 0; i < n && el[i].Stream != 0xFF; ++i) {
        if (el[i].UsageIndex != 0) continue;
        if (el[i].Usage == D3DDECLUSAGE_POSITION) pos = &el[i];
        else if (el[i].Usage == D3DDECLUSAGE_BLENDWEIGHT) wgt = &el[i];
        else if (el[i].Usage == D3DDECLUSAGE_BLENDINDICES) idx = &el[i];
        else if (el[i].Usage == D3DDECLUSAGE_TEXCOORD && el[i].Type == D3DDECLTYPE_FLOAT2) uvEl = &el[i];
        else if (el[i].Usage == D3DDECLUSAGE_COLOR && el[i].Type == D3DDECLTYPE_D3DCOLOR) colEl = &el[i];
    }
    if (!pos || !wgt || !idx || pos->Stream >= 4 || wgt->Stream != pos->Stream || idx->Stream != pos->Stream ||
        !d.streamSource[pos->Stream])
        return skip(kSkipNoDecl);
    if (pos->Type != D3DDECLTYPE_FLOAT3 && pos->Type != D3DDECLTYPE_FLOAT4) return skip(kSkipPosType);

    MeshKey k     = {};
    k.vb          = d.streamSource[pos->Stream];
    k.ib          = d.indexBuffer;
    k.vbOffset    = d.streamOffset[pos->Stream];
    k.stride      = d.streamStride[pos->Stream];
    k.posOffset   = pos->Offset;
    k.primCount   = d.primitiveCount;
    k.primType    = d.primitiveType;
    k.baseVertex  = d.baseVertexIndex;
    k.firstVertex = static_cast<UINT>(d.baseVertexIndex + static_cast<INT>(d.minVertexIndex));
    k.numVertices = d.numVertices;
    k.startIndex  = d.startIndexOrVertex;

    auto it = m_skin.find(k);
    if (it == m_skin.end()) {
        SkinMesh sm;
        D3DVERTEXBUFFER_DESC vd;
        int fail = kSkipVbPool;
        const UINT64 vtxStart = k.vbOffset + static_cast<UINT64>(k.firstVertex) * k.stride;
        const UINT64 vtxBytes = static_cast<UINT64>(k.numVertices) * k.stride;
        void* vp = nullptr;
        sm.bad = true;
        if (SUCCEEDED(k.vb->GetDesc(&vd)) && (vd.Pool == D3DPOOL_MANAGED || vd.Pool == D3DPOOL_SYSTEMMEM) &&
            !(vd.Usage & D3DUSAGE_DYNAMIC) && k.stride && k.numVertices && vtxStart + vtxBytes <= vd.Size) {
            sm.tris = ReadTriangles(k, &fail);
            if (!sm.tris.empty() && SUCCEEDED(k.vb->Lock(static_cast<UINT>(vtxStart), static_cast<UINT>(vtxBytes), &vp, D3DLOCK_READONLY))) {
                sm.pos.resize(k.numVertices * 3u);
                sm.uv.assign(k.numVertices * 2u, 0.0f);
                sm.weights.resize(k.numVertices * 3u);
                sm.colors.assign(k.numVertices, 0xFFFFFFFFu);
                sm.bones.resize(k.numVertices * 4u);
                for (UINT v = 0; v < k.numVertices; ++v) {
                    const uint8_t* vtx = static_cast<const uint8_t*>(vp) + static_cast<size_t>(v) * k.stride;
                    memcpy(&sm.pos[v * 3u], vtx + pos->Offset, 12);
                    if (uvEl && uvEl->Stream == pos->Stream) memcpy(&sm.uv[v * 2u], vtx + uvEl->Offset, 8);
                    if (colEl && colEl->Stream == pos->Stream) memcpy(&sm.colors[v], vtx + colEl->Offset, 4);
                    float w4[4], i4[4];
                    DecodeElement(vtx + wgt->Offset, wgt->Type, w4);
                    DecodeElement(vtx + idx->Offset, idx->Type, i4);
                    sm.weights[v * 3u] = w4[0]; sm.weights[v * 3u + 1] = w4[1]; sm.weights[v * 3u + 2] = w4[2];
                    sm.bones[v * 4u]     = static_cast<uint16_t>(i4[2] * 765.005859f);
                    sm.bones[v * 4u + 1] = static_cast<uint16_t>(i4[1] * 765.005859f);
                    sm.bones[v * 4u + 2] = static_cast<uint16_t>(i4[0] * 765.005859f);
                    sm.bones[v * 4u + 3] = static_cast<uint16_t>(i4[3] * 765.005859f);
                }
                k.vb->Unlock();
                sm.bad = false;
            }
        }
        if (sm.bad) m_meshFail = fail;
        k.vb->AddRef();
        k.ib->AddRef();
        it = m_skin.emplace(k, std::move(sm)).first;
    }
    SkinMesh& sm = it->second;
    sm.lastUsed = d.frameIndex;
    if (sm.bad) return skip(kSkipVbPool);

    const size_t posBytes = static_cast<size_t>(k.numVertices) * 12, uvBytes = static_cast<size_t>(k.numVertices) * 8,
                 colBytes = static_cast<size_t>(k.numVertices) * 4;
    const size_t bytes = posBytes + uvBytes + colBytes + sm.tris.size() * 4;
    if (m_dynData.size() + bytes > br::kMaxDynamicBytes) return skip(kSkipTooBig);

    const size_t offset = m_dynData.size();
    m_dynData.resize(offset + bytes);
    auto* out = reinterpret_cast<float*>(m_dynData.data() + offset);
    const float* bones = d.vsConstantsF + vi.bone * 4;
    const int regs = vi.boneCount;
    for (UINT v = 0; v < k.numVertices; ++v) {
        const float* w = &sm.weights[v * 3u];
        const float wt[4] = {w[0], w[1], w[2], 1.0f - w[0] - w[1] - w[2]};
        const uint16_t* bi = &sm.bones[v * 4u];
        const float* p = &sm.pos[v * 3u];
        float acc[3][4] = {};
        for (int b = 0; b < 4; ++b) {
            if (wt[b] == 0.0f || bi[b] + 2 >= regs) continue;
            const float* m = bones + bi[b] * 4;
            for (int row = 0; row < 3; ++row)
                for (int c = 0; c < 4; ++c) acc[row][c] += wt[b] * m[row * 4 + c];
        }
        for (int row = 0; row < 3; ++row)
            out[v * 3 + row] = acc[row][0] * p[0] + acc[row][1] * p[1] + acc[row][2] * p[2] + acc[row][3];
    }
    memcpy(m_dynData.data() + offset + posBytes, sm.uv.data(), uvBytes);
    memcpy(m_dynData.data() + offset + posBytes + uvBytes, sm.colors.data(), colBytes);
    memcpy(m_dynData.data() + offset + posBytes + uvBytes + colBytes, sm.tris.data(), sm.tris.size() * 4);

    const int baseSampler = d.pixelShaderHash ? PsShadowFor(d.pixelShaderHash).base : -1;
    if (IsSecondCharPass(d)) { m_dynData.resize(offset); return; }
    br::DynGeom g  = {};
    g.vertexCount  = k.numVertices;
    g.indexCount   = static_cast<uint32_t>(sm.tris.size());
    g.dataOffset   = offset;
    g.textureId    = baseSampler >= 0 && baseSampler < 8 ? TextureIdFor(d.textures[baseSampler]) : 0;
    m_dynGeoms.push_back(g);
    ++m_captured;
}

std::string RaytracingBridge::CaptureTexture(IDirect3DBaseTexture9* base)
{
    if (!base || base->GetType() != D3DRTYPE_TEXTURE) return std::string();
    auto found = m_capTex.find(base);
    if (found != m_capTex.end()) return found->second;
    std::string name;
    auto* t = static_cast<IDirect3DTexture9*>(base);
    const DWORD levels = t->GetLevelCount();
    for (DWORD l = 0; l < levels; ++l) {
        D3DSURFACE_DESC ld;
        if (FAILED(t->GetLevelDesc(l, &ld))) break;
        if (ld.Width > 256 || ld.Height > 256) continue;
        if (ld.Pool != D3DPOOL_MANAGED && ld.Pool != D3DPOOL_SYSTEMMEM) break;
        const bool bc = ld.Format == D3DFMT_DXT1 || ld.Format == D3DFMT_DXT2 || ld.Format == D3DFMT_DXT3 || ld.Format == D3DFMT_DXT4 || ld.Format == D3DFMT_DXT5;
        if (bc && ((ld.Width % 4) || (ld.Height % 4))) break;
        D3DLOCKED_RECT lr;
        if (FAILED(t->LockRect(l, &lr, nullptr, D3DLOCK_READONLY))) break;
        std::vector<uint8_t> rgba;
        const bool ok = framecap::DecodeLevel(static_cast<const uint8_t*>(lr.pBits), lr.Pitch, ld.Format, ld.Width, ld.Height, rgba);
        t->UnlockRect(l);
        if (ok) {
            char n[32];
            sprintf_s(n, "tex%u", m_capNextTex++);
            if (framecap::WriteTga(m_capDir + L"textures\\" + std::wstring(n, n + strlen(n)) + L".tga", ld.Width, ld.Height, rgba)) {
                name = n;
                if (m_capMtl) fprintf(m_capMtl, "newmtl %s\nmap_Kd textures/%s.tga\nmap_d textures/%s.tga\n", n, n, n);
            }
        }
        break;
    }
    m_capTex[base] = name;
    return name;
}

void RaytracingBridge::CaptureFrameBegin(uint64_t frameIndex)
{
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir = exe;
    dir = dir.substr(0, dir.find_last_of(L"\\/") + 1) + L"BullyDX12\\bridge\\dumps\\";
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t name[64];
    swprintf_s(name, L"frame_%02u%02u%02u", st.wHour, st.wMinute, st.wSecond);
    CreateDirectoryW(dir.c_str(), nullptr);
    m_capDir = dir + name + L"\\";
    CreateDirectoryW(m_capDir.c_str(), nullptr);
    CreateDirectoryW((m_capDir + L"textures").c_str(), nullptr);
    m_capObj = _wfopen((m_capDir + L"frame.obj").c_str(), L"w");
    m_capTxt = _wfopen((m_capDir + L"frame.txt").c_str(), L"w");
    m_capMtl = _wfopen((m_capDir + L"frame.mtl").c_str(), L"w");
    if (!m_capObj || !m_capTxt || !m_capMtl) {
        for (FILE** f : {&m_capObj, &m_capTxt, &m_capMtl}) if (*f) { fclose(*f); *f = nullptr; }
        m_host->Log(LogLevel::Warning, "RaytracingBridge: frame capture: cannot write in %ls", m_capDir.c_str());
        return;
    }
    setvbuf(m_capObj, nullptr, _IOFBF, 1 << 20);
    fprintf(m_capObj, "mtllib frame.mtl\n");
    fprintf(m_capMtl, "newmtl plain\nKd 0.6 0.6 0.6\n");
    fprintf(m_capTxt, "frame %llu\n", static_cast<unsigned long long>(frameIndex));
    m_capTex.clear();
    m_capDraw = m_capWritten = m_capMissing = m_capVerts = 0;
    m_capNextTex = 1;
    m_capRunning = true;
}

void RaytracingBridge::CaptureFrameDraw(const DrawCallInfo& d, const VsInfo& vi)
{
    const uint32_t n = m_capDraw++;
    if (!d.renderStates) return;
    const DWORD* rs = d.renderStates;
    const int baseSampler = d.pixelShaderHash ? PsShadowFor(d.pixelShaderHash).base : -1;
    IDirect3DBaseTexture9* baseTex = baseSampler >= 0 && baseSampler < 8 ? d.textures[baseSampler] : d.textures[0];
    const std::string tex = CaptureTexture(baseTex);
    {
        float em[3] = {0.0f, 0.0f, 0.0f};
        const char* where = "none";
        UINT reg = 0, cnt = 0, set = 0;
        if (d.pixelShaderHash && m_host->FindShaderConstant(ShaderStage::Pixel, d.pixelShaderHash, "cEmissive_Material", &reg, &cnt, &set) && set == 2) {
            memcpy(em, d.psConstantsF + reg * 4, sizeof(em)); where = "ps";
        } else if (d.vertexShaderHash && m_host->FindShaderConstant(ShaderStage::Vertex, d.vertexShaderHash, "cEmissive_Material", &reg, &cnt, &set) && set == 2) {
            memcpy(em, d.vsConstantsF + reg * 4, sizeof(em)); where = "vs";
        }
        fprintf(m_capTxt, "material %u tex %s emissive %.3f %.3f %.3f in %s ps %016llx\n", n, tex.empty() ? "none" : tex.c_str(), em[0], em[1], em[2], where, static_cast<unsigned long long>(d.pixelShaderHash));
    }
    auto missing = [&](const char* why) {
        ++m_capMissing;
        fprintf(m_capTxt, "missing %u %s prims %u skinned %d z %lu zw %lu blend %lu src %lu dst %lu at %lu tex %s ps %016llx vs %016llx\n", n, why, d.primitiveCount, vi.skinned ? 1 : 0,
                rs[D3DRS_ZENABLE], rs[D3DRS_ZWRITEENABLE], rs[D3DRS_ALPHABLENDENABLE], rs[D3DRS_SRCBLEND], rs[D3DRS_DESTBLEND], rs[D3DRS_ALPHATESTENABLE],
                tex.empty() ? "none" : tex.c_str(), static_cast<unsigned long long>(d.pixelShaderHash), static_cast<unsigned long long>(d.vertexShaderHash));
    };
    if (d.kind != DrawKind::IndexedPrimitive && d.kind != DrawKind::Primitive) return missing("user-pointer");
    if (d.primitiveType != D3DPT_TRIANGLELIST && d.primitiveType != D3DPT_TRIANGLESTRIP) return missing("primitive-type");
    if (!d.vertexDeclaration) return missing("no-declaration");
    if (d.kind == DrawKind::IndexedPrimitive && !d.indexBuffer) return missing("no-index-buffer");
    D3DVERTEXELEMENT9 el[MAXD3DDECLLENGTH + 1];
    UINT count = 0;
    if (FAILED(d.vertexDeclaration->GetDeclaration(el, &count))) return missing("no-declaration");
    const D3DVERTEXELEMENT9* pos = nullptr, *uvEl = nullptr, *colEl = nullptr, *wgt = nullptr, *idx = nullptr;
    for (UINT i = 0; i < count && el[i].Stream != 0xFF; ++i) {
        if (el[i].UsageIndex != 0) continue;
        if (el[i].Usage == D3DDECLUSAGE_POSITION) pos = &el[i];
        else if (el[i].Usage == D3DDECLUSAGE_TEXCOORD && el[i].Type == D3DDECLTYPE_FLOAT2) uvEl = &el[i];
        else if (el[i].Usage == D3DDECLUSAGE_COLOR && el[i].Type == D3DDECLTYPE_D3DCOLOR) colEl = &el[i];
        else if (el[i].Usage == D3DDECLUSAGE_BLENDWEIGHT) wgt = &el[i];
        else if (el[i].Usage == D3DDECLUSAGE_BLENDINDICES) idx = &el[i];
    }
    if (!pos || pos->Stream >= 4 || !d.streamSource[pos->Stream]) return missing("no-position");
    if (pos->Type != D3DDECLTYPE_FLOAT3 && pos->Type != D3DDECLTYPE_FLOAT4) return missing("position-format");
    const bool skinned = vi.skinned && wgt && idx && wgt->Stream == pos->Stream && idx->Stream == pos->Stream;
    if (vi.skinned && !skinned) return missing("skinning-format");

    MeshKey k     = {};
    k.vb          = d.streamSource[pos->Stream];
    k.ib          = d.kind == DrawKind::IndexedPrimitive ? d.indexBuffer : nullptr;
    k.vbOffset    = d.streamOffset[pos->Stream];
    k.stride      = d.streamStride[pos->Stream];
    k.posOffset   = pos->Offset;
    k.primCount   = d.primitiveCount;
    k.primType    = d.primitiveType;
    if (k.ib) {
        k.baseVertex  = d.baseVertexIndex;
        k.firstVertex = static_cast<UINT>(d.baseVertexIndex + static_cast<INT>(d.minVertexIndex));
        k.numVertices = d.numVertices;
        k.startIndex  = d.startIndexOrVertex;
    } else {
        k.firstVertex = d.startIndexOrVertex;
        k.numVertices = d.primitiveType == D3DPT_TRIANGLESTRIP ? d.primitiveCount + 2 : d.primitiveCount * 3;
    }
    D3DVERTEXBUFFER_DESC vd;
    if (FAILED(k.vb->GetDesc(&vd))) return missing("vertex-buffer");
    if (vd.Pool == D3DPOOL_DEFAULT && (vd.Usage & D3DUSAGE_WRITEONLY)) return missing("write-only-vertices");
    const UINT64 vtxStart = k.vbOffset + static_cast<UINT64>(k.firstVertex) * k.stride;
    const UINT64 vtxBytes = static_cast<UINT64>(k.numVertices) * k.stride;
    if (!k.stride || !k.numVertices || vtxStart + vtxBytes > vd.Size) return missing("vertex-range");
    int fail = 0;
    const std::vector<uint32_t> tris = ReadTriangles(k, &fail, true);
    if (tris.empty()) return missing("indices");
    void* vp = nullptr;
    if (FAILED(k.vb->Lock(static_cast<UINT>(vtxStart), static_cast<UINT>(vtxBytes), &vp, D3DLOCK_READONLY))) return missing("vertex-lock");

    float world[4][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};
    if (!vi.skinned && vi.world >= 0) ReadMatrix(d.vsConstantsF + vi.world * 4, world);
    const float* bones = vi.skinned ? d.vsConstantsF + vi.bone * 4 : nullptr;

    const bool alpha = rs[D3DRS_ALPHATESTENABLE] || rs[D3DRS_ALPHABLENDENABLE] || PsUsesTexkill(d.pixelShaderHash);
    fprintf(m_capObj, "g draw%u_flags%d\nusemtl %s\n", n, alpha ? 1 : 0, tex.empty() ? "plain" : tex.c_str());
    fprintf(m_capTxt, "drawstate %u %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %d\n", n, rs[D3DRS_ZENABLE], rs[D3DRS_ZWRITEENABLE], rs[D3DRS_ZFUNC], rs[D3DRS_ALPHABLENDENABLE], rs[D3DRS_SRCBLEND],
            rs[D3DRS_DESTBLEND], rs[D3DRS_ALPHATESTENABLE], rs[D3DRS_ALPHAREF], rs[D3DRS_ALPHAFUNC], rs[D3DRS_CULLMODE], rs[D3DRS_COLORWRITEENABLE], vi.skinned ? 1 : 0);
    for (UINT v = 0; v < k.numVertices; ++v) {
        const uint8_t* vtx = static_cast<const uint8_t*>(vp) + static_cast<size_t>(v) * k.stride;
        float p[3];
        memcpy(p, vtx + pos->Offset, 12);
        float o[3];
        if (skinned) {
            float w4[4], i4[4];
            DecodeElement(vtx + wgt->Offset, wgt->Type, w4);
            DecodeElement(vtx + idx->Offset, idx->Type, i4);
            const float wt[4] = {w4[0], w4[1], w4[2], 1.0f - w4[0] - w4[1] - w4[2]};
            const int bi[4] = {static_cast<int>(i4[2] * 765.005859f), static_cast<int>(i4[1] * 765.005859f), static_cast<int>(i4[0] * 765.005859f), static_cast<int>(i4[3] * 765.005859f)};
            float acc[3][4] = {};
            for (int b = 0; b < 4; ++b) {
                if (wt[b] == 0.0f || bi[b] + 2 >= vi.boneCount) continue;
                const float* m = bones + bi[b] * 4;
                for (int row = 0; row < 3; ++row)
                    for (int c = 0; c < 4; ++c) acc[row][c] += wt[b] * m[row * 4 + c];
            }
            for (int row = 0; row < 3; ++row) o[row] = acc[row][0] * p[0] + acc[row][1] * p[1] + acc[row][2] * p[2] + acc[row][3];
        } else {
            for (int row = 0; row < 3; ++row) o[row] = world[row][0] * p[0] + world[row][1] * p[1] + world[row][2] * p[2] + world[row][3];
        }
        float uv[2] = {0, 0};
        if (uvEl && uvEl->Stream == pos->Stream) memcpy(uv, vtx + uvEl->Offset, 8);
        uint32_t color = 0xFFFFFFFFu;
        if (colEl && colEl->Stream == pos->Stream) memcpy(&color, vtx + colEl->Offset, 4);
        fprintf(m_capObj, "v %.4f %.4f %.4f\nvt %.5f %.5f\nvc %08x\n", o[0], o[1], o[2], uv[0], 1.0f - uv[1], color);
    }
    k.vb->Unlock();
    const uint32_t base = m_capVerts + 1;
    for (size_t t = 0; t + 2 < tris.size(); t += 3)
        fprintf(m_capObj, "f %u/%u %u/%u %u/%u\n", base + tris[t], base + tris[t], base + tris[t + 1], base + tris[t + 1], base + tris[t + 2], base + tris[t + 2]);
    m_capVerts += k.numVertices;
    ++m_capWritten;
}

bool RaytracingBridge::CaptureFrameDrawGuarded(const DrawCallInfo& d, const VsInfo& vi)
{
    __try {
        CaptureFrameDraw(d, vi);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void RaytracingBridge::CaptureFrameAbort()
{
    m_capRunning = false;
    for (FILE** f : {&m_capObj, &m_capTxt, &m_capMtl}) if (*f) { fclose(*f); *f = nullptr; }
    m_capTex.clear();
    m_host->Log(LogLevel::Warning, "RaytracingBridge: frame capture stopped at draw %u: reading it failed (the game goes on)", m_capDraw);
}

void RaytracingBridge::CaptureFrameEnd(const FrameInfo& frame)
{
    m_capRunning = false;
    if (!m_capObj || !m_capTxt) return;
    const float* c = m_camera;
    fprintf(m_capTxt, "camera %.3f %.3f %.3f\nviewproj", c[0], c[1], c[2]);
    for (int r = 0; r < 4; ++r) for (int k = 0; k < 4; ++k) fprintf(m_capTxt, " %.7g", m_viewProj[r][k]);
    fprintf(m_capTxt, "\nclock %.3f\n", m_hours);
    int season = -1, from = -1, to = -1;
    float blend = 0.0f;
    if (ReadGameWeather(season, from, to, blend)) fprintf(m_capTxt, "weather season %d from %d to %d blend %.3f\n", season, from, to, blend);
    const uint32_t lightState = ReadGameLightState();
    fprintf(m_capTxt, "area %d hour %u cutscene %u\n", static_cast<int>(lightState & 0xff) - 1, (lightState >> 8) & 0xff, (lightState >> 16) & 1);
    for (const WorldLight& w : m_worldLights) {
        float radius = m_lightMaxRadius;
        if (w.att[2] > 1e-5f) radius = std::min(radius, std::sqrt(100.0f / w.att[2]));
        else if (w.att[1] > 1e-5f) radius = std::min(radius, 100.0f / w.att[1]);
        fprintf(m_capTxt, "light %.3f %.3f %.3f radius %.2f colour %.3f %.3f %.3f att %.4g %.4g %.4g\n", w.pos[0], w.pos[1], w.pos[2], radius, w.col[0], w.col[1], w.col[2], w.att[0], w.att[1], w.att[2]);
    }
    fprintf(m_capTxt, "capture draws %u written %u missing %u vertices %u textures %u\n", m_capDraw, m_capWritten, m_capMissing, m_capVerts, m_capNextTex - 1);
    for (FILE** f : {&m_capObj, &m_capTxt, &m_capMtl}) if (*f) { fclose(*f); *f = nullptr; }
    const bool image = framecap::WriteSurfaceBmp(m_host->GetDevice9(), frame.backBuffer, m_capDir + L"final.bmp");
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring ini = exe;
    ini = ini.substr(0, ini.find_last_of(L"\\/") + 1) + L"BullyDX12.ini";
    CopyFileW(ini.c_str(), (m_capDir + L"BullyDX12.ini").c_str(), FALSE);
    m_capTex.clear();
    m_host->Log(LogLevel::Info, "RaytracingBridge: frame capture in %ls: %u draws, %u written, %u could not be read (listed in frame.txt), %u textures, image %s",
                m_capDir.c_str(), m_capDraw, m_capWritten, m_capMissing, m_capNextTex - 1, image ? "saved" : "NOT saved");
}

DrawResult RaytracingBridge::OnDraw(const DrawCallInfo& d)
{
    if (!m_enabled) return DrawResult::Continue;
    const uint32_t drawIndex = m_drawIndex++;

    const bool zEnable = d.renderStates && d.renderStates[D3DRS_ZENABLE] != D3DZB_FALSE;
    bool       fullRT  = false;
    if (d.renderTarget0) {
        D3DSURFACE_DESC sd;
        fullRT = SUCCEEDED(d.renderTarget0->GetDesc(&sd)) && sd.Width == m_bbWidth && sd.Height == m_bbHeight &&
                 d.viewport.Width == sd.Width && d.viewport.Height == sd.Height;
    }

    const VsInfo* vi = d.vertexShaderHash ? &VsInfoFor(d.vertexShaderHash) : nullptr;
    bool candidate = !m_worldEnded && vi && vi->viewProj >= 0 && zEnable && fullRT && d.depthStencil;
    if (candidate && IsMirrored(d, *vi)) {
        candidate = false;
        if (m_fullRT && m_skipMirrored && !(m_header && m_header->rtDisabled)) return DrawResult::Skip;
    }
    if (candidate) RecordPass(d, *vi);
    if (m_capRunning && vi && vi->viewProj >= 0 && fullRT && !CaptureFrameDrawGuarded(d, *vi)) CaptureFrameAbort();
    const bool scene = candidate && (!m_mainRT || d.renderTarget0 == m_mainRT);

    if (m_traceFrame && d.frameIndex == m_traceFrame && drawIndex < 3000)
        m_host->Log(LogLevel::Info, "RTB trace #%u scene=%d z=%d zw=%d rt=%p ds=%p full=%d vs=%016llx vp=%d world=%d prims=%u",
                    drawIndex, scene, zEnable, d.renderStates ? d.renderStates[D3DRS_ZWRITEENABLE] != 0 : -1,
                    static_cast<void*>(d.renderTarget0), static_cast<void*>(d.depthStencil), fullRT,
                    static_cast<unsigned long long>(d.vertexShaderHash), vi ? vi->viewProj : -2, vi ? vi->world : -2,
                    d.primitiveCount);

    if (m_tracing) {
        if (scene) {
            ++m_traceSceneRun;
        } else {
            TraceFlush();
            DWORD blend = d.renderStates ? d.renderStates[D3DRS_ALPHABLENDENABLE] : 0;
            D3DSURFACE_DESC sd = {};
            if (d.renderTarget0) d.renderTarget0->GetDesc(&sd);
            m_host->Log(LogLevel::Info, "TRACE #%u draw rt=%p %ux%u fmt %d ds=%p tex0=%p tex1=%p prims=%u z=%d zw=%d blend=%lu src=%lu dst=%lu vp=%d viewport=%lux%lu",
                        drawIndex, static_cast<void*>(d.renderTarget0), sd.Width, sd.Height, static_cast<int>(sd.Format), static_cast<void*>(d.depthStencil),
                        static_cast<void*>(d.textures[0]), static_cast<void*>(d.textures[1]), d.primitiveCount, zEnable,
                        d.renderStates ? d.renderStates[D3DRS_ZWRITEENABLE] != 0 : -1, blend,
                        d.renderStates ? d.renderStates[D3DRS_SRCBLEND] : 0, d.renderStates ? d.renderStates[D3DRS_DESTBLEND] : 0,
                        vi ? vi->viewProj : -2, d.viewport.Width, d.viewport.Height);
        }
    }
    if (!scene && m_sunFromShader) NoteSunSprite(d);

    const bool outside = !scene && m_sceneDraws > 0 && (d.renderTarget0 != m_sceneRT || ((!vi || vi->viewProj < 0) && !zEnable));
    if (outside && !m_inRun) ++m_runs;
    m_inRun = outside || (m_inRun && !scene);
    if (scene) {
        m_inRun           = false;
        m_runsAtLastScene = m_runs;
        if (m_composited) ++m_lateScene;
    }
    if (m_composited) return DrawResult::Continue;
    if (outside && m_sceneDraws >= m_minSceneDraws && m_runs > m_prevRunsAtLastScene) {
        m_composited = true;
        if (Composite(m_sceneRT, m_sceneDS)) ++m_statsComposited;
        return DrawResult::Continue;
    }
    if (!scene) return DrawResult::Continue;

    if (m_sceneDraws == 0 && m_internal) ClearGBuffer(d.renderTarget0);
    ++m_sceneDraws;
    m_sceneRT = d.renderTarget0;
    m_sceneDS = d.depthStencil;

    const float* vp = d.vsConstantsF + vi->viewProj * 4;
    if (vi->camera >= 0) {
        if (!m_orientationCalibrated) {
            const float* c  = d.vsConstantsF + vi->camera * 4;
            const float  w0 = std::fabs(vp[12] * c[0] + vp[13] * c[1] + vp[14] * c[2] + vp[15]);
            const float  w1 = std::fabs(vp[3] * c[0] + vp[7] * c[1] + vp[11] * c[2] + vp[15]);
            const float  hi = std::max(w0, w1), lo = std::min(w0, w1);
            if (hi > 1e-6f && lo < 0.01f * hi) {
                m_orientation           = w0 <= w1 ? 0 : 1;
                m_orientationCalibrated = true;
                m_host->Log(LogLevel::Info, "RaytracingBridge: matrix orientation detected = %d (|w(camera)| %.3g vs %.3g)",
                            m_orientation, w0, w1);
            }
        }
    } else if (m_orientation < 0) {
        m_orientation = 0;
        m_host->Log(LogLevel::Info, "RaytracingBridge: provisional MatrixOrientation=0 until a shader exposes vPosition_Camera");
    }
    NoteLights(d);
    if (m_sunFromShader && d.pixelShaderHash) {
        const int sr = SunRegisterFor(d.pixelShaderHash);
        if (sr >= 0) {
            const float* s = d.psConstantsF + sr * 4;
            const float len = std::sqrt(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]);
            if (len > 1e-4f) {
                const float k = (m_invertSun ? -1.0f : 1.0f) / len;
                const float x = s[0] * k, y = s[1] * k, z = s[2] * k;
                if (z > m_sunMinZ) {
                    m_sunRaw[0] = x; m_sunRaw[1] = y; m_sunRaw[2] = z;
                    m_haveSunRaw = true;
                    const float c[3] = {x, y, z};
                    for (int i = 0; i < 3; ++i) { m_sunMin[i] = std::min(m_sunMin[i], c[i]); m_sunMax[i] = std::max(m_sunMax[i], c[i]); }
                }
            }
        }
    }

    m_drawIsMirrorFloor = false;
    m_drawMesh = nullptr;
    if (m_orientation >= 0) CaptureInstance(d, *vi);
    if (m_ownCull && m_cullRaster && m_drawMesh) {
        float vpm[4][4];
        ReadMatrix(vp, vpm);
        if (!BoxOnScreen(*m_drawMesh, m_drawWorld, vpm)) { ++m_cullSkipped; return DrawResult::Skip; }
        ++m_cullKept;
    }

    if (m_internal && d.pixelShaderHash) {
        const PsShadowInfo& si = PsShadowFor(d.pixelShaderHash);
        if (si.lightPos >= 0 && !m_lightLogged) {
            m_lightLogged = true;
            const float* lp = d.psConstantsF + si.lightPos * 4;
            UINT r = 0, c = 0, st = 0;
            auto pc = [&](const char* name) -> const float* {
                return m_host->FindShaderConstant(ShaderStage::Pixel, d.pixelShaderHash, name, &r, &c, &st)
                           ? d.psConstantsF + r * 4 : nullptr;
            };
            const float* dir = pc("vDirection_ShadowSpotLight0");
            const float* att = pc("vAttenuation_ShadowSpotLight0");
            const float* spot = pc("vSpotAttenuation_ShadowSpotLight0");
            m_host->Log(LogLevel::Info,
                        "RaytracingBridge: ShadowSpotLight0 pos (%.1f %.1f %.1f) camera (%.1f %.1f %.1f) dir (%.2f %.2f %.2f) "
                        "atten (%.3g %.3g %.3g) spot (%.3g %.3g %.3g)",
                        lp[0], lp[1], lp[2], m_camera[0], m_camera[1], m_camera[2], dir ? dir[0] : 0.f, dir ? dir[1] : 0.f,
                        dir ? dir[2] : 0.f, att ? att[0] : 0.f, att ? att[1] : 0.f, att ? att[2] : 0.f,
                        spot ? spot[0] : 0.f, spot ? spot[1] : 0.f, spot ? spot[2] : 0.f);
        }
        if (si.sunColor >= 0 && si.ambient >= 0) {
            memcpy(m_gameSunColor, d.psConstantsF + si.sunColor * 4, sizeof(m_gameSunColor));
            memcpy(m_gameSkyColor, d.psConstantsF + si.ambient * 4, sizeof(m_gameSkyColor));
            m_haveGameLight = true;
        }
    }
    if (m_internal && d.pixelShaderHash && (vi->skinned ? vi->outPos1 : vi->world >= 0) && DrawLit(d, *vi))
        return DrawResult::Skip;
    return DrawResult::Continue;
}

void RaytracingBridge::RecordPass(const DrawCallInfo& d, const VsInfo& vi)
{
    if (m_orientation < 0) return;
    float m[4][4];
    ReadMatrix(d.vsConstantsF + vi.viewProj * 4, m);
    auto samePass = [&](const Pass& p) {
        if (p.rt != d.renderTarget0) return false;
        for (int r : {0, 1, 3})
            for (int c = 0; c < 4; ++c)
                if (std::fabs(p.viewProj[r][c] - m[r][c]) > 1e-3f * (1.0f + std::fabs(m[r][c]))) return false;
        return true;
    };
    for (Pass& p : m_passes)
        if (samePass(p)) {
            p.prims += d.primitiveCount;
            ++p.draws;
            return;
        }
    if (m_passes.size() >= 16) return;
    Pass p = {};
    bool ok = false;
    if (vi.camera >= 0) {
        const float* c = d.vsConstantsF + vi.camera * 4;
        const float  w = std::fabs(m[3][0] * c[0] + m[3][1] * c[1] + m[3][2] * c[2] + m[3][3]);
        const float  scale = std::fabs(m[3][0] * c[0]) + std::fabs(m[3][1] * c[1]) + std::fabs(m[3][2] * c[2]) + std::fabs(m[3][3]);
        if (w <= 0.01f * scale + 1e-3f) {
            memcpy(p.camera, c, sizeof(p.camera));
            ok = true;
        }
    }
    if (!ok) ok = CameraFromViewProj(m, p.camera);
    if (!ok) { ++m_rejectedMatrices; return; }
    memcpy(p.viewProj, m, sizeof(m));
    p.rt    = d.renderTarget0;
    p.ds    = d.depthStencil;
    p.prims = d.primitiveCount;
    p.draws = 1;
    m_passes.push_back(p);
}

const RaytracingBridge::Pass* RaytracingBridge::MainPass() const
{
    const Pass* best = nullptr;
    for (const Pass& p : m_passes) {
        if (m_mainRT && p.rt != m_mainRT) continue;
        if (!best || p.prims > best->prims) best = &p;
    }
    return best;
}

void RaytracingBridge::WriteGameClock(float hours)
{
    float now = 0.0f;
    if (!ReadGameClock(now)) return;
    hours = std::fmod(std::fmod(hours, 24.0f) + 24.0f, 24.0f);
    const int h = static_cast<int>(hours), m = std::clamp(static_cast<int>((hours - h) * 60.0f), 0, 59);
    *reinterpret_cast<volatile uint8_t*>(0xBCEB4B) = static_cast<uint8_t>(h);
    *reinterpret_cast<volatile uint8_t*>(0xBCEB4A) = static_cast<uint8_t>(m);
    m_host->Log(LogLevel::Info, "RaytracingBridge: the game's clock set to %02d:%02d from the menu", h, m);
}

bool RaytracingBridge::ReadGameClock(float& hours) const
{
    static const uint8_t kClockGet[7] = {0x0F, 0xB6, 0x05, 0x4B, 0xEB, 0xBC, 0x00};
    static const int kChecked = []() {
        if (reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)) != 0x400000) return 0;
        MEMORY_BASIC_INFORMATION mi = {};
        if (!VirtualQuery(reinterpret_cast<const void*>(0x5BA590), &mi, sizeof(mi)) || mi.State != MEM_COMMIT) return 0;
        if (memcmp(reinterpret_cast<const void*>(0x5BA590), kClockGet, sizeof(kClockGet)) != 0) return 0;
        return VirtualQuery(reinterpret_cast<const void*>(0xBCEB4A), &mi, sizeof(mi)) && mi.State == MEM_COMMIT ? 1 : 0;
    }();
    if (!kChecked) return false;
    const uint8_t minute = *reinterpret_cast<const volatile uint8_t*>(0xBCEB4A), hour = *reinterpret_cast<const volatile uint8_t*>(0xBCEB4B);
    if (hour >= 24 || minute >= 60) return false;
    hours = hour + minute / 60.0f;
    return true;
}

bool RaytracingBridge::LoadTimeCycle()
{
    if (m_timeCycleState) return m_timeCycleState > 0;
    m_timeCycleState = -1;
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir = exe;
    dir = dir.substr(0, dir.find_last_of(L"\\/") + 1) + L"Config\\";
    std::vector<TimeRow> all(3 * 6 * 24);
    const wchar_t* const files[3] = {L"timecycF.dat", L"timecycW.dat", L"timecycS.dat"};
    for (int file = 0; file < 3; ++file) {
        FILE* f = _wfopen((dir + files[file]).c_str(), L"r");
        if (!f) { m_host->Log(LogLevel::Info, "RaytracingBridge: no %ls: the sky does not follow the weather", files[file]); return false; }
        char line[2048];
        int weather = -1, hour = 0;
        bool ok = true;
        while (fgets(line, sizeof(line), f)) {
            if (!strncmp(line, "//////", 6)) { ++weather; hour = 0; continue; }
            const char* p = line;
            while (*p == ' ' || *p == '\t') ++p;
            if (weather < 0 || *p == 0 || *p == '\n' || *p == '\r' || (p[0] == '/' && p[1] == '/')) continue;
            float v[40];
            int n = 0;
            for (char* q = const_cast<char*>(p); *q && n < 40;) {
                char* e = nullptr;
                const float x = strtof(q, &e);
                if (e == q) { ++q; continue; }
                v[n++] = x;
                q = e;
            }
            if (n < 29 || weather >= 6 || hour >= 24) { ok = false; break; }
            TimeRow& r = all[(file * 6 + weather) * 24 + hour];
            for (int c = 0; c < 3; ++c) { r.top[c] = v[16 + c] / 255.0f; r.bot[c] = v[19 + c] / 255.0f; }
            r.core = (v[22] * 0.3f + v[23] * 0.6f + v[24] * 0.1f) / 255.0f;
            ++hour;
        }
        fclose(f);
        if (!ok || weather != 5 || hour != 24) { m_host->Log(LogLevel::Warning, "RaytracingBridge: %ls is not as expected: the sky does not follow the weather", files[file]); return false; }
    }
    m_timeCycle.swap(all);
    m_timeCycleState = 1;
    return true;
}

bool RaytracingBridge::ReadGameWeather(int& season, int& from, int& to, float& blend) const
{
    static const uint8_t kWeatherGet[7] = {0x0F, 0xBF, 0x05, 0x6C, 0x75, 0xCE, 0x00};
    static const uint8_t kChapterGet[7] = {0x0F, 0xB6, 0x05, 0xE0, 0x47, 0xA1, 0x00};
    static const bool kChecked = []() {
        if (reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)) != 0x400000) return false;
        MEMORY_BASIC_INFORMATION mi = {};
        for (uintptr_t a : {uintptr_t(0x5BA740), uintptr_t(0x5C0D00), uintptr_t(0xCE7570), uintptr_t(0xA147E0)})
            if (!VirtualQuery(reinterpret_cast<const void*>(a), &mi, sizeof(mi)) || mi.State != MEM_COMMIT) return false;
        return memcmp(reinterpret_cast<const void*>(0x5BA740), kWeatherGet, 7) == 0 && memcmp(reinterpret_cast<const void*>(0x5C0D00), kChapterGet, 7) == 0;
    }();
    if (!kChecked) return false;
    const uint8_t chapter = *reinterpret_cast<const volatile uint8_t*>(0xA147E0);
    if (chapter >= 7) return false;
    season = *reinterpret_cast<const volatile int32_t*>(0xA147EC + chapter * 4);
    from   = *reinterpret_cast<const volatile uint16_t*>(0xCE7570);
    to     = *reinterpret_cast<const volatile uint16_t*>(0xCE7574);
    blend  = *reinterpret_cast<const volatile float*>(0xCE7568);
    return from < 6 && to < 6 && blend == blend;
}

uint32_t RaytracingBridge::ReadGameLightState() const
{
    static const uint8_t kArea[5] = {0xA1, 0x08, 0x10, 0xBD, 0x00};
    static const uint8_t kHour[6] = {0x8A, 0x0D, 0x4B, 0xEB, 0xBC, 0x00};
    static const bool kChecked = []() {
        if (reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)) != 0x400000) return false;
        MEMORY_BASIC_INFORMATION mi = {};
        for (uintptr_t a : {uintptr_t(0x41081E), uintptr_t(0x40FD8E), uintptr_t(0xBD1008), uintptr_t(0xBCEB4B), uintptr_t(0x20C5BE4)})
            if (!VirtualQuery(reinterpret_cast<const void*>(a), &mi, sizeof(mi)) || mi.State != MEM_COMMIT) return false;
        return memcmp(reinterpret_cast<const void*>(0x41081E), kArea, 5) == 0 && memcmp(reinterpret_cast<const void*>(0x40FD8E), kHour, 6) == 0;
    }();
    if (!kChecked) return 0;
    const int32_t area = *reinterpret_cast<const volatile int32_t*>(0xBD1008);
    if (area < 0 || area >= 64) return 0;
    const uint32_t hour = *reinterpret_cast<const volatile uint8_t*>(0xBCEB4B);
    const uint32_t cutscene = *reinterpret_cast<const volatile uint8_t*>(0x20C5BE4) ? 1u : 0u;
    return static_cast<uint32_t>(area + 1) | (hour << 8) | (cutscene << 16);
}

void RaytracingBridge::ApplyWeather(br::FrameSlot& s, float night)
{
    s.weatherBot[3] = 1.0f;
    s.weatherFog[0] = 1.0f;
    int season = 1, from = 0, to = 0;
    float blend = 0.0f;
    const bool fromGame = ReadGameWeather(season, from, to, blend);
    if (m_weatherOverride >= 0 && m_weatherOverride < 6) { from = to = m_weatherOverride; blend = 0.0f; }
    if (m_seasonOverride >= 0) season = m_seasonOverride;
    {
        rtcycle::Now& n = rtcycle::Current();
        n.season = season == 2 ? 1 : (season == 3 ? 2 : 0);
        n.from = std::clamp(from, 0, 5); n.to = std::clamp(to, 0, 5); n.blend = std::clamp(blend, 0.0f, 1.0f);
        n.hours = m_hours; n.fromGame = fromGame;
    }
    if (!m_followWeather || !LoadTimeCycle()) return;
    if (!fromGame && m_weatherOverride < 0 && m_seasonOverride < 0) return;
    blend = std::clamp(blend, 0.0f, 1.0f);
    const int  file   = season == 2 ? 1 : (season == 3 ? 2 : 0);
    const bool winter = season == 2;
    if (m_weatherLogged[0] != season || m_weatherLogged[1] != from || m_weatherLogged[2] != to) {
        m_weatherLogged[0] = season; m_weatherLogged[1] = from; m_weatherLogged[2] = to;
        m_host->Log(LogLevel::Info, "RaytracingBridge: weather: season %d, weather %d -> %d", season, from, to);
    }
    const int   h0 = static_cast<int>(m_hours) % 24, h1 = (h0 + 1) % 24;
    const float hf = m_hours - std::floor(m_hours);
    float top[3], bot[3], core = 0.0f;
    const auto& tc = m_timeCycle;
    auto row = [&](int w, int h) -> const TimeRow& { return tc[(file * 6 + w) * 24 + h]; };
    auto mix4 = [&](float a, float b, float c, float d) { return ((a * (1 - hf) + b * hf) * (1 - blend)) + ((c * (1 - hf) + d * hf) * blend); };
    for (int c = 0; c < 3; ++c) {
        top[c] = mix4(row(from, h0).top[c], row(from, h1).top[c], row(to, h0).top[c], row(to, h1).top[c]);
        bot[c] = mix4(row(from, h0).bot[c], row(from, h1).bot[c], row(to, h0).bot[c], row(to, h1).bot[c]);
    }
    core = mix4(row(from, h0).core, row(from, h1).core, row(to, h0).core, row(to, h1).core);
    static const float kGrey[6]        = {0.00f, 0.65f, 0.95f, 0.95f, 0.00f, 1.00f};
    static const float kGreyWinter[6]  = {0.75f, 0.85f, 0.95f, 0.95f, 0.60f, 1.00f};
    static const float kSun[6]         = {1.00f, 0.45f, 0.12f, 0.20f, 1.10f, 0.05f};
    static const float kFog[6]         = {1.00f, 1.30f, 2.50f, 5.00f, 0.80f, 4.00f};
    const float kCover[6] = {winter ? std::max(m_cloudCoverage, 0.6f) : m_cloudCoverage, 0.85f, 1.00f, 0.90f, std::min(m_cloudCoverage, 0.25f), 1.00f};
    auto pick = [&](const float* t) { return t[from] * (1.0f - blend) + t[to] * blend; };
    const float grey = pick(winter ? kGreyWinter : kGrey);
    const float sun  = pick(kSun) * (winter && from == 0 && to == 0 ? 0.8f : 1.0f);
    for (int c = 0; c < 3; ++c) {
        s.weatherTop[c] = top[c];
        s.weatherBot[c] = bot[c];
        if (night < 0.5f) s.sunColor[c] *= sun;
    }
    s.weatherTop[3] = grey * (1.0f - night);
    s.weatherBot[3] = std::clamp(core * 4.0f, 0.0f, 1.0f);
    s.weatherFog[0] = pick(kFog);
    s.clouds[0]     = pick(kCover);
}

void RaytracingBridge::UpdateClock()
{
    const uint64_t now = GetTickCount64();
    const float dt = m_clockTick ? std::min(static_cast<float>(now - m_clockTick) * 0.001f, 1.0f) : 0.0f;
    m_clockTick = now;
    m_seconds += dt;
    float gameHours = 0.0f;
    if (m_followGameClock && ReadGameClock(gameHours)) {
        float step = gameHours - m_hours;
        if (step > 12.0f) step -= 24.0f;
        if (step < -12.0f) step += 24.0f;
        m_hours += std::fabs(step) > 0.5f ? step : step * std::min(dt * 2.0f, 1.0f);
        if (static_cast<int>(gameHours) != m_lastLoggedHour) {
            m_lastLoggedHour = static_cast<int>(gameHours);
            m_host->Log(LogLevel::Info, "RaytracingBridge: following the game's clock, %.2f h", gameHours);
        }
    } else if (!m_clockPaused && m_dayMinutes > 0.0f) {
        m_hours += dt * 24.0f / (m_dayMinutes * 60.0f);
    }

    const bool mods = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_MENU) & 0x8000);
    const int keys[4] = {VK_PRIOR, VK_NEXT, VK_END, VK_HOME};
    for (int i = 0; i < 4; ++i) {
        const bool down = mods && (GetAsyncKeyState(keys[i]) & 0x8000);
        if (down && !m_keyWasDown[i]) {
            if (i <= 1) m_followGameClock = false;
            if (i == 0) m_hours += 1.0f;
            if (i == 1) m_hours -= 1.0f;
            if (i == 2) m_clockPaused = !m_clockPaused;
            if (i == 3) m_cloudCoverage = m_cloudCoverage >= 0.95f ? 0.0f : m_cloudCoverage + 0.15f;
            m_host->Log(LogLevel::Info, "RaytracingBridge: light system: %.1f h, clock %s, clouds %.2f", std::fmod(m_hours + 24.0f, 24.0f), m_clockPaused ? "paused" : "running", m_cloudCoverage);
        }
        m_keyWasDown[i] = down;
    }
    m_hours = std::fmod(m_hours + 24.0f, 24.0f);
}

void RaytracingBridge::FillOwnLight(br::FrameSlot& s)
{
    UpdateClock();
    constexpr float kRad = 0.0174532925f;
    const float h = (m_hours - 6.0f) / 24.0f * 6.2831853f;
    const float up = std::sin(h) * std::cos(m_tilt * kRad), side = std::sin(h) * std::sin(m_tilt * kRad), along = std::cos(h);
    const float az = m_azimuth * kRad;
    const float sun[3] = {along * std::cos(az) - side * std::sin(az), along * std::sin(az) + side * std::cos(az), up};

    auto smooth = [](float a, float b, float x) {
        const float k = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
        return k * k * (3.0f - 2.0f * k);
    };
    const float day   = smooth(-0.02f, 0.08f, sun[2]);
    const float night = 1.0f - smooth(-0.12f, 0.04f, sun[2]);
    const float z = std::max(sun[2], 0.0f);
    const float mass = 1.0f / (z + 0.15f * std::pow(std::max(93.885f - std::acos(z) / kRad, 1.0f), -1.253f));
    const float tau[3] = {0.075f, 0.15f, 0.32f};
    if (sun[2] > -0.02f) {
        for (int i = 0; i < 3; ++i) {
            s.sunDirection[i] = sun[i];
            s.sunColor[i]     = 1.15f * m_sunIntensity * std::exp(-tau[i] * mass) * day;
        }
    } else {
        const float moon[3] = {-sun[0], -sun[1], -sun[2]};
        const float bluish[3] = {0.16f, 0.20f, 0.34f};
        for (int i = 0; i < 3; ++i) {
            s.sunDirection[i] = moon[i];
            s.sunColor[i]     = bluish[i] * m_moonIntensity * smooth(0.02f, 0.12f, moon[2]);
        }
    }
    const float sl = std::sqrt(s.sunDirection[0] * s.sunDirection[0] + s.sunDirection[1] * s.sunDirection[1] + s.sunDirection[2] * s.sunDirection[2]) + 1e-12f;
    for (int i = 0; i < 3; ++i) {
        s.sunDirection[i] /= sl;
        s.skyColor[i]      = 0.7f * m_skyIntensity;
    }
    s.atmosphere[0] = 1.0f;
    s.atmosphere[1] = m_seconds;
    s.atmosphere[2] = m_bakedPools;
    s.atmosphere[3] = night;
    for (int i = 0; i < 3; ++i) s.trueSun[i] = sun[i];
    s.trueSun[3] = 28.0f * smooth(-0.12f, 0.05f, sun[2]);
    s.clouds[0] = m_cloudCoverage;
    s.clouds[1] = static_cast<float>(m_cloudSteps);
    s.clouds[2] = m_cloudBottom;
    s.clouds[3] = m_cloudTop;
    ApplyWeather(s, night);
    {
        const rtcycle::Key k = rtcycle::Sample(rtcycle::Current());
        const float light = k.v[rtcycle::kSun] * (1.0f - night) + k.v[rtcycle::kMoon] * night;
        for (int i = 0; i < 3; ++i) {
            s.sunColor[i] *= std::max(light, 0.0f) * std::max(k.v[rtcycle::kTintR + i], 0.0f);
            s.skyColor[i] *= std::max(k.v[rtcycle::kSky], 0.0f);
            s.weatherTop[i] *= std::max(k.v[rtcycle::kSky], 0.0f);
            s.weatherBot[i] *= std::max(k.v[rtcycle::kSky], 0.0f);
        }
        s.clouds[0]     = std::clamp(s.clouds[0] * k.v[rtcycle::kClouds], 0.0f, 1.0f);
        s.weatherFog[0] = (s.weatherFog[0] > 0.0f ? s.weatherFog[0] : 1.0f) * std::max(k.v[rtcycle::kFog], 0.05f);
        m_cycleAmbient  = std::max(k.v[rtcycle::kAmbient], 0.0f);
    }
    s.strength[1] = (m_params.shadowAmbient + (0.22f - m_params.shadowAmbient) * night) * m_cycleAmbient;
    s.hitBakedRadiance = m_hitBaked * (1.0f - 0.6f * night);
}

void RaytracingBridge::TraceFlush()
{
    if (m_traceSceneRun) m_host->Log(LogLevel::Info, "TRACE ... %u 3D scene draws", m_traceSceneRun);
    m_traceSceneRun = 0;
}

void RaytracingBridge::TracePixels(const char* when, IDirect3DSurface9* surface)
{
    if (!surface) return;
    D3DSURFACE_DESC sd = {};
    if (FAILED(surface->GetDesc(&sd))) return;
    if (sd.Format != D3DFMT_A8R8G8B8 && sd.Format != D3DFMT_X8R8G8B8) {
        m_host->Log(LogLevel::Info, "TRACE pixels %s: surface %p has format %d", when, static_cast<void*>(surface), static_cast<int>(sd.Format));
        return;
    }
    ComPtr<IDirect3DSurface9> sys;
    IDirect3DDevice9* dev = m_host->GetDevice9();
    if (FAILED(dev->CreateOffscreenPlainSurface(sd.Width, sd.Height, sd.Format, D3DPOOL_SYSTEMMEM, &sys, nullptr)) ||
        FAILED(dev->GetRenderTargetData(surface, sys.Get())))
        return;
    D3DLOCKED_RECT lr = {};
    if (FAILED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) return;
    auto at = [&](UINT x, UINT y) { return reinterpret_cast<const DWORD*>(static_cast<const BYTE*>(lr.pBits) + y * lr.Pitch)[x] & 0xFFFFFF; };
    const UINT w = sd.Width, h = sd.Height;
    m_host->Log(LogLevel::Info, "TRACE pixels %s (%p): centre %06lx low %06lx left %06lx right %06lx top %06lx", when, static_cast<void*>(surface),
                at(w / 2, h / 2), at(w / 2, h * 7 / 8), at(w / 8, h / 2), at(w * 7 / 8, h / 2), at(w / 2, h / 8));
    sys->UnlockRect();
}

bool RaytracingBridge::BoxOnScreen(const MeshEntry& m, const float w[4][4], const float vp[4][4]) const
{
    if (m.bmin[0] > m.bmax[0]) return true;
    float lo[3], hi[3];
    for (int a = 0; a < 3; ++a) {
        const float pad = 0.3f + 0.1f * (m.bmax[a] - m.bmin[a]);
        lo[a] = m.bmin[a] - pad;
        hi[a] = m.bmax[a] + pad;
    }
    uint32_t outAll = 0x3F;
    for (int i = 0; i < 8 && outAll; ++i) {
        const float o[3] = {(i & 1) ? hi[0] : lo[0], (i & 2) ? hi[1] : lo[1], (i & 4) ? hi[2] : lo[2]};
        float p[3];
        for (int r = 0; r < 3; ++r) p[r] = w[r][0] * o[0] + w[r][1] * o[1] + w[r][2] * o[2] + w[r][3];
        float c[4];
        for (int r = 0; r < 4; ++r) c[r] = vp[r][0] * p[0] + vp[r][1] * p[1] + vp[r][2] * p[2] + vp[r][3];
        uint32_t out = 0;
        if (c[0] < -c[3]) out |= 1;
        if (c[0] > c[3])  out |= 2;
        if (c[1] < -c[3]) out |= 4;
        if (c[1] > c[3])  out |= 8;
        if (c[2] < 0.0f)  out |= 16;
        if (c[2] > c[3])  out |= 32;
        outAll &= out;
    }
    return outAll == 0;
}

bool RaytracingBridge::InView(const MeshEntry& m, const MeshEntry::Placed& pl) const
{
    if (m.bmin[0] > m.bmax[0]) return false;
    for (int i = 0; i < 8; ++i) {
        const float o[3] = {(i & 1) ? m.bmax[0] : m.bmin[0], (i & 2) ? m.bmax[1] : m.bmin[1], (i & 4) ? m.bmax[2] : m.bmin[2]};
        float w[3];
        for (int r = 0; r < 3; ++r) w[r] = pl.transform[r][0] * o[0] + pl.transform[r][1] * o[1] + pl.transform[r][2] * o[2] + pl.transform[r][3];
        float c[4];
        for (int r = 0; r < 4; ++r) c[r] = m_viewProj[r][0] * w[0] + m_viewProj[r][1] * w[1] + m_viewProj[r][2] * w[2] + m_viewProj[r][3];
        if (c[3] <= 1e-3f || std::fabs(c[0]) > c[3] || std::fabs(c[1]) > c[3] || c[2] < 0 || c[2] > c[3]) return false;
    }
    return true;
}

bool RaytracingBridge::IndependentMasks()
{
    if (m_masksCap < 0) {
        D3DCAPS9 caps = {};
        m_masksCap = SUCCEEDED(m_host->GetDevice9()->GetDeviceCaps(&caps)) && (caps.PrimitiveMiscCaps & D3DPMISCCAPS_INDEPENDENTWRITEMASKS) ? 1 : 0;
        m_host->Log(LogLevel::Info, "RaytracingBridge: blended surfaces in the G-buffer: %s", m_masksCap ? "yes" : "no (no independent write masks)");
    }
    return m_masksCap == 1;
}

const std::vector<RaytracingBridge::LightRegs>& RaytracingBridge::LightRegsFor(ShaderStage stage, uint64_t hash)
{
    const uint64_t key = hash ^ (stage == ShaderStage::Pixel ? 0x51ED270B27ull : 0);
    auto it = m_lightRegs.find(key);
    if (it != m_lightRegs.end()) return it->second;
    std::vector<LightRegs> out;
    auto reg = [&](const std::string& name) {
        UINT r = 0, c = 0, set = 0;
        return m_host->FindShaderConstant(stage, hash, name.c_str(), &r, &c, &set) && set == 2 ? static_cast<int>(r) : -1;
    };
    for (const char* kind : {"PointLight", "SpotLight"})
        for (int i = 0; i < 4; ++i) {
            const std::string n = std::string(kind) + std::to_string(i);
            LightRegs l = {reg("vPosition_" + n), reg("cDiffuse_" + n), reg("vAttenuation_" + n), reg("vDirection_" + n), reg("vSpotAttenuation_" + n), stage == ShaderStage::Pixel};
            if (l.pos >= 0 && l.col >= 0) out.push_back(l);
        }
    return m_lightRegs.emplace(key, std::move(out)).first->second;
}

void RaytracingBridge::NoteLights(const DrawCallInfo& d)
{
    if (!m_gameLights) return;
    const uint64_t frame = d.frameIndex;
    auto read = [&](const std::vector<LightRegs>& regs) {
        for (const LightRegs& r : regs) {
            const float* k = r.pixel ? d.psConstantsF : d.vsConstantsF;
            const float* col = k + r.col * 4;
            if (col[0] + col[1] + col[2] < 0.003f) continue;
            const float* pos = k + r.pos * 4;
            if (r.att < 0 || (k[r.att * 4 + 1] < 1e-5f && k[r.att * 4 + 2] < 1e-5f)) continue;
            bool finite = true;
            for (int c = 0; c < 3; ++c) finite = finite && std::isfinite(pos[c]) && std::isfinite(col[c]) && std::isfinite(k[r.att * 4 + c]);
            if (!finite) continue;
            ++m_lightDraws;
            WorldLight* slot = nullptr;
            for (WorldLight& w : m_worldLights) {
                const float dx = w.pos[0] - pos[0], dy = w.pos[1] - pos[1], dz = w.pos[2] - pos[2];
                if (dx * dx + dy * dy + dz * dz < 0.25f) { slot = &w; break; }
            }
            if (!slot) {
                if (m_worldLights.size() >= 1024) continue;
                m_worldLights.push_back({});
                slot = &m_worldLights.back();
                if (m_worldLights.size() <= 40)
                    m_host->Log(LogLevel::Info, "RaytracingBridge: game light at (%.1f %.1f %.1f), colour (%.2f %.2f %.2f), attenuation (%.3g %.3g %.3g), %s", pos[0], pos[1], pos[2], col[0], col[1], col[2],
                                r.att >= 0 ? k[r.att * 4] : 1.f, r.att >= 0 ? k[r.att * 4 + 1] : 0.f, r.att >= 0 ? k[r.att * 4 + 2] : 0.f, r.dir >= 0 ? "spot" : "point");
            }
            memcpy(slot->pos, pos, sizeof(slot->pos));
            memcpy(slot->col, col, sizeof(slot->col));
            slot->att[0] = 1.0f; slot->att[1] = 0.0f; slot->att[2] = 0.0f;
            if (r.att >= 0) memcpy(slot->att, k + r.att * 4, sizeof(slot->att));
            slot->cosOuter = -2.0f;
            slot->cosInner = -2.0f;
            if (r.dir >= 0) {
                memcpy(slot->dir, k + r.dir * 4, sizeof(slot->dir));
                if (r.spot >= 0) {
                    slot->cosInner = k[r.spot * 4];
                    slot->cosOuter = k[r.spot * 4 + 1];
                }
            }
            slot->lastSeen = frame;
        }
    };
    if (d.pixelShaderHash) read(LightRegsFor(ShaderStage::Pixel, d.pixelShaderHash));
    if (d.vertexShaderHash) read(LightRegsFor(ShaderStage::Vertex, d.vertexShaderHash));
}

void RaytracingBridge::ReloadLive()
{
    IHost* h = m_host;
    const float menuTime = h->GetConfigFloat("Light", "TimeOfDay", m_hours);
    m_followGameClock = h->GetConfigInt("Light", "FollowGameClock", m_followGameClock ? 1 : 0) != 0;
    if (menuTime != m_menuTime) {
        if (m_menuTime >= 0.0f && m_followGameClock) WriteGameClock(menuTime);
        m_menuTime = menuTime;
        m_hours = menuTime;
    }
    m_dayMinutes      = h->GetConfigFloat("Light", "DayLengthMinutes", m_dayMinutes);
    m_clockPaused     = h->GetConfigInt("Light", "ClockPaused", m_clockPaused ? 1 : 0) != 0;
    m_azimuth         = h->GetConfigFloat("Light", "SunAzimuthDegrees", m_azimuth);
    m_tilt            = h->GetConfigFloat("Light", "SunTiltDegrees", m_tilt);
    m_moonIntensity   = h->GetConfigFloat("Light", "MoonIntensity", m_moonIntensity);
    m_cloudCoverage   = h->GetConfigFloat("Light", "CloudCoverage", m_cloudCoverage);
    m_cloudBottom     = h->GetConfigFloat("Light", "CloudBottom", m_cloudBottom);
    m_cloudTop        = h->GetConfigFloat("Light", "CloudTop", m_cloudTop);
    m_sunIntensity    = h->GetConfigFloat("RaytracingBridge", "SunIntensity", m_sunIntensity);
    m_skyIntensity    = h->GetConfigFloat("RaytracingBridge", "SkyIntensity", m_skyIntensity);
    m_giStrength      = h->GetConfigFloat("RaytracingBridge", "GiStrength", m_giStrength);
    m_reflStrength    = h->GetConfigFloat("RaytracingBridge", "ReflectionStrength", m_reflStrength);
    gamelights::Configure(h->GetConfigFloat("Light", "GameLampPower", 3.0f), h->GetConfigFloat("Light", "GameLampReach", 1.5f),
                          h->GetConfigFloat("Light", "CarLightPower", 1.5f));
    const float onHour = h->GetConfigFloat("Light", "LampsOnHour", 18.0f);
    if (onHour > 0.0f) gamelights::SetLampHours(onHour, h->GetConfigFloat("Light", "LampsOffHour", 7.0f), [](const std::string&) {});
}

bool RaytracingBridge::FillGameLamps(br::FrameSlot& s)
{
    if (!gamelights::Active()) return false;
    const std::vector<gamelights::Lamp>& lamps = gamelights::Lamps();
    struct Pick { float score; uint32_t i; };
    std::vector<Pick> picks;
    picks.reserve(lamps.size());
    for (uint32_t i = 0; i < lamps.size(); ++i) {
        const gamelights::Lamp& l = lamps[i];
        const float dx = l.pos[0] - m_camera[0], dy = l.pos[1] - m_camera[1], dz = l.pos[2] - m_camera[2];
        const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (d - l.reach > 120.0f) continue;
        const float power = l.col[0] * 0.3f + l.col[1] * 0.6f + l.col[2] * 0.1f;
        float score = power * l.reach * l.reach / std::max(d * d, 4.0f);
        if (std::find(m_lampKeys.begin(), m_lampKeys.end(), l.key) != m_lampKeys.end()) score *= 2.0f;
        picks.push_back({score, i});
    }
    std::sort(picks.begin(), picks.end(), [](const Pick& a, const Pick& b) { return a.score > b.score; });
    const uint32_t n = std::min<uint32_t>(static_cast<uint32_t>(picks.size()), br::kMaxLights);
    m_lampKeys.clear();
    for (uint32_t k = 0; k < n; ++k) {
        const gamelights::Lamp& l = lamps[picks[k].i];
        for (int c = 0; c < 3; ++c) {
            s.lightPos[k][c] = l.pos[c];
            s.lightCol[k][c] = l.col[c];
            s.lightDir[k][c] = 0.0f;
        }
        s.lightPos[k][3] = l.reach;
        for (int c = 0; c < 3; ++c) s.lightDir[k][c] = l.dir[c];
        s.lightCol[k][3] = l.cosOuter;
        s.lightDir[k][3] = l.cosInner;
        s.lightAtt[k][0] = 1.0f; s.lightAtt[k][1] = 0.0f; s.lightAtt[k][2] = 0.0f;
        s.lightAtt[k][3] = 0.0f;
        m_lampKeys.push_back(l.key);
    }
    s.lightCount = n;
    m_lightsSent = n;
    return true;
}

void RaytracingBridge::FillLights(br::FrameSlot& s)
{
    if (FillGameLamps(s)) return;
    const uint64_t frame = m_host->GetFrameIndex();
    for (size_t i = 0; i < m_worldLights.size();) {
        const WorldLight& wl = m_worldLights[i];
        const float ex = wl.pos[0] - m_camera[0], ey = wl.pos[1] - m_camera[1], ez = wl.pos[2] - m_camera[2];
        if (frame - wl.lastSeen > 300 && ex * ex + ey * ey + ez * ez > 120.f * 120.f) { m_worldLights[i] = m_worldLights.back(); m_worldLights.pop_back(); continue; }
        ++i;
    }
    auto dist2 = [&](const WorldLight& w) {
        const float dx = w.pos[0] - m_camera[0], dy = w.pos[1] - m_camera[1], dz = w.pos[2] - m_camera[2];
        return dx * dx + dy * dy + dz * dz;
    };
    std::sort(m_worldLights.begin(), m_worldLights.end(), [&](const WorldLight& a, const WorldLight& b) { return dist2(a) < dist2(b); });
    const uint32_t n = std::min<uint32_t>(static_cast<uint32_t>(m_worldLights.size()), br::kMaxLights);
    for (uint32_t i = 0; i < n; ++i) {
        const WorldLight& w = m_worldLights[i];
        float radius = m_lightMaxRadius;
        if (w.att[2] > 1e-5f) radius = std::min(radius, std::sqrt(100.0f / w.att[2]));
        else if (w.att[1] > 1e-5f) radius = std::min(radius, 100.0f / w.att[1]);
        s.lightPos[i][0] = w.pos[0]; s.lightPos[i][1] = w.pos[1]; s.lightPos[i][2] = w.pos[2]; s.lightPos[i][3] = radius;
        const float peak = std::max(std::max(w.col[0], w.col[1]), std::max(w.col[2], 1e-4f));
        float fill = 1.0f;
        if (peak < 0.1f) {
            const float t = std::clamp((w.pos[2] - m_camera[2] + 1.8f) / 1.0f, 0.0f, 1.0f);
            fill = t * t * (3.0f - 2.0f * t);
            if (w.att[2] >= 0.3f) fill = 0.0f;
        }
        for (int c = 0; c < 3; ++c) {
            s.lightCol[i][c] = w.col[c] / peak * m_lightIntensity;
            {
                const float low = std::min(w.col[0], std::min(w.col[1], w.col[2]));
                const float chroma = 1.0f - low / std::max(peak, 1e-5f);
                if (chroma > 0.85f || w.col[2] > w.col[0] || w.col[1] > w.col[0] * 1.05f) s.lightCol[i][c] = (s.lightCol[i][c] * 0.3f + m_lightIntensity * 0.7f) * 0.2f;
            }
            s.lightCol[i][c] *= fill;
            s.lightAtt[i][c] = w.att[c];
            s.lightDir[i][c] = w.dir[c];
        }
        s.lightCol[i][3] = w.cosOuter;
        s.lightDir[i][3] = w.cosInner;
        s.lightAtt[i][3] = 0.0f;
    }
    s.lightCount = n;
    m_lightsSent = n;
}

void RaytracingBridge::GpuListFailed()
{
    NoteFail(kFailHandoff);
    size_t freeTotal = 0, freeLargest = 0;
    MEMORY_BASIC_INFORMATION mi = {};
    for (const char* a = nullptr; VirtualQuery(a, &mi, sizeof(mi)) == sizeof(mi); a = static_cast<const char*>(mi.BaseAddress) + mi.RegionSize) {
        if (mi.State == MEM_FREE) {
            freeTotal += mi.RegionSize;
            freeLargest = std::max(freeLargest, static_cast<size_t>(mi.RegionSize));
        }
        if (static_cast<const char*>(mi.BaseAddress) + mi.RegionSize <= a) break;
    }
    m_host->Log(LogLevel::Error, "RaytracingBridge: GPU command list failed. Address space free %zu MB, largest block %zu MB", freeTotal >> 20, freeLargest >> 20);
    if (++m_gpuFailures >= 3) Disable("GPU command lists keep failing: the game continues without ray tracing");
}

bool RaytracingBridge::IsMirrored(const DrawCallInfo& d, const VsInfo& vi)
{
    if (m_orientation < 0) return false;
    float m[4][4];
    ReadMatrix(d.vsConstantsF + vi.viewProj * 4, m);
    const double det = m[0][0] * (m[1][1] * m[3][2] - m[1][2] * m[3][1]) - m[0][1] * (m[1][0] * m[3][2] - m[1][2] * m[3][0]) +
                       m[0][2] * (m[1][0] * m[3][1] - m[1][1] * m[3][0]);
    if (std::fabs(det) < 1e-9) return false;
    (det > 0 ? m_handPos : m_handNeg) += d.primitiveCount;
    const bool mirrored = (det > 0) != (m_handPos > m_handNeg);
    if (mirrored) m_mirrorFrame = d.frameIndex;
    if (mirrored && m_mirroredDraws++ == 0)
        m_host->Log(LogLevel::Info, "RaytracingBridge: a reflection is drawn with a mirrored camera: kept out of the ray traced world and view");
    return mirrored;
}

void RaytracingBridge::NoteFail(Fail f)
{
    static const char* const kNames[kFailCount] = {"no camera, target or depth", "bridge not running", "shared textures not open yet", "resources unavailable",
                                                   "slot still busy", "hand-off failed", "bridge late", "writing the image failed"};
    if (m_fail[f]++ == 0) m_host->Log(LogLevel::Warning, "RaytracingBridge: frame not ray traced: %s", kNames[f]);
}

bool RaytracingBridge::Composite(IDirect3DSurface9* target, IDirect3DSurface9* depth)
{
    if (!m_enabled) return false;
    if (const Pass* main = MainPass()) {
        memcpy(m_viewProj, main->viewProj, sizeof(m_viewProj));
        memcpy(m_camera, main->camera, sizeof(m_camera));
        m_haveCamera = true;
        target = main->rt;
        depth  = main->ds;
    }
    if (!target || !depth || !m_haveCamera) { NoteFail(kFailCamera); return false; }
    auto waitFor = [&](auto&& condition, DWORD ms) {
        const ULONGLONG end = GetTickCount64() + ms;
        while (!condition() && m_enabled && GetTickCount64() < end) SwitchToThread();
        return condition();
    };
    if (m_header->bridgeState != 1 && !waitFor([&] { return m_header->bridgeState == 1; }, 6000)) {
        NoteFail(kFailBridge);
        return false;
    }

    if (!m_bridgeRunningLogged) {
        m_bridgeRunningLogged = true;
        m_host->Log(LogLevel::Info, "RaytracingBridge: bridge running (DXR tier %u.%u in the 64-bit process)",
                    m_header->bridgeRaytracingTier / 10, m_header->bridgeRaytracingTier % 10);
    }

    D3DSURFACE_DESC td;
    target->GetDesc(&td);
    if (td.Width != m_width || td.Height != m_height) {
        m_colorFormat = td.Format == D3DFMT_X8R8G8B8 ? DXGI_FORMAT_B8G8R8X8_UNORM
                      : td.Format == D3DFMT_A16B16G16R16F ? DXGI_FORMAT_R16G16B16A16_FLOAT
                                                          : DXGI_FORMAT_B8G8R8A8_UNORM;
        if (!CreateSharedResources(td.Width, td.Height)) { Disable("shared texture creation failed"); return false; }
    }
    if (m_header->bridgeGeneration != m_generation && !waitFor([&] { return m_header->bridgeGeneration == m_generation; }, 6000)) {
        NoteFail(kFailGeneration);
        return false;
    }
    if (!EnsureVisTexture(m_width, m_height) || !EnsureGBuffer() || !EnsurePipelines(DXGI_FORMAT_UNKNOWN)) {
        NoteFail(kFailResources);
        return false;
    }

    ID3D12CommandQueue* queue = m_host->GetD3D12Queue();
    ID3D12GraphicsCommandList* cl = nullptr;
    auto barrier = [&](ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
        D3D12_RESOURCE_BARRIER x = {};
        x.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        x.Transition.pResource   = r;
        x.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        x.Transition.StateBefore = a;
        x.Transition.StateAfter  = b;
        cl->ResourceBarrier(1, &x);
    };
    const D3D12_RESOURCE_STATES kCommon = D3D12_RESOURCE_STATE_COMMON, kSrc = D3D12_RESOURCE_STATE_COPY_SOURCE,
                                kDst = D3D12_RESOURCE_STATE_COPY_DEST;

    const uint64_t frame = m_frameId + 1;
    const uint32_t slot  = static_cast<uint32_t>(frame & 1);
    br::FrameSlot& s     = m_header->slots[slot];
    if (!waitFor([&] { return s.state == br::kSlotFree; }, 250)) {
        NoteFail(kFailSlot);
        return false;
    }
    s.state = br::kSlotWriting;

    IDirect3DResource9* res9[5] = {depth, target, m_gbufTex[0].Get(), m_gbufTex[1].Get(), m_gbufTex[2].Get()};
    ComPtr<ID3D12Resource> res12[5];
    int unwrapped = 0;
    for (; unwrapped < 5; ++unwrapped)
        if (FAILED(m_host->UnwrapResource(res9[unwrapped], &res12[unwrapped]))) break;
    bool ok = unwrapped == 5 && res12[0]->GetDesc().SampleDesc.Count == 1 && EnsureDepthCopy(res12[0]->GetDesc());

    if (ok) {
        ID3D12Device* dev = m_host->GetD3D12Device();
        const UINT base = (m_ring++ % 64) * 4;
        D3D12_CPU_DESCRIPTOR_HANDLE srvCpu = m_srvHeap->GetCPUDescriptorHandleForHeapStart();
        D3D12_GPU_DESCRIPTOR_HANDLE srvGpu = m_srvHeap->GetGPUDescriptorHandleForHeapStart();
        D3D12_CPU_DESCRIPTOR_HANDLE rtv    = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
        srvCpu.ptr += static_cast<SIZE_T>(base) * m_srvInc;
        srvGpu.ptr += static_cast<UINT64>(base) * m_srvInc;
        rtv.ptr    += static_cast<SIZE_T>(base) * m_rtvInc;

        cl = m_host->BeginCommandList();
        barrier(res12[0].Get(), kCommon, kSrc);
        barrier(m_depthCopy.Get(), kCommon, kDst);
        cl->CopyResource(m_depthCopy.Get(), res12[0].Get());
        barrier(res12[0].Get(), kSrc, kCommon);
        const D3D12_RESOURCE_STATES depthRead = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_DEPTH_READ;
        barrier(m_depthCopy.Get(), kDst, depthRead);
        barrier(m_sharedDepth[slot].Get(), kCommon, D3D12_RESOURCE_STATE_RENDER_TARGET);
        D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
        sv.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.Texture2D.MipLevels     = 1;
        const DXGI_FORMAT cf = m_depthCopy->GetDesc().Format;
        sv.Format = cf == DXGI_FORMAT_R24G8_TYPELESS ? DXGI_FORMAT_R24_UNORM_X8_TYPELESS
                  : cf == DXGI_FORMAT_R32_TYPELESS   ? DXGI_FORMAT_R32_FLOAT
                                                     : DXGI_FORMAT_R16_UNORM;
        dev->CreateShaderResourceView(m_depthCopy.Get(), &sv, srvCpu);
        D3D12_RENDER_TARGET_VIEW_DESC rd = {};
        rd.Format        = DXGI_FORMAT_R32_FLOAT;
        rd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        dev->CreateRenderTargetView(m_sharedDepth[slot].Get(), &rd, rtv);
        ID3D12DescriptorHeap* heaps[] = {m_srvHeap.Get()};
        const D3D12_VIEWPORT vpt = {0, 0, static_cast<float>(m_width), static_cast<float>(m_height), 0, 1};
        const D3D12_RECT     sc  = {0, 0, static_cast<LONG>(m_width), static_cast<LONG>(m_height)};
        cl->SetDescriptorHeaps(1, heaps);
        cl->SetGraphicsRootSignature(m_rootSig.Get());
        cl->SetGraphicsRoot32BitConstants(0, sizeof(Params) / 4, &m_params, 0);
        cl->SetGraphicsRootDescriptorTable(1, srvGpu);
        cl->SetPipelineState(m_depthPso.Get());
        cl->RSSetViewports(1, &vpt);
        cl->RSSetScissorRects(1, &sc);
        cl->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cl->DrawInstanced(3, 1, 0, 0);
        barrier(m_depthCopy.Get(), depthRead, kCommon);
        barrier(m_sharedDepth[slot].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, kCommon);

        ID3D12Resource* src[4] = {res12[2].Get(), res12[3].Get(), res12[4].Get(), res12[1].Get()};
        ID3D12Resource* dst[4] = {m_sharedNormal[slot].Get(), m_sharedAlbedo[slot].Get(), m_sharedOpaque[slot].Get(), m_sharedColor[slot].Get()};
        for (int i = 0; i < 4; ++i) {
            barrier(src[i], kCommon, kSrc);
            barrier(dst[i], kCommon, kDst);
            cl->CopyResource(dst[i], src[i]);
            barrier(src[i], kSrc, kCommon);
            barrier(dst[i], kDst, kCommon);
        }
        if (!m_host->ExecuteCommandList(cl)) {
            ok = false;
            s.state = br::kSlotFree;
            for (int i = unwrapped - 1; i >= 0; --i) {
                m_host->DeferredRelease(res12[i].Detach());
                m_host->ReturnResource(res9[i]);
            }
            GpuListFailed();
            return false;
        }
        const uint64_t ready = ++m_gameValue;
        queue->Signal(m_fenceGame.Get(), ready);

        s.frameId         = frame;
        s.gameFenceValue  = ready;
        s.outputFreeValue = m_outputFree[slot];
        memcpy(s.viewProj, m_viewProj, sizeof(s.viewProj));
        s.cameraPos[0] = m_camera[0]; s.cameraPos[1] = m_camera[1]; s.cameraPos[2] = m_camera[2]; s.cameraPos[3] = 1;
        const float* sun = (m_sunFromShader && m_haveSun) ? m_sun : m_fallbackSun;
        const float sl = std::sqrt(sun[0] * sun[0] + sun[1] * sun[1] + sun[2] * sun[2]) + 1e-12f;
        s.sunDirection[0] = sun[0] / sl; s.sunDirection[1] = sun[1] / sl; s.sunDirection[2] = sun[2] / sl; s.sunDirection[3] = 0;
        s.tanSunRadius    = m_sunAngle;
        s.width           = m_width;
        s.height          = m_height;
        s.effects         = br::kEffectSunShadow;
        for (int i = 0; i < 3; ++i) {
            s.sunColor[i] = (m_haveSmoothLight ? m_smoothSunColor[i] : 1.0f) * m_sunIntensity;
            s.skyColor[i] = (m_haveSmoothLight ? m_smoothSkyColor[i] : 0.5f) * m_skyIntensity;
        }
        if (s.skyColor[0] + s.skyColor[1] + s.skyColor[2] < 0.03f) {
            const float l = (s.sunColor[0] * 0.3f + s.sunColor[1] * 0.6f + s.sunColor[2] * 0.1f) * 0.3f * m_skyIntensity;
            s.skyColor[0] = l * 0.55f; s.skyColor[1] = l * 0.7f; s.skyColor[2] = l;
        }
        s.strength[0]    = m_shadows ? m_params.shadowStrength : 0.0f;
        s.strength[1]    = m_params.shadowAmbient;
        s.strength[2]    = m_giStrength;
        s.strength[3]    = m_reflStrength;
        s.hitBakedRadiance = m_hitBaked;
        s.atmosphere[0] = 0.0f;
        if (m_ownLight) FillOwnLight(s);
        FillLights(s);
        s.reserved2 = ReadGameLightState() | (gamelights::Active() ? 1u << 24 : 0u);
        if (!m_followGameClock && (s.reserved2 & 0xffu)) {
            const uint32_t hr = static_cast<uint32_t>(std::fmod(std::fmod(m_hours, 24.0f) + 24.0f, 24.0f));
            s.reserved2 = (s.reserved2 & ~0xff00u) | (hr << 8);
        }
        s.denoisingRange = m_denoisingRange;
        s.resetHistory   = 0;
        s.fullRayTracing = m_fullRT ? 1u : 0u;

        uint8_t* data = m_view + m_header->slotDataOffset[slot];
        uint64_t used = 0;

        s.dynamicGeomCount = 0;
        if (!m_dynData.empty() && m_dynData.size() <= br::kMaxDynamicBytes) {
            memcpy(data, m_dynData.data(), m_dynData.size());
            used = (m_dynData.size() + 15) & ~15ull;
            s.dynamicGeomCount = static_cast<uint32_t>(m_dynGeoms.size());
            memcpy(s.dynamicGeoms, m_dynGeoms.data(), m_dynGeoms.size() * sizeof(br::DynGeom));
        }

        uint32_t texOps = 0;
        while (!m_pendingTex.empty() && texOps < br::kMaxTexOps && used + m_pendingTex.back().data.size() <= br::kSlotDataBytes / 2) {
            PendingTex& pt = m_pendingTex.back();
            memcpy(data + used, pt.data.data(), pt.data.size());
            pt.op.dataOffset = used;
            s.texOps[texOps++] = pt.op;
            used += (pt.data.size() + 15) & ~15ull;
            m_pendingTex.pop_back();
        }
        s.texOpCount = texOps;

        uint32_t ops = 0;
        while (!m_pendingDestroy.empty() && ops < br::kMaxMeshOps) {
            s.meshOps[ops++] = {br::kMeshDestroy, m_pendingDestroy.back(), 0, 0, 0};
            m_pendingDestroy.pop_back();
        }
        for (auto& kv : m_meshes) {
            MeshEntry& m = kv.second;
            if (m.sent) continue;
            if (ops >= br::kMaxMeshOps || used + m.data.size() > br::kSlotDataBytes) break;
            memcpy(data + used, m.data.data(), m.data.size());
            s.meshOps[ops++] = {br::kMeshCreate, m.id, m.vertexCount, m.indexCount, used};
            used += (m.data.size() + 15) & ~15ull;
            m.sent = true;
            std::vector<uint8_t>().swap(m.data);
        }
        s.meshOpCount = ops;

        const uint64_t now = m_host->GetFrameIndex();
        const float r2 = m_retainRadius * m_retainRadius;
        uint32_t ni = 0;
        uint32_t drawnNow = 0;
        for (const auto& kv : m_meshes)
            for (const MeshEntry::Placed& pl : kv.second.placed) if (pl.lastSeen == now) ++drawnNow;
        const bool worldDrawn = drawnNow >= 1;
        for (auto& kv : m_meshes) {
            MeshEntry& m = kv.second;
            if (!m.sent) continue;
            if (!m.moves) {
                bool jumped = false;
                for (const MeshEntry::Placed& a : m.placed) {
                    if (a.firstSeen != now) continue;
                    for (const MeshEntry::Placed& b : m.placed) {
                        if (b.lastSeen == now || now - b.lastSeen > 3) continue;
                        const float dx = a.transform[0][3] - b.transform[0][3], dy = a.transform[1][3] - b.transform[1][3], dz = a.transform[2][3] - b.transform[2][3];
                        if (dx * dx + dy * dy + dz * dz < 64.0f) { jumped = true; break; }
                    }
                    if (jumped) break;
                }
                if (jumped) {
                    if (now - m.moveSeen > 10) m.moveCount = 0;
                    m.moveSeen = now;
                    if (++m.moveCount >= 4) {
                        m.moves = true;
                        static int logged = 0;
                        if (logged++ < 30) m_host->Log(LogLevel::Info, "RaytracingBridge: mesh %u moves: not kept where it was last seen", m.id);
                    }
                }
            }
            const bool smallObject = m.bmin[0] <= m.bmax[0] && m.bmax[0] - m.bmin[0] <= 9.0f && m.bmax[1] - m.bmin[1] <= 9.0f && m.bmax[2] - m.bmin[2] <= 9.0f;
            for (size_t i = 0; i < m.placed.size();) {
                MeshEntry::Placed& pl = m.placed[i];
                if (now - pl.lastSeen > m_retainFrames) { pl = m.placed.back(); m.placed.pop_back(); continue; }
                if (m.moves && worldDrawn && now - pl.lastSeen > 3) { pl = m.placed.back(); m.placed.pop_back(); continue; }
                if ((m_dropHiddenInView || smallObject) && worldDrawn && pl.lastSeen != now && InView(m, pl)) {
                    if (++pl.hiddenInView > 20) {
                        ++m_lodDropped;
                        static int logged = 0;
                        if (logged++ < 30) m_host->Log(LogLevel::Info, "RaytracingBridge: mesh %u in view and no longer drawn: taken out of the rays", m.id);
                        pl = m.placed.back(); m.placed.pop_back();
                        continue;
                    }
                } else if (pl.lastSeen != now) {
                    pl.hiddenInView = 0;
                }
                const float dx = pl.transform[0][3] - m_camera[0], dy = pl.transform[1][3] - m_camera[1], dz = pl.transform[2][3] - m_camera[2];
                if (dx * dx + dy * dy + dz * dz <= r2 && ni < br::kMaxInstances) {
                    br::Instance& in = s.instances[ni++];
                    in.meshId = m.id;
                    in.mask   = 0xFF;
                    memcpy(in.transform, pl.transform, sizeof(in.transform));
                    in.textureId = pl.textureId;
                    in.flags     = pl.flags;
                    in.alphaRef  = pl.alphaRef;
                    in.reserved  = 0;
                }
                ++i;
            }
        }
        s.instanceCount = ni;
        MemoryBarrier();
        s.state   = br::kSlotReady;
        m_frameId = frame;
        SetEvent(m_frameEvent);
    } else {
        s.state = br::kSlotFree;
    }

    for (int i = unwrapped - 1; i >= 0; --i) {
        m_host->DeferredRelease(res12[i].Detach());
        m_host->ReturnResource(res9[i]);
    }
    if (!ok) {
        NoteFail(kFailHandoff);
        m_host->Log(LogLevel::Trace, "RaytracingBridge: hand-off failed, %d/5 resources unwrapped", unwrapped);
        return false;
    }

    const uint64_t done = waitFor([&] { return m_header->completedFrame >= frame; }, m_visValid ? 250 : 4000) ? m_header->completedFrame : 0;
    if (done < frame) { NoteFail(kFailLate); return false; }
    const uint32_t slotC = static_cast<uint32_t>(frame & 1);
    ComPtr<ID3D12Resource> vis12;
    if (FAILED(m_host->UnwrapResource(m_visTex.Get(), &vis12))) { NoteFail(kFailStretch); return false; }
    queue->Wait(m_fenceBridge.Get(), frame);
    cl = m_host->BeginCommandList();
    barrier(m_sharedOutput[slotC].Get(), kCommon, kSrc);
    barrier(vis12.Get(), kCommon, kDst);
    cl->CopyResource(vis12.Get(), m_sharedOutput[slotC].Get());
    barrier(vis12.Get(), kDst, kCommon);
    barrier(m_sharedOutput[slotC].Get(), kSrc, kCommon);
    const bool copied = m_host->ExecuteCommandList(cl) != 0;
    const uint64_t released = ++m_gameValue;
    queue->Signal(m_fenceGame.Get(), released);
    m_outputFree[slotC] = released;
    m_lastConsumed      = frame;
    m_host->DeferredRelease(vis12.Detach());
    m_host->ReturnResource(m_visTex.Get());
    if (!copied) {
        GpuListFailed();
        return false;
    }
    m_gpuFailures = 0;

    if (m_tracing) {
        TraceFlush();
        m_host->Log(LogLevel::Info, "TRACE composing onto rt=%p ds=%p, frame %llu", static_cast<void*>(target), static_cast<void*>(depth), static_cast<unsigned long long>(frame));
        TracePixels("scene before RT", target);
        m_traceSurface = target;
    }
    ComPtr<IDirect3DSurface9> image;
    if (FAILED(m_visTex->GetSurfaceLevel(0, &image)) ||
        FAILED(m_host->GetDevice9()->StretchRect(image.Get(), nullptr, target, nullptr, D3DTEXF_POINT))) {
        NoteFail(kFailStretch);
        return false;
    }
    if (m_tracing) TracePixels("scene after RT", target);
    if (!m_visValid) m_host->Log(LogLevel::Info, "RaytracingBridge: first RTX frame ready");
    m_visValid = true;
    return true;
}

void RaytracingBridge::NoteSunSprite(const DrawCallInfo& d)
{
    if (m_orientation < 0 || !d.renderStates || !d.vertexShaderHash || d.primitiveCount > 8 || m_spriteClips.size() >= 16) return;
    if (!d.renderStates[D3DRS_ALPHABLENDENABLE] || d.renderStates[D3DRS_DESTBLEND] != D3DBLEND_ONE) return;
    auto it = m_vsWvp.find(d.vertexShaderHash);
    if (it == m_vsWvp.end()) {
        UINT reg = 0, cnt = 0, set = 0;
        const bool found = m_host->FindShaderConstant(ShaderStage::Vertex, d.vertexShaderHash, "mWorldViewProjection", &reg, &cnt, &set) && set == 2;
        it = m_vsWvp.emplace(d.vertexShaderHash, found ? static_cast<int>(reg) : -1).first;
    }
    if (it->second < 0) return;
    float m[4][4];
    ReadMatrix(d.vsConstantsF + it->second * 4, m);
    m_spriteClips.push_back({d.vertexShaderHash * 31 + d.pixelShaderHash + d.primitiveCount, {m[0][3], m[1][3], m[2][3], m[3][3]}});
}

void RaytracingBridge::UpdateSunFromSprites()
{
    if (m_sceneDraws == 0 || m_spriteClips.empty()) { m_spriteClips.clear(); return; }
    double a[4][8];
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) { a[i][j] = m_viewProj[i][j]; a[i][j + 4] = i == j ? 1.0 : 0.0; }
    for (int col = 0; col < 4; ++col) {
        int piv = col;
        for (int r = col + 1; r < 4; ++r) if (std::fabs(a[r][col]) > std::fabs(a[piv][col])) piv = r;
        if (std::fabs(a[piv][col]) < 1e-12) { m_spriteClips.clear(); return; }
        if (piv != col) for (int j = 0; j < 8; ++j) std::swap(a[piv][j], a[col][j]);
        const double k = 1.0 / a[col][col];
        for (int j = 0; j < 8; ++j) a[col][j] *= k;
        for (int r = 0; r < 4; ++r) {
            if (r == col) continue;
            const double f = a[r][col];
            for (int j = 0; j < 8; ++j) a[r][j] -= f * a[col][j];
        }
    }
    float forward[3] = {m_viewProj[3][0], m_viewProj[3][1], m_viewProj[3][2]};
    const float fl = std::sqrt(forward[0] * forward[0] + forward[1] * forward[1] + forward[2] * forward[2]) + 1e-12f;
    for (float& v : forward) v /= fl;
    const uint64_t frame = m_host->GetFrameIndex();
    for (const SpriteClip& s : m_spriteClips) {
        double w[4] = {};
        for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) w[r] += a[r][c + 4] * s.clip[c];
        if (std::fabs(w[3]) < 1e-9) continue;
        float dir[3];
        for (int i = 0; i < 3; ++i) dir[i] = static_cast<float>(w[i] / w[3]) - m_camera[i];
        const float l = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
        if (l < 100.0f) continue;
        for (float& v : dir) v /= l;
        if (dir[2] > 0.985f) continue;
        auto it = m_sunCandidates.find(s.key);
        if (it == m_sunCandidates.end()) {
            if (m_sunCandidates.size() >= 32) continue;
            SunCandidate c = {};
            memcpy(c.dir, dir, sizeof(dir));
            memcpy(c.forward, forward, sizeof(forward));
            c.seen = frame;
            m_sunCandidates.emplace(s.key, c);
            m_host->Log(LogLevel::Info, "RaytracingBridge: sky sprite %016llx, direction (%.3f %.3f %.3f), distance %.0f", static_cast<unsigned long long>(s.key), dir[0], dir[1], dir[2], l);
            continue;
        }
        SunCandidate& c = it->second;
        const float same = c.dir[0] * dir[0] + c.dir[1] * dir[1] + c.dir[2] * dir[2];
        if (same < 0.9995f) {
            c.stable = 0;
            c.swing  = 0;
            memcpy(c.forward, forward, sizeof(forward));
            if (m_sunSprite == s.key) {
                m_sunSprite = 0;
                m_host->Log(LogLevel::Info, "RaytracingBridge: sky sprite %016llx stopped being stable, back to the shadow direction", static_cast<unsigned long long>(s.key));
            }
        } else {
            ++c.stable;
            const float turn = c.forward[0] * forward[0] + c.forward[1] * forward[1] + c.forward[2] * forward[2];
            c.swing = std::max(c.swing, 1.0f - turn);
        }
        memcpy(c.dir, dir, sizeof(dir));
        c.seen = frame;
        if (m_sunFromSprite && !m_sunSprite && c.stable >= 120 && c.swing > 0.05f && dir[2] > m_sunMinZ) {
            m_sunSprite = s.key;
            m_host->Log(LogLevel::Info, "RaytracingBridge: sun position from sky sprite %016llx: (%.3f %.3f %.3f)", static_cast<unsigned long long>(s.key), dir[0], dir[1], dir[2]);
        }
        if (m_sunSprite == s.key && dir[2] > m_sunMinZ) {
            memcpy(m_sunRaw, dir, sizeof(dir));
            m_haveSunRaw = true;
        }
    }
    m_spriteClips.clear();
}

void RaytracingBridge::EndFrame()
{
    UpdateSunFromSprites();
    {
        std::unordered_map<IDirect3DSurface9*, uint64_t> perTarget;
        IDirect3DSurface9* top = nullptr;
        for (const Pass& p : m_passes) {
            const uint64_t n = perTarget[p.rt] += p.prims;
            if (!top || n > perTarget[top]) top = p.rt;
        }
        if (top && top != m_mainRT) {
            m_host->Log(LogLevel::Info, "RaytracingBridge: main target is now %p (%zu 3D passes this frame)", static_cast<void*>(top), m_passes.size());
            m_mainRT = top;
        }
        if (m_host->GetFrameIndex() % 600 == 0) {
            for (const Pass& p : m_passes)
                m_host->Log(LogLevel::Info, "RaytracingBridge: pass rt=%p ds=%p%s: %u draws, %llu triangles, camera (%.1f %.1f %.1f)", static_cast<void*>(p.rt), static_cast<void*>(p.ds),
                            p.rt == m_mainRT ? " MAIN" : "", p.draws, static_cast<unsigned long long>(p.prims), p.camera[0], p.camera[1], p.camera[2]);
        }
        m_passes.clear();
    }
    if (m_sceneDraws > 0 && m_orientation >= 0) {
        memcpy(m_prevViewProj, m_viewProj, sizeof(m_prevViewProj));
        m_havePrevViewProj = true;
    }
    if (m_haveSunRaw) {
        const uint64_t now = GetTickCount64();
        if (!m_haveSun) {
            memcpy(m_sun, m_sunRaw, sizeof(m_sun));
            m_host->Log(LogLevel::Info, "RaytracingBridge: sun direction from the game (%.3f %.3f %.3f)", m_sun[0], m_sun[1], m_sun[2]);
        } else {
            const float cosA  = std::min(1.0f, std::max(-1.0f, m_sun[0] * m_sunRaw[0] + m_sun[1] * m_sunRaw[1] + m_sun[2] * m_sunRaw[2]));
            const float angle = std::acos(cosA) * 57.29578f;
            if (angle > 20.0f) {
                if (!m_sunFarSince) m_sunFarSince = now;
                if (now - m_sunFarSince > 2000) {
                    memcpy(m_sun, m_sunRaw, sizeof(m_sun));
                    m_sunFarSince = 0;
                    m_host->Log(LogLevel::Info, "RaytracingBridge: sun direction changed by %.0f degrees, now (%.3f %.3f %.3f)", angle, m_sun[0], m_sun[1], m_sun[2]);
                }
            } else {
                m_sunFarSince = 0;
                const float dt   = std::min(static_cast<float>(now - m_sunTick) * 0.001f, 0.25f);
                const float step = std::min(1.0f, m_sunMaxRate * dt / std::max(angle, 1e-4f));
                float len = 0;
                for (int i = 0; i < 3; ++i) { m_sun[i] += (m_sunRaw[i] - m_sun[i]) * step; len += m_sun[i] * m_sun[i]; }
                len = std::sqrt(len) + 1e-12f;
                for (float& v : m_sun) v /= len;
            }
        }
        m_sunTick    = now;
        m_haveSun    = true;
        m_haveSunRaw = false;
    }
    if (m_haveGameLight) {
        const float a = m_haveSmoothLight ? 0.05f : 1.0f;
        for (int i = 0; i < 3; ++i) {
            m_smoothSunColor[i] += (m_gameSunColor[i] - m_smoothSunColor[i]) * a;
            m_smoothSkyColor[i] += (m_gameSkyColor[i] - m_smoothSkyColor[i]) * a;
        }
        m_haveSmoothLight = true;
        m_haveGameLight   = false;
    }
    const uint64_t frame = m_host->GetFrameIndex();
    if (frame % 60 == 0) {
        for (auto it = m_meshes.begin(); it != m_meshes.end();) {
            if (frame - it->second.lastUsed > m_evictAfter) {
                if (it->second.sent) m_pendingDestroy.push_back(it->second.id);
                it->first.vb->Release();
                if (it->first.ib) it->first.ib->Release();
                it = m_meshes.erase(it);
            } else {
                ++it;
            }
        }
        for (auto it = m_skin.begin(); it != m_skin.end();) {
            if (frame - it->second.lastUsed > m_evictAfter) {
                it->first.vb->Release();
                if (it->first.ib) it->first.ib->Release();
                it = m_skin.erase(it);
            } else {
                ++it;
            }
        }
    }
    m_dynDrawsLast = m_dynGeoms.size();
    m_dynData.clear();
    m_dynGeoms.clear();
    m_texCreatedThisFrame = 0;
    m_prevRunsAtLastScene    = m_runsAtLastScene;
    m_runs = m_runsAtLastScene = 0;
    m_inRun                  = false;
    m_worldEnded             = false;
    m_drawIndex              = 0;
    m_sceneDraws             = 0;
    m_composited             = false;
    m_sceneRT = m_sceneDS    = nullptr;
    m_meshesCreatedThisFrame = 0;
}

void RaytracingBridge::OnClear(DWORD flags, IDirect3DSurface9* renderTarget0, IDirect3DSurface9* depthStencil)
{
    if (m_tracing) {
        TraceFlush();
        m_host->Log(LogLevel::Info, "TRACE clear flags=0x%lx rt=%p ds=%p (scene rt=%p ds=%p, composed=%d)", flags, static_cast<void*>(renderTarget0),
                    static_cast<void*>(depthStencil), static_cast<void*>(m_sceneRT), static_cast<void*>(m_sceneDS), m_composited);
    }
    if (!m_enabled || !(flags & D3DCLEAR_ZBUFFER) || depthStencil != m_sceneDS || !m_sceneRT) return;
    if (m_sceneDraws < m_minSceneDraws) return;
    m_worldEnded = true;
    if (m_composited) return;
    m_composited = true;
    ++m_clearComposites;
    if (Composite(m_sceneRT, m_sceneDS)) ++m_statsComposited;
    (void)renderTarget0;
}

void RaytracingBridge::OnCopySurface(IDirect3DSurface9* source, IDirect3DSurface9* dest)
{
    if (m_tracing) {
        TraceFlush();
        m_host->Log(LogLevel::Info, "TRACE copy src=%p -> dst=%p (scene rt=%p, composed=%d)", static_cast<void*>(source), static_cast<void*>(dest),
                    static_cast<void*>(m_sceneRT), m_composited);
    }
    if (!m_enabled || !source || source != m_sceneRT || m_sceneDraws < m_minSceneDraws) return;
    m_worldEnded = true;
    if (m_composited) return;
    m_composited = true;
    ++m_copyComposites;
    if (Composite(m_sceneRT, m_sceneDS)) ++m_statsComposited;
}

void RaytracingBridge::OnPresent(const FrameInfo& frame)
{
    gamelights::EndFrame();
    menu::Frame(m_host->GetDevice9());
    if (m_tracing) {
        TraceFlush();
        TracePixels("composed target at Present", m_traceSurface);
        TracePixels("back buffer at Present", frame.backBuffer);
        m_host->Log(LogLevel::Info, "TRACE end of frame (back buffer %p, composed %d, composites by copy %llu, by clear %llu)", static_cast<void*>(frame.backBuffer), m_composited,
                    static_cast<unsigned long long>(m_copyComposites), static_cast<unsigned long long>(m_clearComposites));
        m_tracing = false;
        m_traceSurface = nullptr;
    }
    if (m_statsFrames % 900 == 299 && static_cast<int>(m_traceCount) < m_traceMax) {
        ++m_traceCount;
        m_tracing = true;
        m_host->Log(LogLevel::Info, "TRACE start, camera (%.1f %.1f %.1f)", m_camera[0], m_camera[1], m_camera[2]);
    }
    if (m_debugGBuffer && m_gbufSurf[0] && frame.backBuffer)
        m_host->GetDevice9()->StretchRect(m_gbufSurf[0].Get(), nullptr, frame.backBuffer, nullptr, D3DTEXF_NONE);
    if (m_enabled && !m_composited && m_sceneDraws >= m_minSceneDraws) {
        m_composited = true;
        if (Composite(m_sceneRT, m_sceneDS)) ++m_statsComposited;
    }
    if (++m_statsFrames % 600 == 0 && m_enabled && m_header) {
        m_host->Log(LogLevel::Info, "RaytracingBridge: left out of the one material: no normal %llu, other blend %llu, masks %llu, no depth %llu, no shader %llu",
                    m_litSkip[0], m_litSkip[1], m_litSkip[2], m_litSkip[3], m_litSkip[4]);
        if (m_ownCull)
            m_host->Log(LogLevel::Info, "RaytracingBridge: culling: game frustum off, %llu draws off the view not rasterized, %llu drawn (600 frames)",
                        m_cullSkipped, m_cullKept);
        pools::Report([this](const std::string& line) { m_host->Log(LogLevel::Info, "RaytracingBridge: %s", line.c_str()); });
        if (gamelights::Active()) {
            const gamelights::Stats gs = gamelights::LastStats();
            m_host->Log(LogLevel::Info, "RaytracingBridge: game lights lit this frame: %u coronas (%u of objects), %u point lights -> %u lamps, %u traced (hour %u)",
                        gs.coronas, gs.objectCoronas, gs.pointLights, gs.lamps, m_lightsSent, (ReadGameLightState() >> 8) & 0xff);
        }
        m_cullSkipped = m_cullKept = 0;
        m_host->Log(LogLevel::Info,
                    "RaytracingBridge: %llu/%llu frames ray traced | not traced: camera %llu, bridge %llu, generation %llu, resources %llu, slot %llu, hand-off %llu, late %llu, write %llu | ended by a copy %llu, by a depth clear %llu, 3D draws after composition %llu, matrices without a camera %llu | hidden by the game in view (LOD) %llu, mirrored draws %llu | game lights %zu known, %u traced | %llu G-buffer draws + %llu blended (%llu shaders patched, %llu rejected) | %u meshes, %u instances | %.2f ms GPU (bridge)",
                    m_statsComposited, m_statsFrames, m_fail[kFailCamera], m_fail[kFailBridge], m_fail[kFailGeneration], m_fail[kFailResources], m_fail[kFailSlot],
                    m_fail[kFailHandoff], m_fail[kFailLate], m_fail[kFailStretch], m_copyComposites, m_clearComposites, m_lateScene, m_rejectedMatrices, m_lodDropped, m_mirroredDraws, m_worldLights.size(), m_lightsSent, m_statsGBufferDraws, m_statsBlendedGBuffer, m_patchedOk, m_patchedFail, m_header->bridgeMeshCount,
                    m_header->bridgeLastInstanceCount, m_header->bridgeGpuMs);
    }
    if (m_statsFrames % 600 == 0 && m_enabled) {
        char buf[1024];
        size_t off = static_cast<size_t>(snprintf(buf, sizeof(buf), "captured %llu |", m_captured));
        for (int i = 0; i < kSkipCount && off < sizeof(buf); ++i)
            if (m_skip[i]) off += static_cast<size_t>(snprintf(buf + off, sizeof(buf) - off, " %s=%llu", kSkipNames[i], m_skip[i]));
        m_host->Log(LogLevel::Info, "RaytracingBridge: capture over 600 frames: %s | characters last frame %llu, textures %u | world holds %zu meshes",
                    buf, m_dynDrawsLast, m_nextTexId - 1, m_meshes.size());
        if (m_sunMax[0] > -9) {
            m_host->Log(LogLevel::Info, "RaytracingBridge: sun over 600 frames: now (%.3f %.3f %.3f), raw range x[%.3f %.3f] y[%.3f %.3f] z[%.3f %.3f]; camera (%.1f %.1f %.1f); colour sun (%.2f %.2f %.2f) sky (%.2f %.2f %.2f)",
                        m_sun[0], m_sun[1], m_sun[2], m_sunMin[0], m_sunMax[0], m_sunMin[1], m_sunMax[1], m_sunMin[2], m_sunMax[2],
                        m_camera[0], m_camera[1], m_camera[2], m_smoothSunColor[0], m_smoothSunColor[1], m_smoothSunColor[2],
                        m_smoothSkyColor[0], m_smoothSkyColor[1], m_smoothSkyColor[2]);
            for (int i = 0; i < 3; ++i) { m_sunMin[i] = 9; m_sunMax[i] = -9; }
        }
        memset(m_skip, 0, sizeof(m_skip));
        m_captured = 0;
    }
    {
        if (m_capRunning) CaptureFrameEnd(frame);
        const bool down = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_MENU) & 0x8000) && (GetAsyncKeyState(VK_F12) & 0x8000);
        static const char* const atEnv = getenv("BULLYRT_FRAME_AT");
        const bool atFrame = atEnv && frame.frameIndex == static_cast<uint64_t>(_atoi64(atEnv));
        if (m_capRequest.empty()) {
            wchar_t exe[MAX_PATH] = {};
            GetModuleFileNameW(nullptr, exe, MAX_PATH);
            m_capRequest = exe;
            m_capRequest = m_capRequest.substr(0, m_capRequest.find_last_of(L"\\/") + 1) + L"BullyDX12\\bridge\\frame.request";
        }
        const bool requested = frame.frameIndex % 8 == 0 && GetFileAttributesW(m_capRequest.c_str()) != INVALID_FILE_ATTRIBUTES;
        if (requested) DeleteFileW(m_capRequest.c_str());
        if (((down && !m_capKeyDown) || atFrame || requested) && !m_capRunning) CaptureFrameBegin(frame.frameIndex + 1);
        m_capKeyDown = down;
    }
    EndFrame();
}

}

BULLYDX12_DECLARE_EXTENSION(RaytracingBridge)
