// NVIDIA RTXDI (ReSTIR DI): which light each pixel uses. The light sources are the lamps (LightPos) and the windows (Portal*), in one list:
// index < LightCount is a lamp, the rest are windows. The sun stays apart: it has its own ray. Only for the first surface a pixel sees;
// the bounces go on using SampleDirect, because SHaRC averages them.

#ifndef RTXDI_BRIDGE_HLSLI
#define RTXDI_BRIDGE_HLSLI

#define RTXDI_ENABLE_PRESAMPLING 0
#define RTXDI_REGIR_MODE RTXDI_REGIR_DISABLED

struct RAB_Surface {
    float3 pos;
    float  viewDepth;
    float3 normal;
    uint   valid;
};
struct RAB_Material { uint unused; };
struct RAB_LightInfo { int index; };
struct RAB_LightSample {
    float3 position;
    float3 radiance;
    float3 factor;
    float  stopShort;
};

RWStructuredBuffer<RAB_Surface> RestirSurfaces : register(u21);

uint RestirSurfaceIndex(int2 pixel, uint frameArray) { return (frameArray * RtSize.y + (uint)pixel.y) * RtSize.x + (uint)pixel.x; }
uint RestirCurrentArray() { return asuint(ProjParams.w) & 1u; }
int  RestirLampCount() { return min((int)LightCount, 32); }
int  RestirWindowCount() { return asfloat(Expo.Load(52)) > 0.5 ? min((int)PortalInfo.x, 128) : 0; }
static const int kRestirWindowBase = 32;
bool RestirLightExists(int index)
{
    return index >= 0 && (index < kRestirWindowBase ? index < RestirLampCount() : index - kRestirWindowBase < RestirWindowCount());
}
int RestirMapped(uint index, bool currentToPrevious) { return LightNumberMapped(index, currentToPrevious); }

RAB_Surface RAB_EmptySurface() { RAB_Surface s = (RAB_Surface)0; return s; }
RAB_LightSample RAB_EmptyLightSample() { RAB_LightSample s = (RAB_LightSample)0; return s; }
RAB_LightInfo RAB_EmptyLightInfo() { RAB_LightInfo l; l.index = -1; return l; }
bool   RAB_IsSurfaceValid(RAB_Surface s) { return s.valid != 0u; }
float3 RAB_GetSurfaceWorldPos(RAB_Surface s) { return s.pos; }
float3 RAB_GetSurfaceNormal(RAB_Surface s) { return s.normal; }
float  RAB_GetSurfaceLinearDepth(RAB_Surface s) { return s.viewDepth; }
RAB_Material RAB_GetMaterial(RAB_Surface s) { RAB_Material m; m.unused = 0u; return m; }
bool   RAB_AreMaterialsSimilar(RAB_Material a, RAB_Material b) { return true; }
int2   RAB_ClampSamplePositionIntoView(int2 p, bool previousFrame) { return clamp(p, int2(0, 0), int2(RtSize) - 1); }

RAB_Surface RestirSurfaceNow(int2 pixel)
{
    RAB_Surface s = RAB_EmptySurface();
    const uint2 src = RtSrc(uint2(pixel));
    const float d = Depth[src];
    if (d >= 0.99999 || d <= 0.0) return s;
    const float mat = GAlbedo[src].a;
    if (mat > 0.75 && mat < 0.9) return s;
    const float3 P = WorldFromDepth(src, d);
    const float4 g = GNormal[src];
    const float3 gN = g.xyz * 2.0 - 1.0;
    float3 N;
    if ((g.w > 0.0 || any(g.xyz > 0.0)) && dot(gN, gN) > 0.05) N = normalize(gN);
    else {
        const uint2 r = uint2(min(src.x + 1, Size.x - 1), src.y), u = uint2(src.x, min(src.y + 1, Size.y - 1));
        const float3 c = cross(WorldFromDepth(u, Depth[u]) - P, WorldFromDepth(r, Depth[r]) - P);
        N = dot(c, c) > 1e-12 ? normalize(c) : normalize(CameraPos.xyz - P);
    }
    s.normal = TrustedNormal(N, src, P, normalize(CameraPos.xyz - P));
    s.pos = P + s.normal * 0.05;
    s.viewDepth = abs(ViewZFromDepth(d));
    s.valid = 1u;
    return s;
}

RAB_Surface RAB_GetGBufferSurface(int2 pixel, bool previousFrame)
{
    if (any(pixel < 0) || any(pixel >= int2(RtSize))) return RAB_EmptySurface();
    if (previousFrame) return RestirSurfaces[RestirSurfaceIndex(pixel, 1u - RestirCurrentArray())];
    return RestirSurfaceNow(pixel);
}

RAB_LightInfo RAB_LoadLightInfo(uint index, bool previousFrame)
{
    RAB_LightInfo l = RAB_EmptyLightInfo();
    const int now = previousFrame ? RestirMapped(index, false) : (int)index;
    if (RestirLightExists(now)) l.index = now;
    return l;
}
int RAB_TranslateLightIndex(uint index, bool currentToPrevious) { return RestirMapped(index, currentToPrevious); }

RAB_LightSample RAB_SamplePolymorphicLight(RAB_LightInfo info, RAB_Surface surface, float2 uv)
{
    RAB_LightSample s = RAB_EmptyLightSample();
    if (!RestirLightExists(info.index)) return s;
    if (info.index < kRestirWindowBase) {
        float3 l; float dist;
        const float k = LightWeight(info.index, surface.pos, surface.normal, l, dist);
        if (k <= 0.0) return s;
        const float z = uv.x * 2.0 - 1.0, phi = uv.y * 6.2831853, ring = sqrt(saturate(1.0 - z * z));
        s.position = LightPos[info.index].xyz + float3(ring * cos(phi), ring * sin(phi), z) * kLampRadius;
        s.radiance = LightCol[info.index].rgb * k;
        s.factor = -1.0;
        s.stopShort = 0.6;
        return s;
    }
    const int w = info.index - kRestirWindowBase;
    const float3 x = PortalC[w].xyz + PortalU[w].xyz * (uv.x * 2.0 - 1.0) + PortalV[w].xyz * (uv.y * 2.0 - 1.0);
    const float3 d = x - surface.pos;
    const float  d2 = max(dot(d, d), 0.25);
    const float  inv = rcp(sqrt(d2));
    const float  cs = dot(surface.normal, d) * inv;
    if (cs <= 0.0) return s;
    const float cp = abs(dot(d, PortalN[w].xyz)) * inv;
    const float amount = min(PortalInfo.y * cs * cp * PortalN[w].w / (3.14159265 * d2), 8.0 * PortalInfo.y / 80.0);
    s.position = x;
    s.factor = float3(PortalU[w].w, PortalV[w].w, PortalC[w].w) * amount;
    s.radiance = s.factor * SkyColor.rgb * 0.8;
    s.stopShort = 0.05;
    return s;
}

float RAB_GetLightSampleTargetPdfForSurface(RAB_LightSample s, RAB_Surface surface) { return max(dot(s.radiance, float3(0.3, 0.6, 0.1)), 0.0); }
void  RAB_GetLightDirDistance(RAB_Surface surface, RAB_LightSample s, out float3 dir, out float dist)
{
    const float3 to = s.position - surface.pos;
    dist = max(length(to), 1e-3);
    dir = to / dist;
}
float RAB_LightSampleSolidAnglePdf(RAB_LightSample s) { return 1.0; }
bool  RAB_IsAnalyticLightSample(RAB_LightSample s) { return true; }
float RAB_SurfaceEvaluateBrdfPdf(RAB_Surface surface, float3 dir) { return max(dot(surface.normal, dir), 0.0) / 3.14159265; }
bool  RAB_GetConservativeVisibility(RAB_Surface surface, RAB_LightSample s)
{
    float3 dir; float dist;
    RAB_GetLightDirDistance(surface, s, dir, dist);
    return TraceFromFloor(surface.pos, dir, max(dist - s.stopShort, 0.01), surface.normal) >= NRD_FP16_MAX;
}
bool  RAB_GetTemporalConservativeVisibility(RAB_Surface current, RAB_Surface previous, RAB_LightSample s) { return RAB_GetConservativeVisibility(current, s); }

float RAB_EvaluateLocalLightSourcePdf(uint index) { return 1.0; }
float RAB_EvaluateEnvironmentMapSamplingPdf(float3 dir) { return 0.0; }
float2 RAB_GetEnvironmentMapRandXYFromDir(float3 dir) { return float2(0, 0); }

#include <Rtxdi/Utils/RandomSamplerState.hlsli>
bool RAB_SurfaceImportanceSampleBrdf(RAB_Surface surface, inout RTXDI_RandomSamplerState rng, out float3 dir) { dir = surface.normal; return false; }
bool RAB_TraceRayForLocalLight(float3 origin, float3 dir, float tMin, float tMax, out uint lightIndex, out float2 uv) { lightIndex = 0; uv = float2(0, 0); return false; }

#include <Rtxdi/DI/Reservoir.hlsli>
RWStructuredBuffer<RTXDI_PackedDIReservoir> RestirReservoirs : register(u20);
#define RTXDI_LIGHT_RESERVOIR_BUFFER RestirReservoirs
#include "RtxdiNeighbors.hlsli"
#define RTXDI_NEIGHBOR_OFFSETS_BUFFER gRtxdiNeighbors
#include <Rtxdi/DI/ReservoirStorage.hlsli>
#include <Rtxdi/DI/InitialSampling.hlsli>
#include <Rtxdi/DI/SpatioTemporalResampling.hlsli>

float RestirSourceWeight(int i, int lamps, RAB_Surface surface)
{
    if (i < lamps) {
        float3 l; float dist;
        return LightWeight(i, surface.pos, surface.normal, l, dist) * dot(LightCol[i].rgb, float3(0.3, 0.6, 0.1)) * (0.03 + LightVisibility(i));
    }
    const int j = i - lamps;
    const float3 d  = PortalC[j].xyz - surface.pos;
    const float  d2 = max(dot(d, d), 0.25);
    const float  inv = rcp(sqrt(d2));
    const float  cs = dot(surface.normal, d) * inv;
    return cs < -0.3 ? 0.0 : PortalN[j].w * max(cs, 0.05) * max(abs(dot(d, PortalN[j].xyz)) * inv, 0.05) / d2 * (0.05 + PortalVisibility(j));
}

RTXDI_DIReservoir RestirInitial(inout RTXDI_RandomSamplerState rng, RAB_Surface surface, uint candidates, out RAB_LightSample chosen)
{
    chosen = RAB_EmptyLightSample();
    RTXDI_DIReservoir state = RTXDI_EmptyDIReservoir();
    const int lamps = RestirLampCount(), n = lamps + RestirWindowCount();
    float lampTotal = 0, windowTotal = 0;
    for (int i = 0; i < n; ++i) {
        const float w = RestirSourceWeight(i, lamps, surface);
        if (i < lamps) lampTotal += w; else windowTotal += w;
    }
    if (lampTotal <= 1e-6 && windowTotal <= 1e-6) return state;
    const float lampShare = lampTotal <= 1e-6 ? 0.0 : (windowTotal <= 1e-6 ? 1.0 : 0.5);
    const float lampScale = lampShare / max(lampTotal, 1e-6), windowScale = (1.0 - lampShare) / max(windowTotal, 1e-6);
    const float jitter = RTXDI_GetNextRandom(rng);
    float run = 0;
    uint c = 0;
    for (int k = 0; k < n && c < candidates; ++k) {
        const float pdf = RestirSourceWeight(k, lamps, surface) * (k < lamps ? lampScale : windowScale);
        run += pdf;
        [loop] while (c < candidates && ((float)c + jitter) / (float)candidates < run) {
            ++c;
            if (pdf <= 0.0) continue;
            const float2 uv = float2(RTXDI_GetNextRandom(rng), RTXDI_GetNextRandom(rng));
            RAB_LightInfo info; info.index = k < lamps ? k : kRestirWindowBase + (k - lamps);
            const RAB_LightSample s = RAB_SamplePolymorphicLight(info, surface, uv);
            const float target = RAB_GetLightSampleTargetPdfForSurface(s, surface);
            if (target > 0.0 && RTXDI_StreamSample(state, (uint)info.index, uv, RTXDI_GetNextRandom(rng), target, 1.0 / pdf)) chosen = s;
        }
    }
    RTXDI_FinalizeResampling(state, 1.0, (float)candidates);
    state.M = 1;
    return state;
}

float3 RestirDirect(uint2 px, float3 origin, float3 Ng, float3 wasAt)
{
    const uint frame = asuint(ProjParams.w);
    const uint current = RestirCurrentArray();
    RAB_Surface surface = RestirSurfaceNow(int2(px));
    RestirSurfaces[RestirSurfaceIndex(int2(px), current)] = surface;
    if (!RAB_IsSurfaceValid(surface)) return 0;
    const uint lights = (uint)(RestirLampCount() + RestirWindowCount());
    RTXDI_ReservoirBufferParameters place = (RTXDI_ReservoirBufferParameters)0;
    place.reservoirBlockRowPitch = (uint)RestirInfo.x;
    place.reservoirArrayPitch = (uint)RestirInfo.y;
    if (lights == 0u) {
        RTXDI_StoreDIReservoir(RTXDI_EmptyDIReservoir(), place, px, current);
        return 0;
    }
    RTXDI_RandomSamplerState rng = RTXDI_InitRandomSampler(px, frame, RTXDI_DI_GENERATE_INITIAL_SAMPLES_RANDOM_SEED);
    RAB_LightSample chosen;
    RTXDI_DIReservoir r = RestirInitial(rng, surface, max((uint)RestirInfo.w, 1u), chosen);
    if (RestirInfo.z > 1.5 && RTXDI_IsValidDIReservoir(r) && !RAB_GetConservativeVisibility(surface, chosen))
        RTXDI_StoreVisibilityInDIReservoir(r, 0, true);

    RTXDI_RuntimeParameters runtime = (RTXDI_RuntimeParameters)0;
    runtime.neighborOffsetMask = 255u;
    runtime.frameIndex = frame;
    RTXDI_DISpatioTemporalResamplingParameters reuse = (RTXDI_DISpatioTemporalResamplingParameters)0;
    reuse.depthThreshold = 0.1;
    reuse.normalThreshold = 0.5;
    reuse.biasCorrectionMode = RestirInfo.z > 2.5 ? RTXDI_BIAS_CORRECTION_RAY_TRACED : RTXDI_BIAS_CORRECTION_BASIC;
    reuse.maxHistoryLength = (uint)RestirTune.x;
    reuse.enablePermutationSampling = RestirTune.w > 0.5 ? 1u : 0u;
    reuse.uniformRandomNumber = frame * 2654435761u;
    reuse.numSamples = (uint)RestirTune.z;
    reuse.numDisocclusionBoostSamples = max((uint)RestirTune.z, 4u);
    reuse.samplingRadius = RestirTune.y;
    const float4 was = mul(PrevViewProj, float4(wasAt, 1));
    const float3 motion = float3(OutMotion[px] * float2(RtSize), was.w - surface.viewDepth);
    int2 fromPixel = int2(px);
    r = RTXDI_DISpatioTemporalResampling(int2(px), surface, r, rng, motion, 1u - current, runtime, place, reuse, fromPixel, chosen);

    float3 direct = 0;
    if (RTXDI_IsValidDIReservoir(r)) {
        const RAB_LightSample s = RAB_SamplePolymorphicLight(RAB_LoadLightInfo(RTXDI_GetDIReservoirLightIndex(r), false), surface, RTXDI_GetDIReservoirSampleUV(r));
        const float3 to = s.position - origin;
        const float  dist = max(length(to), 1e-3);
        const float3 dir = to / dist;
        const bool free = dot(dir, Ng) > 0.0 && (!OPT_LOCAL || TraceFromFloor(origin, dir, max(dist - s.stopShort, 0.01), Ng) >= NRD_FP16_MAX);
        if (free) {
            const float3 light = s.factor.x >= 0.0 ? s.factor * Sky(dir) : s.radiance;
            direct = light * RTXDI_GetDIReservoirInvPdf(r);
        } else {
            RTXDI_StoreVisibilityInDIReservoir(r, float3(0, 0, 0), RestirTune.w < 1.5);
        }
    }
    RTXDI_StoreDIReservoir(r, place, px, current);
    return direct;
}

#endif
