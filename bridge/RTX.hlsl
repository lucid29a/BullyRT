// BullyRTBridge - hybrid RTX lighting (DXR 1.1 inline ray queries + NVIDIA NRD).

#include "NRD.hlsli"

RaytracingAccelerationStructure Scene : register(t0);
Texture2D<float>  Depth     : register(t1);
Texture2D<float4> GNormal   : register(t2);
Texture2D<float4> GAlbedo   : register(t3);
Texture2D<float4> SceneColor : register(t4);
Texture2D<float>  SigmaOut  : register(t5);
Texture2D<float4> DiffOut   : register(t6);
Texture2D<float4> SpecOut   : register(t7);

struct GeomRecord {
    uint  attr;
    uint  posOffset;
    uint  uvOffset;
    uint  idxOffset;
    int   tex;
    uint  flags;
    float alphaRef;
    uint  colorOffset;
    uint  prevAttr;
    uint  prevPosOffset;
    uint  moved;
    uint  pad0;
    float4 prevT[3];
};
StructuredBuffer<GeomRecord> Records : register(t8);
Texture2D<float4> GOpaque : register(t9);
Texture2D<float4> Tex[]  : register(t0, space1);
ByteAddressBuffer Attr[] : register(t0, space2);
SamplerState Smp : register(s0);

RWTexture2D<float>  OutViewZ   : register(u0);
RWTexture2D<float4> OutNormal  : register(u1);
RWTexture2D<float>  OutPenumbra: register(u2);
RWTexture2D<float4> OutDiff    : register(u3);
RWTexture2D<float4> OutSpec    : register(u4);
RWTexture2D<float4> OutFinal   : register(u5);
RWTexture2D<float2> OutMotion  : register(u6);
RWTexture3D<float4> AirGrid    : register(u14);
RWTexture3D<float4> AirLitA    : register(u15);
RWTexture3D<float4> AirLitB    : register(u16);
RWTexture3D<float4> AirLocalA  : register(u22);
RWTexture3D<float4> AirLocalB  : register(u23);
RWTexture3D<float4> AirTrans   : register(u24);
RWTexture3D<uint4>  VolResA    : register(u25);
RWTexture3D<uint4>  VolResB    : register(u26);

#define SHARC_RADIANCE_SCALE 1e3f
#include "SharcCommon.h"
RWStructuredBuffer<HashGridKey>           SharcHash     : register(u17);
RWStructuredBuffer<SharcAccumulationData> SharcAccum    : register(u18);
RWStructuredBuffer<SharcPackedData>       SharcResolved : register(u19);
static const uint kSharcEntries = 1u << 21;
RWTexture2D<float4> OutRRColor  : register(u9);
RWTexture2D<float4> OutRRDiff   : register(u10);
RWTexture2D<float4> OutRRSpec   : register(u11);
RWTexture2D<float4> OutRRNormal : register(u12);
RWTexture2D<float2> OutRRMv     : register(u13);
Texture2D<float4>   RROut       : register(t10);
RWByteAddressBuffer Expo       : register(u8);

cbuffer Frame : register(b0)
{
    row_major float4x4 ViewProj;
    row_major float4x4 InvViewProj;
    row_major float4x4 PrevViewProj;
    float4 CameraPos;
    float4 SunDir;
    float4 SunColor;
    float4 SkyColor;
    float4 ProjParams;
    float4 Strength;
    uint2  Size;
    float  HitBaked;
    float  LightCount;
    float4 Atmosphere;
    float4 TrueSun;
    float4 Clouds;
    float4 LightPos[32];
    float4 LightCol[32];
    float4 LightDir[32];
    float4 LightAtt[32];
    uint   DebugMode;
    uint   FullRT;
    uint2  Pad2;
    float4 Exposure;
    float4 WindowLight;
    uint2  RtSize;
    uint   RtScale;
    uint   RtOptions;
    float4 Osd;
    uint4  OsdText[8];
    float4 Fog;
    float4 Bounce;
    float4 WeatherTop;
    float4 WeatherBot;
    float4 PrevCam;
    float4 PortalInfo;
    float4 PortalC[128];
    float4 PortalU[128];
    float4 PortalV[128];
    float4 PortalN[128];
    float4 SharcInfo;
    float4 SharcInfo2;
    float4 MovedMin[8];
    float4 MovedMax[8];
    float4 RestirInfo;
    float4 RestirTune;
    uint4  RestirMap[20];
    float4 AoInfo;
    float4 VolInfo;
    uint4  RtJitter;
    float4 RoomAmbient;
    float4 FixInfo;
    float4 FixGrid;
    float4 FixPos[768];
    float4 FixCol[768];
    uint4  FixCells[256];
    uint4  FixList[1024];
};

#define OPT_REFLECTIONS ((RtOptions & 1u) != 0)
#define OPT_SUN         ((RtOptions & 2u) != 0)
#define OPT_LOCAL       ((RtOptions & 4u) != 0)
#define OPT_LIGHTING    ((RtOptions >> 3) & 3u)
#define OPT_RT          ((RtOptions & 32u) != 0)
#define OPT_VOLUMETRIC  ((RtOptions & 64u) != 0)
#define RR_ACTIVE       (Pad2.x != 0u)
static const uint2 kFont[64] = {uint2(0x00000000u, 0x0u), uint2(0x00000000u, 0x0u), uint2(0x00000000u, 0x0u), uint2(0x00000000u, 0x0u), uint2(0x00000000u, 0x0u), uint2(0x00000000u, 0x0u), uint2(0x00000000u, 0x0u), uint2(0x00000000u, 0x0u), uint2(0x08210888u, 0x2u), uint2(0x88842082u, 0x0u), uint2(0x00000000u, 0x0u), uint2(0x084f9080u, 0x0u), uint2(0x00000000u, 0x0u), uint2(0x000f8000u, 0x0u), uint2(0x8c000000u, 0x1u), uint2(0x02222200u, 0x0u), uint2(0xa33ae62eu, 0x3u), uint2(0x884210c4u, 0x3u), uint2(0xc444422eu, 0x7u), uint2(0xa304111fu, 0x3u), uint2(0x11f4a988u, 0x2u), uint2(0xa3083c3fu, 0x3u), uint2(0xa317844cu, 0x3u), uint2(0x8422221fu, 0x0u), uint2(0xa317462eu, 0x3u), uint2(0x910f462eu, 0x1u), uint2(0x0c6018c0u, 0x0u), uint2(0x00000000u, 0x0u), uint2(0x00000000u, 0x0u), uint2(0x01f07c00u, 0x0u), uint2(0x00000000u, 0x0u), uint2(0x00000000u, 0x0u), uint2(0x00000000u, 0x0u), uint2(0x631fc62eu, 0x4u), uint2(0xe317c62fu, 0x3u), uint2(0xa210862eu, 0x3u), uint2(0xd318c527u, 0x1u), uint2(0xc217843fu, 0x7u), uint2(0x4217843fu, 0x0u), uint2(0xa31e862eu, 0x7u), uint2(0x631fc631u, 0x4u), uint2(0x8842108eu, 0x3u), uint2(0x9284211cu, 0x1u), uint2(0x52519531u, 0x4u), uint2(0xc2108421u, 0x7u), uint2(0x631ad771u, 0x4u), uint2(0x639ace31u, 0x4u), uint2(0xa318c62eu, 0x3u), uint2(0x4217c62fu, 0x0u), uint2(0x9358c62eu, 0x5u), uint2(0x5257c62fu, 0x4u), uint2(0xe107043eu, 0x3u), uint2(0x0842109fu, 0x1u), uint2(0xa318c631u, 0x3u), uint2(0x1518c631u, 0x1u), uint2(0xab5ac631u, 0x2u), uint2(0x62a22a31u, 0x4u), uint2(0x08422a31u, 0x1u), uint2(0xc222221fu, 0x7u), uint2(0x00000000u, 0x0u), uint2(0x00000000u, 0x0u), uint2(0x00000000u, 0x0u), uint2(0x00000000u, 0x0u), uint2(0x00000000u, 0x0u)};
static float gRayMin = 0.0;
static float3 gDebug9 = 0;
static bool  gGlassPortal = true;
static uint  gRayMask = 0xFF;

static const float3 HitDistParams = float3(3.0, 0.1, 20.0);

uint Hash(uint x) { x ^= x >> 16; x *= 0x7feb352d; x ^= x >> 15; x *= 0x846ca68b; x ^= x >> 16; return x; }
float Rand(inout uint s) { s = Hash(s); return (s & 0xFFFFFF) / 16777216.0; }
bool IsMirror(float a) { return a > 0.394 && a < 0.398; }
uint2 RtSrc(uint2 lp) { return min(lp * RtScale + RtJitter.xy, Size - 1u); }

float3 WorldFromDepth(float2 pixel, float d)
{
    float2 uv  = pixel / float2(Size);
    float4 w   = mul(InvViewProj, float4(uv.x * 2 - 1, 1 - uv.y * 2, d, 1));
    return w.xyz / w.w;
}

float ViewZFromDepth(float d)
{
    const float den = d - ProjParams.x;
    return ProjParams.y / (abs(den) < 1e-7 ? -1e-7 : den);
}

float3 DepthNormal(uint2 px, float3 P, float3 V)
{
    const uint2 r = uint2(min(px.x + 1, Size.x - 1), px.y), l = uint2(px.x > 0 ? px.x - 1 : 0, px.y);
    const uint2 u = uint2(px.x, min(px.y + 1, Size.y - 1)), b = uint2(px.x, px.y > 0 ? px.y - 1 : 0);
    const float3 pr = WorldFromDepth(r, Depth[r]) - P, pl = P - WorldFromDepth(l, Depth[l]);
    const float3 pu = WorldFromDepth(u, Depth[u]) - P, pb = P - WorldFromDepth(b, Depth[b]);
    const float3 dx = dot(pr, pr) < dot(pl, pl) ? pr : pl;
    const float3 dy = dot(pu, pu) < dot(pb, pb) ? pu : pb;
    float3 n = cross(dy, dx);
    if (dot(n, n) < 1e-12) return V;
    n = normalize(n);
    return dot(n, V) < 0 ? -n : n;
}

float3 CornerNormal(uint2 px, float3 P, float3 V, float3 ref, out float best)
{
    const uint2 r = uint2(min(px.x + 1, Size.x - 1), px.y), l = uint2(px.x > 0 ? px.x - 1 : 0, px.y);
    const uint2 u = uint2(px.x, min(px.y + 1, Size.y - 1)), b = uint2(px.x, px.y > 0 ? px.y - 1 : 0);
    const float3 pr = WorldFromDepth(r, Depth[r]) - P, pl = P - WorldFromDepth(l, Depth[l]);
    const float3 pu = WorldFromDepth(u, Depth[u]) - P, pb = P - WorldFromDepth(b, Depth[b]);
    const float3 sx[2] = {pr, pl};
    const float3 sy[2] = {pu, pb};
    float3 res = V;
    best = -2.0;
    for (int i = 0; i < 2; ++i)
        for (int j = 0; j < 2; ++j) {
            float3 n = cross(sy[j], sx[i]);
            if (dot(n, n) < 1e-12) continue;
            n = normalize(n);
            if (dot(n, V) < 0) n = -n;
            const float m = dot(n, ref);
            if (m > best) { best = m; res = n; }
        }
    return res;
}

float3 TrustedNormal(float3 gN, uint2 px, float3 P, float3 V)
{
    const float3 nd = DepthNormal(px, P, V);
    if (dot(gN, gN) < 0.05) return nd;
    float3 n = normalize(gN);
    if (dot(n, nd) < 0) n = -n;
    float best;
    CornerNormal(px, P, V, n, best);
    return best < 0.35 ? nd : n;
}

void Basis(float3 n, out float3 t, out float3 b)
{
    t = normalize(abs(n.z) < 0.99 ? cross(n, float3(0, 0, 1)) : cross(n, float3(1, 0, 0)));
    b = cross(n, t);
}

float3 UnpackColor(uint c) { return float3((c >> 16) & 255, (c >> 8) & 255, c & 255) / 255.0; }

float2 HitUv(GeomRecord g, uint prim, float2 bary)
{
    const ByteAddressBuffer buf = Attr[NonUniformResourceIndex(g.attr)];
    const uint3 i = buf.Load3(g.idxOffset + prim * 12);
    const float2 a = asfloat(buf.Load2(g.uvOffset + i.x * 8));
    const float2 b = asfloat(buf.Load2(g.uvOffset + i.y * 8));
    const float2 c = asfloat(buf.Load2(g.uvOffset + i.z * 8));
    return a + (b - a) * bary.x + (c - a) * bary.y;
}

bool IsGlassPane(uint record, uint prim, float3x4 o2w)
{
    const GeomRecord g = Records[record];
    if (!(g.flags & 8u)) return false;
    const ByteAddressBuffer gb = Attr[NonUniformResourceIndex(g.attr)];
    const uint3  gi = gb.Load3(g.idxOffset + prim * 12);
    const float3 p0 = asfloat(gb.Load3(g.posOffset + gi.x * 12));
    const float3 n  = normalize(cross(mul((float3x3)o2w, asfloat(gb.Load3(g.posOffset + gi.y * 12)) - p0),
                                      mul((float3x3)o2w, asfloat(gb.Load3(g.posOffset + gi.z * 12)) - p0)));
    return abs(n.z) < 0.6;
}

static const float kGlassBacking = 0.35;

bool AlphaBlocks(uint record, uint prim, float2 bary, float3x4 o2w)
{
    const GeomRecord g = Records[record];
    if (g.alphaRef > 0.85) {
        const ByteAddressBuffer fb = Attr[NonUniformResourceIndex(g.attr)];
        const uint3  fi = fb.Load3(g.idxOffset + prim * 12);
        const float3 f0 = asfloat(fb.Load3(g.posOffset + fi.x * 12));
        const float3 fn = cross(mul((float3x3)o2w, asfloat(fb.Load3(g.posOffset + fi.y * 12)) - f0), mul((float3x3)o2w, asfloat(fb.Load3(g.posOffset + fi.z * 12)) - f0));
        if (abs(fn.z) > 0.9 * length(fn)) return true;
    }
    if (g.flags & 8u) {
        if (IsGlassPane(record, prim, o2w)) return false;
        if (!(g.flags & 1u)) return true;
    }
    const ByteAddressBuffer buf = Attr[NonUniformResourceIndex(g.attr)];
    const uint3 i = buf.Load3(g.idxOffset + prim * 12);
    const float a0 = (buf.Load(g.colorOffset + i.x * 4) >> 24) / 255.0;
    const float a1 = (buf.Load(g.colorOffset + i.y * 4) >> 24) / 255.0;
    const float a2 = (buf.Load(g.colorOffset + i.z * 4) >> 24) / 255.0;
    float alpha = a0 + (a1 - a0) * bary.x + (a2 - a0) * bary.y;
    if (g.tex >= 0) alpha *= Tex[NonUniformResourceIndex(g.tex)].SampleLevel(Smp, HitUv(g, prim, bary), 0).a;
    return alpha >= g.alphaRef;
}

struct Hit {
    float  t;
    float3 N;
    float3 albedo;
    float3 baked;
    float  emissive;
    bool   glass;
    bool   mirror;
};

Hit TraceFaces(float3 o, float3 d, float tmax, uint rayFlags)
{
    float start = 0;
    Hit h;
    h.t = NRD_FP16_MAX; h.N = -d; h.albedo = 0.4; h.baked = 0.0; h.emissive = 0.0; h.glass = false; h.mirror = false;
    [loop] for (int pass = 0; pass < 3; ++pass) {
    RayDesc r;
    r.Origin = o + d * start; r.Direction = d; r.TMin = pass == 0 ? gRayMin : 0.0; r.TMax = max(tmax - start, r.TMin + 1e-3);
    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
    q.TraceRayInline(Scene, rayFlags, gRayMask, r);
    float glassT = 1e30;
    while (q.Proceed())
        if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE) {
            const uint rec = q.CandidateInstanceID() + q.CandidateGeometryIndex();
            if (IsGlassPane(rec, q.CandidatePrimitiveIndex(), q.CandidateObjectToWorld3x4()))
                glassT = min(glassT, q.CandidateTriangleRayT());
            else if (AlphaBlocks(rec, q.CandidatePrimitiveIndex(), q.CandidateTriangleBarycentrics(), q.CandidateObjectToWorld3x4()))
                q.CommitNonOpaqueTriangleHit();
        }
    if (glassT < 1e29 && (q.CommittedStatus() != COMMITTED_TRIANGLE_HIT || q.CommittedRayT() > glassT - 0.01)) {
        h.glass = true;
        if (gGlassPortal && asfloat(Expo.Load(52)) > 0.5) return h;
    }
    if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT && q.CommittedRayT() > glassT - 0.01 && q.CommittedRayT() < glassT + kGlassBacking && pass < 2) {
        start += q.CommittedRayT() + 0.01;
        continue;
    }
    if (q.CommittedStatus() != COMMITTED_TRIANGLE_HIT) return h;
    h.t = start + q.CommittedRayT();

    const GeomRecord g = Records[q.CommittedInstanceID() + q.CommittedGeometryIndex()];
    h.emissive = ((g.flags >> 8) & 255u) / 8.0;
    h.mirror = (g.flags & 32u) != 0u;
    const ByteAddressBuffer buf = Attr[NonUniformResourceIndex(g.attr)];
    const uint3  i  = buf.Load3(g.idxOffset + q.CommittedPrimitiveIndex() * 12);
    const float3 p0 = asfloat(buf.Load3(g.posOffset + i.x * 12));
    const float3 p1 = asfloat(buf.Load3(g.posOffset + i.y * 12));
    const float3 p2 = asfloat(buf.Load3(g.posOffset + i.z * 12));
    const float3x4 m = q.CommittedObjectToWorld3x4();
    float3 n = cross(mul((float3x3)m, p1 - p0), mul((float3x3)m, p2 - p0));
    const float l = length(n);
    if (l > 1e-8) {
        n /= l;
        h.N = dot(n, d) > 0 ? -n : n;
    }
    const float2 bc = q.CommittedTriangleBarycentrics();
    const float3 c0 = UnpackColor(buf.Load(g.colorOffset + i.x * 4));
    const float3 c1 = UnpackColor(buf.Load(g.colorOffset + i.y * 4));
    const float3 c2 = UnpackColor(buf.Load(g.colorOffset + i.z * 4));
    const float3 vc = c0 + (c1 - c0) * bc.x + (c2 - c0) * bc.y;
    h.albedo = vc;
    h.baked  = vc;
    if (g.tex >= 0) {
        h.albedo = Tex[NonUniformResourceIndex(g.tex)].SampleLevel(Smp, HitUv(g, q.CommittedPrimitiveIndex(), bc), 0).rgb;
        h.baked  = h.albedo * vc;
    }
    return h;
    }
    return h;
}

Hit Trace(float3 o, float3 d, float tmax) { return TraceFaces(o, d, tmax, RAY_FLAG_CULL_BACK_FACING_TRIANGLES); }

static const float kBackFaceHeight = 1.2;

float TraceAnyFace(float3 o, float3 d, float tmax, float3 n)
{
    float start = 0;
    [loop] for (int pass = 0; pass < 4; ++pass) {
        const Hit h = TraceFaces(o + d * start, d, tmax - start, RAY_FLAG_NONE);
        if (h.t >= NRD_FP16_MAX) return NRD_FP16_MAX;
        const float3 hp = o + d * (start + h.t);
        const float climb = dot(hp - o, n);
        if (!(abs(dot(h.N, n)) > 0.9 && climb > 0.0 && climb < 0.35)) return start + h.t;
        start += h.t + 0.02;
        if (start >= tmax) break;
    }
    return NRD_FP16_MAX;
}

static const float kLayerHeight = 0.35;

bool SameLayer(float3 o, float3 n, float3 hp, float3 hn)
{
    const float climb = dot(hp - o, n);
    return abs(dot(hn, n)) > 0.9 && climb > 0.0 && climb < kLayerHeight;
}

Hit TraceLayered(float3 o, float3 d, float tmax, float3 n)
{
    float start = 0;
    Hit h;
    [loop] for (int i = 0; i < 3; ++i) {
        h = Trace(o + d * start, d, tmax - start);
        if (h.t >= NRD_FP16_MAX) return h;
        if (!SameLayer(o, n, o + d * (start + h.t), h.N)) { h.t += start; return h; }
        start += h.t + 0.02;
        if (start >= tmax) break;
    }
    h.t = NRD_FP16_MAX;
    return h;
}

bool OccludedFront(float3 o, float3 d, float tmax)
{
    return Trace(o, d, tmax).t < NRD_FP16_MAX;
}

float  Valid(float v)  { return isfinite(v) ? v : 0.0; }
float3 Valid(float3 v) { return all(isfinite(v)) ? max(v, 0.0) : float3(0, 0, 0); }

float AirMass(float z)
{
    z = saturate(z);
    return 1.0 / (z + 0.15 * pow(max(93.885 - degrees(acos(z)), 1.0), -1.253));
}

static const float3 kBetaR = float3(0.055, 0.13, 0.30);
static const float3 kBetaM = float3(0.02, 0.02, 0.02);

static const float kUnderWall = 0.3;

float TraceFromFloor(float3 o, float3 d, float tmax, float3 n)
{
    const float first = TraceAnyFace(o, d, tmax, n);
    if (first < NRD_FP16_MAX || n.z < 0.7) return first;
    return TraceAnyFace(o + float3(0, 0, kUnderWall), d, tmax, n);
}

bool Occluded(float3 o, float3 d, float tmax, float3 n)
{
    return TraceFromFloor(o, d, tmax, n) < NRD_FP16_MAX;
}

float3 AtmosphereColor(float3 d)
{
    const float  below = saturate(-d.z * 6.0);
    const float3 v     = normalize(float3(d.xy, max(d.z, 0.0) + 1e-3));
    const float  mu    = dot(v, TrueSun.xyz);
    const float  g     = 0.76;
    const float  phaseR = 0.0596831 * (1.0 + mu * mu);
    const float  phaseM = 0.0795775 * (1.0 - g * g) / pow(max(1.0 + g * g - 2.0 * g * mu, 1e-3), 1.5);
    const float3 ext    = kBetaR + kBetaM;
    const float3 tView  = exp(-ext * AirMass(v.z));
    const float3 tSun   = exp(-ext * min(AirMass(TrueSun.z), 6.0));
    float3 sky = TrueSun.w * (kBetaR * phaseR + kBetaM * phaseM) / ext * (1.0 - tView) * tSun;
    sky += float3(0.004, 0.006, 0.014) * Atmosphere.w;
    sky  = pow(saturate(1.0 - exp(-sky * 1.5)), 1.0 / 2.2);
    if (WeatherTop.a > 0.0) sky = lerp(sky, lerp(WeatherBot.rgb, WeatherTop.rgb, sqrt(saturate(v.z * 1.6))), WeatherTop.a);
    return sky * (1.0 - 0.6 * below);
}

float3 Sky(float3 d)
{
    if (Atmosphere.x > 0.5) return AtmosphereColor(d) * SkyColor.rgb;
    float h = saturate(d.z * 0.5 + 0.5);
    return SkyColor.rgb * lerp(0.35, 1.0, h);
}

float Hash3(float3 p)
{
    p = frac(p * 0.3183099 + 0.1);
    p *= 17.0;
    return frac(p.x * p.y * p.z * (p.x + p.y + p.z));
}

float Noise3(float3 x)
{
    const float3 i = floor(x);
    float3 f = frac(x);
    f = f * f * (3.0 - 2.0 * f);
    return lerp(lerp(lerp(Hash3(i), Hash3(i + float3(1, 0, 0)), f.x), lerp(Hash3(i + float3(0, 1, 0)), Hash3(i + float3(1, 1, 0)), f.x), f.y),
                lerp(lerp(Hash3(i + float3(0, 0, 1)), Hash3(i + float3(1, 0, 1)), f.x), lerp(Hash3(i + float3(0, 1, 1)), Hash3(i + float3(1, 1, 1)), f.x), f.y), f.z);
}

float CloudDensity(float3 p, bool detail)
{
    const float h = saturate((p.z - Clouds.z) / (Clouds.w - Clouds.z));
    const float profile = saturate(h * 6.0) * saturate((1.0 - h) * 1.8);
    if (profile <= 0.0) return 0.0;
    const float3 q = p * 0.0009 + float3(Atmosphere.y * 0.010, Atmosphere.y * 0.004, 0.0);
    float n = 0.58 * Noise3(q) + 0.30 * Noise3(q * 2.3 + 3.1);
    const float threshold = (1.0 - Clouds.x) * 0.80 + 0.12 * (1.0 - profile);
    if (n + 0.14 < threshold) return 0.0;
    n += 0.09 * Noise3(q * 5.3 + 7.7);
    if (detail) n += 0.05 * Noise3(q * 12.7 + 1.3) - 0.03;
    return saturate((n - threshold) * 4.0);
}

float4 MarchClouds(float3 o, float3 d, uint2 px)
{
    if (Clouds.x <= 0.0) return float4(0, 0, 0, 1);
    const float  sunLevel = dot(SunColor.rgb, float3(0.3, 0.6, 0.1));
    const float3 ambient  = lerp(AtmosphereColor(float3(0, 0, 1)) * 0.35 + (0.30 * sunLevel + 0.03), WeatherTop.rgb * 1.05, WeatherTop.a);
    const float  deckCover = saturate((Clouds.x - 0.2) / 0.6);
    const float3 edge      = (ambient + SunColor.rgb * 0.55 * 0.4) * deckCover;
    if (d.z < 0.02) return float4(edge, 1.0 - deckCover);
    const float t0 = (Clouds.z - o.z) / d.z, t1 = (Clouds.w - o.z) / d.z;
    const int   steps = clamp((int)Clouds.y, 4, 64);
    const float ds = (t1 - t0) / steps;
    const float jitter = frac(52.9829189 * frac(dot(float2(px), float2(0.06711056, 0.00583715))));
    float3 light = 0;
    float  trans = 1;
    for (int i = 0; i < steps && trans > 0.02; ++i) {
        const float3 p = o + d * (t0 + (i + jitter) * ds);
        const float  den = CloudDensity(p, true);
        if (den <= 0.005) continue;
        float tau = 0;
        for (int k = 1; k <= 2; ++k) tau += CloudDensity(p + SunDir.xyz * (k * 170.0), false);
        const float sunVis = max(exp(-tau * 1.6), 0.4 * exp(-tau * 0.3));
        const float a = 1.0 - exp(-den * ds * 0.012);
        light += trans * a * (SunColor.rgb * 0.55 * sunVis + ambient);
        trans *= 1.0 - a;
    }
    const float horizon = saturate(d.z * 9.0);
    return float4(lerp(edge, light, horizon), lerp(1.0 - deckCover, trans, horizon));
}

float CloudShadow(float3 p)
{
    if (Atmosphere.x < 0.5 || Clouds.x <= 0.0 || SunDir.z < 0.05) return 1.0;
    const float mid = (Clouds.z + Clouds.w) * 0.5;
    if (p.z >= mid) return 1.0;
    const float den = CloudDensity(p + SunDir.xyz * ((mid - p.z) / SunDir.z), false);
    return 1.0 - 0.75 * smoothstep(0.05, 0.7, den);
}

float3 SkyImage(float3 d, uint2 px)
{
    float3 sky = AtmosphereColor(d);
    if (Atmosphere.w < 0.99 && d.z > -0.02) {
        const float disk = smoothstep(0.99990, 0.99997, dot(d, TrueSun.xyz));
        sky += disk * exp(-(kBetaR + kBetaM) * AirMass(TrueSun.z)) * saturate(TrueSun.w) * 6.0 * WeatherBot.a;
    }
    if (Atmosphere.w > 0.01 && d.z > 0.0) {
        const float3 cell = floor(d * 240.0);
        const float  star = step(0.9975, Hash3(cell));
        sky += star * Hash3(cell + 7.0) * Atmosphere.w * saturate(d.z * 6.0);
    }
    const float4 c = MarchClouds(CameraPos.xyz, d, px);
    return sky * c.a + c.rgb;
}

float3 RoomAmbientAt(float3 p)
{
    if (RoomAmbient.w <= 0.0) return 0;
    float3 sum = 0;
    const int count = min((int)LightCount, 32);
    for (int i = 0; i < count; ++i) {
        if (LightAtt[i].w < 0.5) continue;
        const uint v = (uint)LightAtt[i].w - 1u;
        float3 c = float3(v & 63u, (v >> 6) & 63u, (v >> 12) & 63u) / 32.0;
        const uint zone = (v >> 18) & 63u;
        if (zone != 0u) c *= saturate(1.0 - (length(LightPos[i].xyz - p) - (float)zone * 2.0) / 5.0);
        sum += c;
    }
    return sum / max(max(sum.r, max(sum.g, sum.b)), 1.0) * RoomAmbient.w;
}

float3 FixtureGlowAt(float3 p);
float3 ShadeHit(float3 p, Hit hit)
{
    float nl = saturate(dot(hit.N, SunDir.xyz));
    float vis = 0;
    if (OPT_SUN && nl > 0 && !Occluded(p + hit.N * 0.05, SunDir.xyz, 500, hit.N)) vis = 1;
    const float3 skyHere = FullRT != 0 ? 0.0 : Sky(hit.N);
    return hit.albedo * (SunColor.rgb * nl * vis + skyHere + hit.emissive + RoomAmbientAt(p)) + (FullRT != 0 ? 0.0 : hit.baked * HitBaked);
}

float3 OutsideLight(float3 dir, Hit hit)
{
    if (hit.glass && asfloat(Expo.Load(52)) > 0.5) return 0;
    return hit.glass ? Sky(dir) * WindowLight.rgb : Sky(dir);
}

float LightWeight(int i, float3 p, float3 n, out float3 l, out float dist)
{
    const float3 d = LightPos[i].xyz - p;
    const float  d2 = dot(d, d);
    dist = LightPos[i].w;
    l = float3(0, 0, 1);
    if (d2 >= LightPos[i].w * LightPos[i].w || dot(n, d) <= 0.0) return 0.0;
    dist = sqrt(d2);
    l = d / max(dist, 1e-3);
    const float nl = dot(n, l);
    const float3 a = LightAtt[i].xyz;
    float k = 1.0 / (1.0 + dist * dist * (Bounce.w > 0.0 ? Bounce.w : 0.08));
    if (LightAtt[i].w > 0.5) {
        k = 1.0 / max(a.x + a.y * dist + a.z * dist * dist, 1.0);
        if (a.y + a.z < 1e-6) k /= 1.0 + d2 * (Bounce.w > 0.0 ? Bounce.w : 0.02);
    }
    const float reach2 = d2 / (LightPos[i].w * LightPos[i].w);
    const float edge = saturate(1.0 - reach2 * reach2);
    k *= edge * edge * nl;
    if (LightCol[i].w > -1.5)
        k *= smoothstep(LightCol[i].w, max(LightDir[i].w, LightCol[i].w + 1e-3), dot(-l, LightDir[i].xyz));
    return k;
}

int LightNumberMapped(uint index, bool currentToPrevious)
{
    if (index >= 160u) return -1;
    const uint word = (currentToPrevious ? 40u : 0u) + (index >> 2);
    const uint v = (RestirMap[word >> 2][word & 3u] >> ((index & 3u) * 8u)) & 255u;
    return v == 255u ? -1 : (int)v;
}
float LightVisibility(int i) { return saturate(asfloat(Expo.Load(64 + i * 4))); }
float PortalVisibility(int i) { return saturate(asfloat(Expo.Load(192 + i * 4))); }
static const float kLampRadius = 0.25;

float3 SampleLights(float3 p, float3 n, float3 ng, inout uint seed)
{
    const int count = min((int)LightCount, 32);
    if (count <= 0) return 0;
    float weights[32];
    float total = 0;
    for (int i = 0; i < count; ++i) {
        float3 l; float dist;
        weights[i] = LightWeight(i, p, n, l, dist) * dot(LightCol[i].rgb, float3(0.3, 0.6, 0.1)) * (0.03 + LightVisibility(i));
        total += weights[i];
    }
    if (total <= 1e-5) return 0;
    float pick = Rand(seed) * total;
    int chosen = -1;
    for (int i = 0; i < count; ++i) {
        if (weights[i] <= 0.0) continue;
        chosen = i;
        pick -= weights[i];
        if (pick <= 0.0) break;
    }
    if (chosen < 0) return 0;
    float3 l; float dist;
    const float k = LightWeight(chosen, p, n, l, dist);
    if (dot(l, ng) <= 0.0) return 0;
    float3 lit = LightCol[chosen].rgb * k * total / weights[chosen];
    const float keep = saturate(dot(lit, float3(0.3, 0.6, 0.1)) / 0.01);
    if (keep < 1.0) {
        if (Rand(seed) >= keep) return 0;
        lit /= max(keep, 1e-3);
    }
    const float u0 = Rand(seed) * 2.0 - 1.0, u1 = Rand(seed) * 6.2831853, u2 = pow(Rand(seed), 0.3333) * kLampRadius;
    const float s0 = sqrt(saturate(1.0 - u0 * u0));
    const float3 toLamp = LightPos[chosen].xyz + float3(s0 * cos(u1), s0 * sin(u1), u0) * u2 - p;
    const float  lampDist = length(toLamp);
    const float3 l2 = toLamp / max(lampDist, 1e-3);
    if (dot(l2, ng) <= 0.0) return 0;
    if (OPT_LOCAL && TraceFromFloor(p, l2, max(lampDist - 0.6, 0.01), ng) < NRD_FP16_MAX) return 0;
    return lit;
}

static const int kFixPerCell = 48;
static const float kFireflyCeiling = 2.0;
static const float kFixtureGlow = 0.6;
float3 FixNormal(uint v)
{
    float2 e = float2(v & 0xFFFFu, v >> 16) / 65535.0 * 2.0 - 1.0;
    float3 n = float3(e.x, e.y, 1.0 - abs(e.x) - abs(e.y));
    if (n.z < 0.0) n.xy = (1.0 - abs(n.yx)) * float2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    return normalize(n);
}
uint2 FixCellRange(float3 p)
{
    if (FixInfo.x < 0.5) return uint2(0, 0);
    const int3 c = (int3)floor((p - FixInfo.yzw) / FixGrid.x);
    if (any(c < 0) || c.x >= (int)FixGrid.y || c.y >= (int)FixGrid.z || c.z >= (int)FixGrid.w) return uint2(0, 0);
    const uint cell = (uint)((c.z * (int)FixGrid.z + c.y) * (int)FixGrid.y + c.x);
    const uint v = FixCells[cell >> 2][cell & 3u];
    return uint2(v & 0x1FFFu, min((v >> 13) & 63u, (uint)kFixPerCell));
}
uint FixAt(uint e) { return (FixList[e >> 3][(e >> 1) & 3u] >> ((e & 1u) * 16u)) & 0xFFFFu; }
float FixtureWeight(uint f, float3 p, float3 n, out float3 l, out float dist)
{
    const float3 d = FixPos[f].xyz - p;
    const float  d2 = dot(d, d);
    const float  r = floor(FixPos[f].w), src = max(frac(FixPos[f].w), 0.05);
    l = float3(0, 0, 1); dist = r;
    if (d2 >= r * r || dot(n, d) <= 0.0) return 0.0;
    dist = sqrt(d2);
    l = d / max(dist, 1e-3);
    float k = 1.0 / (d2 + src * src);
    const float reach2 = d2 / (r * r);
    const float edge = saturate(1.0 - reach2 * reach2);
    const float face = saturate(dot(FixNormal(asuint(FixCol[f].w)), -l)) * 0.8 + 0.2;
    return k * edge * edge * dot(n, l) * face;
}
float3 SampleFixtures(float3 p, float3 n, float3 ng, inout uint seed)
{
    const uint2 range = FixCellRange(p);
    if (range.y == 0u) return 0;
    const uint N = range.y;
    const uint M = min(N, 8u);
    float wSum = 0, wChosen = 0;
    int chosen = -1;
    for (uint i = 0; i < M; ++i) {
        const uint idx = M == N ? i : min((uint)(Rand(seed) * N), N - 1u);
        float3 l; float dist;
        const uint fc = FixAt(range.x + idx);
        const float w = FixtureWeight(fc, p, n, l, dist) * dot(FixCol[fc].rgb, float3(0.3, 0.6, 0.1)) * (M == N ? 1.0 : (float)N);
        if (w <= 0.0) continue;
        wSum += w;
        if (Rand(seed) * wSum <= w) { chosen = (int)idx; wChosen = w; }
    }
    if (chosen < 0 || wSum <= 1e-5) return 0;
    const uint f = FixAt(range.x + (uint)chosen);
    float3 l; float dist;
    const float k = FixtureWeight(f, p, n, l, dist);
    if (dot(l, ng) <= 0.0) return 0;
    const float pHat = wChosen / (M == N ? 1.0 : (float)N);
    float3 lit = FixCol[f].rgb * k * (wSum / (M == N ? 1.0 : (float)M)) / max(pHat, 1e-8);
    const float keep = saturate(dot(lit, float3(0.3, 0.6, 0.1)) / 0.01);
    if (keep < 1.0) {
        if (Rand(seed) >= keep) return 0;
        lit /= max(keep, 1e-3);
    }
    if (!OPT_LOCAL) return lit;
    const float src = max(frac(FixPos[f].w), 0.05);
    const float u0 = Rand(seed) * 2.0 - 1.0, u1 = Rand(seed) * 6.2831853, u2 = pow(Rand(seed), 0.3333) * src * 0.5;
    const float s0 = sqrt(saturate(1.0 - u0 * u0));
    const float3 toLamp = FixPos[f].xyz + float3(s0 * cos(u1), s0 * sin(u1), u0) * u2 - p;
    const float  lampDist = length(toLamp);
    const float3 l2 = toLamp / max(lampDist, 1e-3);
    if (dot(l2, ng) <= 0.0) return 0;
    if (TraceFromFloor(p, l2, max(lampDist - src - 0.1, 0.01), ng) < NRD_FP16_MAX) return 0;
    return lit;
}
float3 FixtureGlowAt(float3 p)
{
    const uint2 range = FixCellRange(p);
    float3 glow = 0;
    for (uint i = 0; i < range.y; ++i) {
        const uint f = FixAt(range.x + i);
        const float src = max(frac(FixPos[f].w), 0.05);
        const float d = length(FixPos[f].xyz - p);
        const float shell = min(src * 1.25, 0.35) + 0.04;
        if (d >= shell) continue;
        const float t = saturate(1.0 - d / shell);
        glow = max(glow, FixCol[f].rgb / (3.14159265 * src * src) * (t * t * (3.0 - 2.0 * t)) * kFixtureGlow);
    }
    return min(glow, 64.0);
}

float3 CharacterFixtures(float3 p, float3 n)
{
    const uint2 range = FixCellRange(p);
    float3 sum = 0;
    int rays = 0;
    for (uint i = 0; i < range.y; ++i) {
        float3 l; float dist;
        const uint f = FixAt(range.x + i);
        const float3 c = FixCol[f].rgb * FixtureWeight(f, p, n, l, dist);
        if (dot(c, float3(0.3, 0.6, 0.1)) < 0.02) continue;
        if (OPT_LOCAL) {
            if (rays >= 4) continue;
            ++rays;
            if (TraceAnyFace(p, l, max(dist - max(frac(FixPos[f].w), 0.05) - 0.1, 0.01), n) < NRD_FP16_MAX) continue;
        }
        sum += c;
    }
    return sum;
}

float3 SamplePortals(float3 p, float3 n, float3 ng, inout uint seed)
{
    const int all = min((int)PortalInfo.x, 128);
    if (all <= 0 || asfloat(Expo.Load(52)) <= 0.5) return 0;
    const int count = min(all, clamp((int)PortalInfo.z, 1, 128));
    const int first = all > count ? min((int)(Rand(seed) * (float)all), all - 1) : 0;
    float weights[128];
    float total = 0;
    for (int s = 0; s < count; ++s) {
        const int i = (first + s) % all;
        weights[s] = 0.0;
        const float3 d  = PortalC[i].xyz - p;
        const float  d2 = max(dot(d, d), 0.25);
        const float  inv = rcp(sqrt(d2));
        const float cs = dot(n, d) * inv;
        if (cs < -0.3) continue;
        weights[s] = PortalN[i].w * max(cs, 0.05) * max(abs(dot(d, PortalN[i].xyz)) * inv, 0.05) / d2 * (0.05 + PortalVisibility(i));
        total += weights[s];
    }
    if (total <= 1e-6) return 0;
    float pick = Rand(seed) * total;
    int chosenSlot = -1;
    for (int s = 0; s < count; ++s) {
        if (weights[s] <= 0.0) continue;
        chosenSlot = s;
        pick -= weights[s];
        if (pick <= 0.0) break;
    }
    if (chosenSlot < 0) return 0;
    const int    chosen = (first + chosenSlot) % all;
    const float  u1 = Rand(seed) * 2.0 - 1.0, u2 = Rand(seed) * 2.0 - 1.0;
    const float3 x  = PortalC[chosen].xyz + PortalU[chosen].xyz * u1 + PortalV[chosen].xyz * u2;
    const float3 d  = x - p;
    const float  d2 = max(dot(d, d), 0.25);
    const float  dist = sqrt(d2);
    const float3 l  = d / dist;
    const float  cs = dot(n, l);
    if (cs <= 0.0 || dot(l, ng) <= 0.0) return 0;
    const float  cp = abs(dot(l, PortalN[chosen].xyz));
    float amount = PortalInfo.y * cs * cp * PortalN[chosen].w / (3.14159265 * d2) * total / weights[chosenSlot] * ((float)all / (float)count);
    amount = min(amount, 8.0 * PortalInfo.y / 80.0);
    const float keep = saturate(amount * dot(SkyColor.rgb, float3(0.3, 0.6, 0.1)) / 0.01);
    if (keep < 1.0) {
        if (Rand(seed) >= keep) return 0;
        amount /= max(keep, 1e-3);
    }
    if (TraceFromFloor(p, l, max(dist - 0.05, 0.01), ng) < NRD_FP16_MAX) return 0;
    const float3 glass = float3(PortalU[chosen].w, PortalV[chosen].w, PortalC[chosen].w);
    return Sky(l) * glass * amount;
}

float3 SampleDirect(float3 p, float3 n, float3 ng, inout uint seed)
{
    return SampleLights(p, n, ng, seed) + SamplePortals(p, n, ng, seed) + SampleFixtures(p, n, ng, seed);
}

float3 SampleDirectCached(float3 p, float3 n, float3 ng, inout uint seed)
{
    const float3 fixtures = SampleFixtures(p, n, ng, seed);
    if ((int)PortalInfo.x <= 0 || asfloat(Expo.Load(52)) <= 0.5) return SampleLights(p, n, ng, seed) + fixtures;
    if ((int)LightCount <= 0) return SamplePortals(p, n, ng, seed) + fixtures;
    return (Rand(seed) < 0.5 ? SampleLights(p, n, ng, seed) * 2.0 : SamplePortals(p, n, ng, seed) * 2.0) + fixtures;
}

float3 CharacterLamps(float3 p, float3 n)
{
    float3 sum = 0;
    int rays = 0;
    const int count = min((int)LightCount, 32);
    for (int i = 0; i < count; ++i) {
        float3 l; float dist;
        const float k = LightWeight(i, p, n, l, dist);
        const float3 c = LightCol[i].rgb * k;
        if (dot(c, float3(0.3, 0.6, 0.1)) < 0.02 || LightVisibility(i) < 0.01) continue;
        if (OPT_LOCAL) {
            if (rays >= 6) continue;
            ++rays;
            if (TraceAnyFace(p, l, max(dist - 0.6, 0.01), n) < NRD_FP16_MAX) continue;
        }
        sum += c;
    }
    return sum;
}

SharcParameters BuildSharc()
{
    SharcParameters p;
    p.hashGridParameters.cameraPosition = CameraPos.xyz;
    p.hashGridParameters.sceneScale     = SharcInfo.x;
    p.hashGridParameters.logarithmBase  = SHARC_GRID_LOGARITHM_BASE;
    p.hashGridParameters.levelBias      = SHARC_GRID_LEVEL_BIAS;
    p.hashGridData.capacity             = kSharcEntries;
    p.hashGridData.hashEntriesBuffer    = SharcHash;
    p.accumulationBuffer                = SharcAccum;
    p.resolvedBuffer                    = SharcResolved;
    p.radianceScale                     = SHARC_RADIANCE_SCALE;
    return p;
}

[numthreads(256, 1, 1)]
void CSSharcResolve(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= kSharcEntries) return;
    SharcResolveParameters r;
    r.cameraPositionPrev   = PrevCam.xyz;
    r.accumulationFrameNum = (uint)SharcInfo.y;
    r.responsiveFrameNum   = 0;
    r.staleFrameNumMax     = (uint)SharcInfo.z;
    r.frameIndex           = asuint(ProjParams.w);
    SharcResolveEntry(id.x, BuildSharc(), r);
}

#if SHARC_UPDATE
float3 SharcCosineDir(float3 n, inout uint seed)
{
    float3 t, b;
    Basis(n, t, b);
    const float u1 = Rand(seed), u2 = Rand(seed);
    const float r = sqrt(u1), phi = 6.2831853 * u2;
    return normalize(t * (r * cos(phi)) + b * (r * sin(phi)) + n * sqrt(max(0.0, 1.0 - u1)));
}

[numthreads(8, 8, 1)]
void CSSharcUpdate(uint3 id : SV_DispatchThreadID)
{
    const uint  tile = max((uint)SharcInfo.w, 1u);
    const uint2 tiles = Size / tile;
    if (any(id.xy >= tiles)) return;
    if (OPT_LIGHTING < 2) return;
    uint seed = Hash(id.x * 1973u + id.y * 9277u + asuint(ProjParams.w) * 26699u);
    const uint2 px = min(id.xy * tile + uint2(Rand(seed) * tile, Rand(seed) * tile), Size - 1u);
    const float d = Depth[px];
    const float4 g = GNormal[px];
    const float3 gN = g.xyz * 2.0 - 1.0;
    if (d >= 0.99999 || d <= 0.0 || dot(gN, gN) < 0.05) return;
    const float matA = GAlbedo[px].a;
    if (matA > 0.75 && matA < 0.9) return;
    const float3 N = normalize(gN);
    const float3 V = normalize(CameraPos.xyz - WorldFromDepth(px, d));
    const float3 P = WorldFromDepth(px, d) + N * 0.03 + V * 0.02;
    gRayMin  = 0.0005;
    gRayMask = 0x01;
    const SharcParameters sharc = BuildSharc();
    SharcState state;
    SharcInit(state);
    Hit h;
    h.t = 0.0; h.N = N; h.albedo = GAlbedo[px].rgb; h.baked = h.albedo; h.glass = false; h.mirror = false;
    h.emissive = (matA > 0.04 && matA < 0.394) ? (matA - 0.05) / 0.35 * 255.0 / 8.0 : 0.0;
    float3 p = P;
    const int surfaces = (int)SharcInfo2.x;
    for (int bounce = 0; bounce < surfaces; ++bounce) {
        SharcHitData sh;
        sh.positionWorld = p;
        sh.normalWorld   = h.N;
        const float3 own = ShadeHit(p, h) + h.albedo * SampleDirectCached(p, h.N, h.N, seed);
        if (!SharcUpdateHit(sharc, state, sh, Valid(own), Rand(seed))) return;
        const float3 dir = SharcCosineDir(h.N, seed);
        SharcSetThroughput(state, h.albedo);
        const float3 o = p + h.N * 0.03;
        const float3 n = h.N;
        h = TraceLayered(o, dir, 60.0, n);
        if (h.t >= NRD_FP16_MAX) {
            SharcUpdateMiss(sharc, state, asfloat(Expo.Load(52)) > 0.5 ? float3(0, 0, 0) : Sky(dir));
            return;
        }
        p = o + dir * h.t + h.N * 0.03;
    }
}
#endif

static const uint  kAirW = 160, kAirH = 90, kAirD = 48;
static const uint  kResW = 80, kResH = 45, kResD = 24;
static const float kAirNear = 1.0, kAirFar = 450.0;
static const uint  kVolSun = 200u;
float AirSliceEnd(float s) { return kAirNear * pow(kAirFar / kAirNear, s / (float)kAirD); }

float3 AirGridPoint(float3 c, float3 dims)
{
    const float3 dir = normalize(WorldFromDepth(c.xy / dims.xy * float2(Size), 0.5) - CameraPos.xyz);
    return CameraPos.xyz + dir * (kAirNear * pow(kAirFar / kAirNear, c.z / dims.z));
}
bool AirGridCoord(float3 q, float3 dims, bool prev, out float3 c)
{
    const float4 h = mul(prev ? PrevViewProj : ViewProj, float4(q, 1));
    c = 0;
    if (h.w <= 1e-3) return false;
    const float2 uv = float2(h.x / h.w * 0.5 + 0.5, 0.5 - h.y / h.w * 0.5);
    const float  dist = length(q - (prev ? PrevCam.xyz : CameraPos.xyz));
    c = float3(uv * dims.xy, log(max(dist, kAirNear) / kAirNear) / log(kAirFar / kAirNear) * dims.z);
    return all(uv >= 0.0) && all(uv <= 1.0) && c.z < dims.z;
}
bool AirSeen(float3 c, float3 dims, float3 q, float margin)
{
    const uint2 pj = min(uint2(c.xy / dims.xy * float2(Size)), Size - 1u);
    const float dS = Depth[pj];
    return !(dS < 0.99999 && dS > 0.0) || length(q - CameraPos.xyz) <= length(WorldFromDepth(pj, dS) - CameraPos.xyz) + margin;
}

int VolWindowCount() { return asfloat(Expo.Load(52)) > 0.5 ? min((int)PortalInfo.x, 128) : 0; }

struct VolSample { float3 dir; float dist; float stop; float3 radiance; };
VolSample VolLight(uint index, float3 q, float2 uv, bool skyAverage, bool roofed)
{
    VolSample s;
    s.dir = float3(0, 0, 1); s.dist = 0; s.stop = 0; s.radiance = 0;
    if (index == kVolSun) {
        if (!OPT_SUN || !roofed || SunColor.r + SunColor.g + SunColor.b < 0.05) return s;
        float3 st, sb;
        Basis(SunDir.xyz, st, sb);
        const float r = sqrt(uv.x) * SunDir.w, a = 6.2831853 * uv.y;
        s.dir = normalize(SunDir.xyz + st * (r * cos(a)) + sb * (r * sin(a)));
        s.dist = 300.0;
        s.radiance = SunColor.rgb * max(Bounce.y, 0.0) * CloudShadow(q);
        return s;
    }
    if (index < 32u) {
        const int i = (int)index;
        if (i >= min((int)LightCount, 32)) return s;
        float3 l; float dist;
        const float k = LightWeight(i, q, normalize(LightPos[i].xyz - q + 1e-5), l, dist);
        if (k <= 0.0) return s;
        const float z = uv.x * 2.0 - 1.0, phi = uv.y * 6.2831853, ring = sqrt(saturate(1.0 - z * z));
        const float3 to = LightPos[i].xyz + float3(ring * cos(phi), ring * sin(phi), z) * kLampRadius - q;
        s.dist = max(length(to), 1e-3);
        s.dir = to / s.dist;
        s.stop = 0.6;
        s.radiance = LightCol[i].rgb * k;
        return s;
    }
    const int w = (int)index - 32;
    if (w >= VolWindowCount()) return s;
    const float3 x = PortalC[w].xyz + PortalU[w].xyz * (uv.x * 2.0 - 1.0) + PortalV[w].xyz * (uv.y * 2.0 - 1.0);
    const float3 d = x - q;
    const float  d2 = max(dot(d, d), 0.25);
    s.dist = sqrt(d2);
    s.dir = d / s.dist;
    s.stop = 0.05;
    const float amount = min(PortalInfo.y * abs(dot(s.dir, PortalN[w].xyz)) * PortalN[w].w / (3.14159265 * d2), 8.0 * PortalInfo.y / 80.0);
    s.radiance = (skyAverage ? SkyColor.rgb * 0.8 : Sky(s.dir)) * float3(PortalU[w].w, PortalV[w].w, PortalC[w].w) * amount;
    return s;
}
float VolTarget(VolSample s) { return max(dot(s.radiance, float3(0.3, 0.6, 0.1)), 0.0); }
bool  VolVisible(float3 q, VolSample s) { return TraceFaces(q, s.dir, max(s.dist - s.stop, 0.01), RAY_FLAG_NONE).t >= NRD_FP16_MAX; }

uint  VolIndexOf(uint k, uint lamps, uint windows) { return k < lamps ? k : (k < lamps + windows ? 32u + (k - lamps) : kVolSun); }
float VolSourceWeight(uint k, uint lamps, uint windows, float3 q, bool roofed)
{
    if (k < lamps) {
        float3 l; float dist;
        return LightWeight((int)k, q, normalize(LightPos[k].xyz - q + 1e-5), l, dist) * dot(LightCol[k].rgb, float3(0.3, 0.6, 0.1)) * (0.03 + LightVisibility((int)k));
    }
    if (k < lamps + windows) {
        const int w = (int)(k - lamps);
        const float3 d = PortalC[w].xyz - q;
        const float  d2 = max(dot(d, d), 0.25);
        return PortalN[w].w * max(abs(dot(d, PortalN[w].xyz)) * rcp(sqrt(d2)), 0.05) / d2 * PortalInfo.y / 3.14159265
             * dot(SkyColor.rgb, float3(0.3, 0.6, 0.1)) * (0.05 + PortalVisibility(w));
    }
    return 0.0;
}

struct VolRes { uint light; float2 uv; float W; float M; bool valid; };
uint4 VolPack(VolRes r)
{
    const uint2 u = uint2(saturate(r.uv) * 65535.0);
    return uint4(r.valid ? ((r.light & 0xFFu) | (min((uint)r.M, 255u) << 8) | 0x80000000u) : 0u, asuint(r.W), u.x | (u.y << 16), 0u);
}
VolRes VolUnpack(uint4 p)
{
    VolRes r;
    r.valid = (p.x & 0x80000000u) != 0u;
    r.light = p.x & 0xFFu;
    r.M = (float)((p.x >> 8) & 0xFFu);
    r.W = asfloat(p.y);
    r.uv = float2(p.z & 0xFFFFu, p.z >> 16) / 65535.0;
    return r;
}

[numthreads(8, 8, 8)]
void CSVolRestir(uint3 id : SV_DispatchThreadID)
{
    if (any(id >= uint3(kResW, kResH, kResD))) return;
    const uint   frame  = asuint(ProjParams.w);
    const bool   writeB = (frame & 1u) != 0u;
    const float3 dims   = float3(kResW, kResH, kResD);
    uint seed = Hash(id.x + id.y * 977u + id.z * 59359u + frame * 26699u);
    const float3 c = float3(id) + float3(Rand(seed), Rand(seed), Rand(seed));
    const float3 q = AirGridPoint(c, dims);
    gRayMin  = 0.0005;
    gRayMask = 0xFF;
    VolRes r;
    r.light = 0; r.uv = 0; r.W = 0; r.M = 0; r.valid = false;
    if (OPT_VOLUMETRIC && VolInfo.z > 0.0 && AirSeen(c, dims, q, 0.5)) {
        const bool roofed = true;
        const uint lamps = (uint)min((int)LightCount, 32), windows = (uint)VolWindowCount(), n = lamps + windows + 1u;
        float total = 0;
        for (uint k = 0; k < n; ++k) total += VolSourceWeight(k, lamps, windows, q, roofed);
        if (total > 1e-6) {
            const uint  kCandidates = 8u;
            const float jitter = Rand(seed);
            float run = 0, wsum = 0, selTarget = 0;
            uint  sel = 0, cnt = 0;
            float2 selUv = 0;
            for (uint k = 0; k < n && cnt < kCandidates; ++k) {
                const float pdf = VolSourceWeight(k, lamps, windows, q, roofed) / total;
                run += pdf;
                [loop] while (cnt < kCandidates && ((float)cnt + jitter) / (float)kCandidates < run) {
                    ++cnt;
                    if (pdf <= 0.0) continue;
                    const uint   idx = VolIndexOf(k, lamps, windows);
                    const float2 uv = float2(Rand(seed), Rand(seed));
                    const float  tp = VolTarget(VolLight(idx, q, uv, true, roofed));
                    const float  wr = tp / pdf;
                    wsum += wr;
                    if (wr > 0.0 && Rand(seed) * wsum < wr) { sel = idx; selUv = uv; selTarget = tp; }
                }
            }
            if (selTarget > 0.0) {
                r.light = sel; r.uv = selUv; r.M = 1.0;
                r.W = wsum / ((float)kCandidates * selTarget);
                r.valid = true;
                if (!VolVisible(q, VolLight(sel, q, selUv, true, roofed))) r.W = 0.0;
            }
        }
        float3 pc;
        if (length(CameraPos.xyz - PrevCam.xyz) < 20.0 && AirGridCoord(q, dims, true, pc)) {
            const int3 pi = clamp(int3(pc + float3(Rand(seed) - 0.5, Rand(seed) - 0.5, 0.0)), int3(0, 0, 0), int3(dims) - 1);
            const VolRes p = VolUnpack(writeB ? VolResA[uint3(pi)] : VolResB[uint3(pi)]);
            if (p.valid && p.W > 0.0) {
                const int mapped = p.light == kVolSun ? (int)kVolSun : LightNumberMapped(p.light, false);
                if (mapped >= 0) {
                    const float tpPrev = VolTarget(VolLight((uint)mapped, q, p.uv, true, roofed));
                    const float tpCur  = r.valid ? VolTarget(VolLight(r.light, q, r.uv, true, roofed)) : 0.0;
                    const float mPrev  = min(p.M, 20.0);
                    const float wCur   = tpCur * r.W * max(r.M, 1.0);
                    const float wPrev  = tpPrev * p.W * mPrev;
                    const float sum    = wCur + wPrev;
                    if (sum > 0.0) {
                        const bool takePrev = Rand(seed) * sum < wPrev;
                        const float m = max(r.M, 1.0) + mPrev;
                        if (takePrev) { r.light = (uint)mapped; r.uv = p.uv; }
                        const float tpSel = takePrev ? tpPrev : tpCur;
                        r.M = m;
                        r.W = tpSel > 0.0 ? sum / (m * tpSel) : 0.0;
                        r.valid = true;
                    }
                }
            }
        }
    }
    if (writeB) VolResB[id] = VolPack(r); else VolResA[id] = VolPack(r);
}

void AirHistory(float3 g, bool fromA, out float4 y, out float4 cc)
{
    const float3 gc = clamp(g - 0.5, float3(0, 0, 0), float3(kAirW - 1, kAirH - 1, kAirD - 1));
    const int3   b = int3(gc), e = min(b + 1, int3(kAirW - 1, kAirH - 1, kAirD - 1));
    const float3 f = gc - float3(b);
    y = 0; cc = 0;
    float sum = 0;
    for (uint k = 0; k < 8u; ++k) {
        const uint3 o = uint3(k & 1u, (k >> 1) & 1u, (k >> 2) & 1u);
        const uint3 p = uint3(o.x ? e.x : b.x, o.y ? e.y : b.y, o.z ? e.z : b.z);
        const float4 c = fromA ? AirLocalA[p] : AirLocalB[p];
        if (c.z <= 0.0) continue;
        const float w = (o.x ? f.x : 1.0 - f.x) * (o.y ? f.y : 1.0 - f.y) * (o.z ? f.z : 1.0 - f.z);
        y  += w * (fromA ? AirLitA[p] : AirLitB[p]);
        cc += w * c;
        sum += w;
    }
    if (sum > 1e-4) { y /= sum; cc /= sum; }
}

[numthreads(8, 8, 8)]
void CSFroxel(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= kAirW || id.y >= kAirH || id.z >= kAirD) return;
    const uint   frame  = asuint(ProjParams.w);
    const bool   writeB = (frame & 1u) != 0u;
    const float3 dims   = float3(kAirW, kAirH, kAirD);
    uint seed = Hash(id.x * 7u + id.y * 211u + id.z * 39119u + frame * 9781u);
    const float3 c = float3(id) + float3(Rand(seed), Rand(seed), Rand(seed));
    const float3 q = AirGridPoint(c, dims);
    gRayMin  = 0.0005;
    gRayMask = 0xFF;
    float4 shY = 0;
    float2 shC = 0;
    const bool seen = AirSeen(c, dims, q, 0.2);
    if (!seen) {
        if (writeB) { AirLitB[id] = 0; AirLocalB[id] = 0; } else { AirLitA[id] = 0; AirLocalA[id] = 0; }
        return;
    }
    if (OPT_VOLUMETRIC && VolInfo.z > 0.0 && OPT_SUN && SunColor.r + SunColor.g + SunColor.b > 0.05) {
        float3 st, sb;
        Basis(SunDir.xyz, st, sb);
        const float rs = sqrt(Rand(seed)) * SunDir.w, as = 6.2831853 * Rand(seed);
        const float3 Ls = normalize(SunDir.xyz + st * (rs * cos(as)) + sb * (rs * sin(as)));
        if (TraceFaces(q, Ls, 300.0, RAY_FLAG_NONE).t >= NRD_FP16_MAX) {
            const bool roofed = asfloat(Expo.Load(52)) < 0.02 || TraceFaces(q, float3(0, 0, 1), 40.0, RAY_FLAG_NONE).t < NRD_FP16_MAX;
            if (roofed) {
                const float3 L = SunColor.rgb * max(Bounce.y, 0.0);
                const float Y = dot(L, float3(0.25, 0.5, 0.25)), Co = 0.5 * (L.r - L.b), Cg = -0.25 * L.r + 0.5 * L.g - 0.25 * L.b;
                shY += float4(Y * 0.282095, Y * 0.488603 * Ls);
                shC += float2(Co, Cg) * 0.282095;
            }
        }
    }
    float4 sunY = shY;
    float2 sunC = shC;
    shY = 0; shC = 0;
    if (OPT_VOLUMETRIC && VolInfo.z > 0.0) {
        const float3 rc = c / dims * float3(kResW, kResH, kResD) + float3(Rand(seed), Rand(seed), Rand(seed)) - 0.5;
        const uint3  ri = uint3(clamp(int3(rc), int3(0, 0, 0), int3(kResW - 1, kResH - 1, kResD - 1)));
        const VolRes r = VolUnpack(writeB ? VolResB[ri] : VolResA[ri]);
        if (r.valid && r.W > 0.0) {
            const VolSample s = VolLight(r.light, q, r.uv, false, true);
            if (VolTarget(s) > 0.0 && VolVisible(q, s)) {
                float3 L = s.radiance * r.W;
                const float lum = dot(L, float3(0.3, 0.6, 0.1));
                if (lum > 16.0) L *= 16.0 / lum;
                const float Y = dot(L, float3(0.25, 0.5, 0.25)), Co = 0.5 * (L.r - L.b), Cg = -0.25 * L.r + 0.5 * L.g - 0.25 * L.b;
                shY = float4(Y * 0.282095, Y * 0.488603 * s.dir);
                shC = float2(Co, Cg) * 0.282095;
            }
        }
    }
    float4 oldY = 0, oldC = 0;
    float  age = 0;
    float3 pc;
    if (length(CameraPos.xyz - PrevCam.xyz) < 20.0 && AirGridCoord(q, dims, true, pc)) {
        AirHistory(pc + (float3(Rand(seed), Rand(seed), Rand(seed)) - 0.5) * 0.1, writeB, oldY, oldC);
        age = min(oldC.z * 64.0 + 1.0, 64.0);
    }
    const float wNew = age > 1.0 ? 1.0 / (age + 5.0) : 1.0;
    {
        const float had = max(oldY.x, 0.0), now = max(shY.x, 0.0);
        const float limit = had * 4.0 + 0.02;
        if (age > 3.0 && now > limit) { const float k = limit / now; shY *= k; shC *= k; }
    }
    shY += sunY;
    shC += sunC;
    const float4 y  = lerp(oldY, shY, wNew);
    const float4 cc = float4(lerp(oldC.xy, shC, wNew), max(age, 1.0) / 64.0, 0.0);
    if (writeB) { AirLitB[id] = y; AirLocalB[id] = cc; } else { AirLitA[id] = y; AirLocalA[id] = cc; }
}

[numthreads(8, 8, 1)]
void CSAirIntegrate(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= kAirW || id.y >= kAirH) return;
    const bool   readA   = (asuint(ProjParams.w) & 1u) == 0u;
    const float2 uv      = (float2(id.xy) + 0.5) / float2(kAirW, kAirH);
    const float3 dir     = normalize(WorldFromDepth(uv * float2(Size), 0.5) - CameraPos.xyz);
    const float  inside  = saturate(asfloat(Expo.Load(52)));
    const float  g       = lerp(0.6, 0.2, inside);
    const float  sigmaT  = max(lerp(VolInfo.y, VolInfo.x, inside), 0.0);
    const float  sigmaS  = VolInfo.z * lerp(0.004 * max(Bounce.z, 0.0), 1.0, inside);
    float3 air = 0;
    float  T = 1.0, zPrev = 0;
    for (uint s = 0; s < kAirD; ++s) {
        const float z = AirSliceEnd((float)s + 1.0), zm = (zPrev + z) * 0.5, dz = z - zPrev;
        zPrev = z;
        const uint3 p0 = uint3(id.xy, s);
        const float young = 1.0 - saturate((readA ? AirLocalA[p0] : AirLocalB[p0]).z * 64.0 / 12.0);
        const int   rr = young > 0.5 ? 2 : 1;
        float4 y = 0, cc = 0;
        float  ws = 0;
        for (int dk = -1; dk <= 1; ++dk)
            for (int j = -rr; j <= rr; ++j)
                for (int i = -rr; i <= rr; ++i) {
                    const int3 q3 = int3(int(id.x) + i, int(id.y) + j, int(s) + dk);
                    if (any(q3 < 0) || any(q3 >= int3(kAirW, kAirH, kAirD))) continue;
                    const float4 cq = readA ? AirLocalA[uint3(q3)] : AirLocalB[uint3(q3)];
                    if (cq.z <= 0.0) continue;
                    const float k = exp(-(float)(i * i + j * j) * (rr > 1 ? 0.25 : 0.6) - (float)(dk * dk) * 1.2);
                    y  += (readA ? AirLitA[uint3(q3)] : AirLitB[uint3(q3)]) * k;
                    cc += cq * k;
                    ws += k;
                }
        if (ws > 1e-4) { y /= ws; cc /= ws; }
        const float  Ys = max(y.x * 0.282095 + g * 0.488603 * dot(y.yzw, dir), 0.0);
        const float  Co = cc.x * 0.282095, Cg = cc.y * 0.282095;
        float3 Lin = max(float3(Ys + Co - Cg, Ys + Cg, Ys - Co - Cg), 0.0);
        if (zm <= 60.0) Lin += RoomAmbientAt(CameraPos.xyz + dir * zm) * VolInfo.w;
        const float span = sigmaT > 1e-6 ? (1.0 - exp(-sigmaT * dz)) / sigmaT : dz;
        air += T * sigmaS * span * Lin;
        T   *= exp(-sigmaT * dz);
        AirGrid[uint3(id.xy, s)]  = float4(air, 1.0);
        AirTrans[uint3(id.xy, s)] = float4(T, 0, 0, 0);
    }
}

bool MovedObject(float3 wp, bool character, inout float3 wasAt)
{
    if (!character) {
        bool inside = false;
        const int boxes = min((int)SharcInfo2.z, 8);
        for (int i = 0; i < boxes && !inside; ++i) inside = all(wp >= MovedMin[i].xyz) && all(wp <= MovedMax[i].xyz);
        if (!inside) return false;
    }
    const float3 to = wp - CameraPos.xyz;
    const float  dist = length(to);
    RayDesc r;
    r.Origin = CameraPos.xyz; r.Direction = to / max(dist, 1e-4); r.TMin = max(dist - 1.5, 0.01); r.TMax = dist + 1.5;
    RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
    q.TraceRayInline(Scene, RAY_FLAG_NONE, character ? 0x02 : 0x01, r);
    q.Proceed();
    if (q.CommittedStatus() != COMMITTED_TRIANGLE_HIT) return false;
    const GeomRecord g = Records[q.CommittedInstanceID() + q.CommittedGeometryIndex()];
    const float2 bc = q.CommittedTriangleBarycentrics();
    const ByteAddressBuffer buf = Attr[NonUniformResourceIndex(g.attr)];
    const uint3  i = buf.Load3(g.idxOffset + q.CommittedPrimitiveIndex() * 12);
    if (g.prevAttr != 0xFFFFFFFFu) {
        const ByteAddressBuffer old = Attr[NonUniformResourceIndex(g.prevAttr)];
        const float3 p0 = asfloat(old.Load3(g.prevPosOffset + i.x * 12));
        const float3 p1 = asfloat(old.Load3(g.prevPosOffset + i.y * 12));
        const float3 p2 = asfloat(old.Load3(g.prevPosOffset + i.z * 12));
        wasAt = p0 + (p1 - p0) * bc.x + (p2 - p0) * bc.y;
    } else if (g.moved != 0u) {
        const float3 p0 = asfloat(buf.Load3(g.posOffset + i.x * 12));
        const float3 p1 = asfloat(buf.Load3(g.posOffset + i.y * 12));
        const float3 p2 = asfloat(buf.Load3(g.posOffset + i.z * 12));
        const float4 o = float4(p0 + (p1 - p0) * bc.x + (p2 - p0) * bc.y, 1);
        wasAt = float3(dot(g.prevT[0], o), dot(g.prevT[1], o), dot(g.prevT[2], o));
    } else return false;
    return dot(wasAt - wp, wasAt - wp) < 9.0;
}

#if RESTIR
#include "RtxdiBridge.hlsli"
#endif

[numthreads(8, 8, 1)]
void CSTrace(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= RtSize)) return;
    const uint2 px = id.xy;
    const uint2 src = RtSrc(px);
    float d = Depth[src];
    float3 restirWasAt = 0;

    {
        float3 wp   = WorldFromDepth(src, min(d, 0.9999));
        float3 wasAt = wp;
        if (d < 0.99999 && d > 0.0) {
            const float matNow = GAlbedo[src].a;
            MovedObject(wp, matNow > 0.75 && matNow < 0.9, wasAt);
        }
        restirWasAt = wasAt;
        float4 prev = mul(PrevViewProj, float4(wasAt, 1));
        float2 uvP  = prev.xy / max(prev.w, 1e-4) * float2(0.5, -0.5) + 0.5;
        OutMotion[px] = uvP - float2(src) / float2(Size);
    }

    if (d >= 0.99999 || d <= 0.0) {
#if RESTIR
        if (RestirInfo.z > 0.5) RestirSurfaces[RestirSurfaceIndex(int2(px), RestirCurrentArray())] = RAB_EmptySurface();
#endif
        OutViewZ[px]    = ProjParams.z * 2.0;
        OutNormal[px]   = NRD_FrontEnd_PackNormalAndRoughness(float3(0, 0, 1), 1.0, 0);
        OutPenumbra[px] = NRD_FP16_MAX;
        OutDiff[px]     = 0;
        OutSpec[px]     = 0;
        return;
    }

    float3 P     = WorldFromDepth(src, d);
    float  viewZ = ViewZFromDepth(d);
    float4 g     = GNormal[src];
    float3 N;
    float  rough;
    const float3 gN = g.xyz * 2 - 1;
    if ((g.w > 0.0 || any(g.xyz > 0.0)) && dot(gN, gN) > 0.05) {
        N     = normalize(gN);
        rough = clamp(g.w, 0.05, 1.0);
        if (IsMirror(GAlbedo[src].a)) rough = 0.0;
    } else {
        uint2 r = uint2(min(src.x + 1, Size.x - 1), src.y), u = uint2(src.x, min(src.y + 1, Size.y - 1));
        const float3 c = cross(WorldFromDepth(u, Depth[u]) - P, WorldFromDepth(r, Depth[r]) - P);
        N     = dot(c, c) > 1e-12 ? normalize(c) : normalize(CameraPos.xyz - P);
        rough = 1.0;
    }
    float3 V = normalize(CameraPos.xyz - P);
    const bool character = GAlbedo[src].a > 0.75 && GAlbedo[src].a < 0.9;
    if (!character) N = TrustedNormal(N, src, P, V);

    float  dist   = distance(CameraPos.xyz, P);
    gRayMin = 0.0005;
    const float  matClass = GAlbedo[src].a;
    float ngBest;
    const float3 Ng = ((matClass > 0.42 && matClass < 0.75) || character) ? N : CornerNormal(src, P, V, N, ngBest);
    if (character) gRayMask = 0x01;
    float3 origin = P + Ng * (0.02 + dist * 0.0015) + V * (0.02 + dist * 0.002);
    uint   seed   = Hash(px.x * 1973u + px.y * 9277u + ((character && !RR_ACTIVE) ? 0u : (asuint(ProjParams.w) % max((uint)Osd.z, 1u)) * 26699u));
    float3 T, B;
    Basis(N, T, B);

    OutViewZ[px]  = viewZ;
    OutNormal[px] = NRD_FrontEnd_PackNormalAndRoughness(N, rough, 0);

    float NoL = dot(N, SunDir.xyz);
    float penumbra = 0;
    const bool isPane = matClass > 0.398 && matClass < 0.42;
    if (isPane) {
        if (OPT_SUN && NoL < 0) penumbra = SIGMA_FrontEnd_PackPenumbra(TraceAnyFace(P - N * 0.4, SunDir.xyz, 1000.0, -N), SunDir.w);
    } else if (OPT_SUN && NoL > 0 && dot(Ng, SunDir.xyz) > 0) {
        float3 st, sb;
        Basis(SunDir.xyz, st, sb);
        const float frame8 = (float)(asuint(ProjParams.w) & 7u);
        const float ign = frac(52.9829189 * frac(dot(float2(px) + 5.588238 * frame8, float2(0.06711056, 0.00583715))));
        const float sunA = RR_ACTIVE ? Rand(seed) : ign, sunB = RR_ACTIVE ? Rand(seed) : frac(ign * 1.61803 + 0.5);
        float r = character ? 0.0 : sqrt(sunA) * SunDir.w, a = 6.2831853 * sunB;
        float3 L = normalize(SunDir.xyz + st * (r * cos(a)) + sb * (r * sin(a)));
        penumbra = SIGMA_FrontEnd_PackPenumbra(TraceFromFloor(origin, L, 1000.0, Ng), SunDir.w);
    }
    OutPenumbra[px] = Valid(penumbra);

    {
        const int giSamples = OPT_LIGHTING == 0 ? 0 : (character ? 8 : (OPT_LIGHTING == 3 ? 2 : 1));
#if RESTIR
        const bool useRestir = RestirInfo.z > 0.5;
        const float3 restirLight = useRestir ? RestirDirect(px, origin, Ng, restirWasAt) : float3(0, 0, 0);
#else
        const bool useRestir = false;
        const float3 restirLight = 0;
#endif
        static const float3 kProbe[8] = {float3(0, 0, 1), float3(0, 0, -1), float3(0.894, 0, 0.447), float3(-0.894, 0, 0.447),
                                         float3(0, 0.894, 0.447), float3(0, -0.894, 0.447), float3(0.632, 0.632, -0.447), float3(-0.632, -0.632, -0.447)};
        float weightSum = 0;
        const float secondBounce = OPT_LIGHTING == 1 ? 0.0 : (OPT_LIGHTING == 2 ? 0.5 : 1.0);
        float3 radSum = 0;
        float  firstHit = 60.0;
        for (int giS = 0; giS < giSamples; ++giS) {
        float u1 = Rand(seed), u2 = Rand(seed);
        float r = sqrt(u1), phi = 6.2831853 * u2;
        float3 dir = normalize(T * (r * cos(phi)) + B * (r * sin(phi)) + N * sqrt(max(0, 1 - u1)));
        float sampleWeight = 1.0;
        if (character) {
            const uint  ph = Hash(src.x * 7919u + src.y * 104729u + (RR_ACTIVE ? asuint(ProjParams.w) * 977u : 0u));
            const float c1 = frac((float)giS / 8.0 + (ph & 0xFFFF) / 65536.0 / 8.0);
            const float c2 = frac((float)giS * 0.618034 + (ph >> 16) / 65536.0);
            const float cr = sqrt(c1), cp = 6.2831853 * c2;
            dir = normalize(T * (cr * cos(cp)) + B * (cr * sin(cp)) + N * sqrt(max(0, 1 - c1)));
        }
        const Hit hit = TraceLayered(origin, dir, 60.0, Ng);
        const bool isHit = hit.t < NRD_FP16_MAX;
        float3 hitLight = 0;
        bool   fromCache = false;
        if (isHit) {
            if (OPT_LIGHTING >= 2) {
                const SharcParameters sharc = BuildSharc();
                SharcHitData sharcHit;
                sharcHit.positionWorld = origin + dir * hit.t;
                sharcHit.normalWorld   = hit.N;
                const float voxel = HashGridGetVoxelSize(HashGridGetLevel(sharcHit.positionWorld, sharc.hashGridParameters), sharc.hashGridParameters);
                fromCache = hit.t > voxel * 1.7320508 && SharcGetCachedRadiance(sharc, sharcHit, hitLight, false);
            }
            if (!fromCache) {
                const float3 hp = origin + dir * hit.t;
                hitLight = ShadeHit(hp, hit) + (character ? float3(0, 0, 0) : hit.albedo * SampleDirect(hp + hit.N * 0.03, hit.N, hit.N, seed));
            }
            if (!character && AoInfo.x > 0.0) {
                const float near = (1.0 - saturate(hit.t / AoInfo.x)) * AoInfo.y;
                if (near > 0.0) hitLight = max(hitLight - hit.albedo * RoomAmbientAt(origin + dir * hit.t) * near, 0.0);
            }
        }
        float3 ind = isHit ? hitLight : (character ? Sky(dir) : OutsideLight(dir, hit));
        const float3 directHere = character ? SamplePortals(origin, N, Ng, seed) : (useRestir ? restirLight + SampleFixtures(origin, N, Ng, seed) : SampleDirect(origin, N, Ng, seed));
        float3 rad = 0;
        if (!character && isHit && !fromCache && secondBounce > 0.0 && Rand(seed) < secondBounce) {
            const float3 p1 = origin + dir * hit.t + hit.N * 0.03;
            float3 t1, b1;
            Basis(hit.N, t1, b1);
            const float v1 = Rand(seed), v2 = Rand(seed);
            const float r1 = sqrt(v1), ph1 = 6.2831853 * v2;
            const float3 dir2 = normalize(t1 * (r1 * cos(ph1)) + b1 * (r1 * sin(ph1)) + hit.N * sqrt(max(0, 1 - v1)));
            const Hit hit2 = Trace(p1, dir2, 40.0);
            float3 in2 = 0;
            if (hit2.t < NRD_FP16_MAX) {
                const float3 hp2 = p1 + dir2 * hit2.t;
                bool cached2 = false;
                if (OPT_LIGHTING >= 2) {
                    const SharcParameters sharc2 = BuildSharc();
                    SharcHitData sh2;
                    sh2.positionWorld = hp2;
                    sh2.normalWorld   = hit2.N;
                    const float voxel2 = HashGridGetVoxelSize(HashGridGetLevel(hp2, sharc2.hashGridParameters), sharc2.hashGridParameters);
                    cached2 = hit2.t > voxel2 * 1.7320508 && SharcGetCachedRadiance(sharc2, sh2, in2, false);
                }
                if (!cached2) in2 = ShadeHit(hp2, hit2) + hit2.albedo * SampleDirect(hp2 + hit2.N * 0.03, hit2.N, hit2.N, seed);
            } else {
                in2 = OutsideLight(dir2, hit2);
            }
            ind += hit.albedo * in2 / secondBounce;
        }
        if (character && isHit && !fromCache) {
            const float3 ph = origin + dir * hit.t + hit.N * 0.03;
            rad += hit.albedo * (SampleLights(ph, hit.N, hit.N, seed) + SampleFixtures(ph, hit.N, hit.N, seed));
        }
        if (!character) {
            const float indLum = dot(ind, float3(0.3, 0.6, 0.1));
            const float ceilingLum = kFireflyCeiling;
            if (indLum > ceilingLum) ind *= ceilingLum / indLum;
        }
        rad += ind + directHere;
        radSum += (character ? min(rad, 1.5) : rad) * sampleWeight;
        weightSum += sampleWeight;
        if (giS == 0 && isHit) firstHit = hit.t;
        }
        const float3 bounced = radSum / max(weightSum, 1e-3);
        const float  bouncedLum = dot(bounced, float3(0.3, 0.6, 0.1));
        OutDiff[px] = REBLUR_FrontEnd_PackRadianceAndNormHitDist(Valid(max(lerp(bouncedLum.xxx, bounced, Bounce.x > 0.0 ? Bounce.x : 1.0), 0.0) + (character ? float3(0, 0, 0) : FixtureGlowAt(origin)) + (character && OPT_LIGHTING != 0 ? CharacterLamps(origin, N) + CharacterFixtures(origin, N) : float3(0, 0, 0))), REBLUR_FrontEnd_GetNormHitDist(firstHit, viewZ, HitDistParams, 1.0), true);
    }

    if (rough >= 0.6 || !OPT_REFLECTIONS || character) {
        OutSpec[px] = REBLUR_FrontEnd_PackRadianceAndNormHitDist(0, REBLUR_FrontEnd_GetNormHitDist(200.0, viewZ, HitDistParams, rough), true);
    } else {
        float  a2 = rough * rough;
        float  u1 = Rand(seed), u2 = Rand(seed);
        float  cosT = sqrt((1 - u1) / (1 + (a2 * a2 - 1) * u1)), sinT = sqrt(1 - cosT * cosT), ph = 6.2831853 * u2;
        float3 H = normalize(T * (sinT * cos(ph)) + B * (sinT * sin(ph)) + N * cosT);
        float3 R = reflect(-V, H);
        float3 rad = 0;
        float  dist2 = 0;
        if (dot(R, N) > 0) {
            const Hit hit = TraceLayered(origin, R, 200.0, Ng);
            const bool isHit = hit.t < NRD_FP16_MAX;
            dist2 = isHit ? hit.t : 200.0;
            if (isHit) {
                const float3 hp = origin + R * hit.t + hit.N * 0.03;
                const float  ambient = asfloat(Expo.Load(44));
                rad = ShadeHit(hp, hit) + hit.albedo * (SampleLights(hp, hit.N, hit.N, seed) + SampleFixtures(hp, hit.N, hit.N, seed) + (ambient > 0.0 ? ambient : 0.0));
            } else {
                rad = Sky(R);
            }
        }
        OutSpec[px] = REBLUR_FrontEnd_PackRadianceAndNormHitDist(min(Valid(rad), IsMirror(GAlbedo[src].a) ? 16.0 : 2.0), REBLUR_FrontEnd_GetNormHitDist(dist2, viewZ, HitDistParams, rough), true);
    }
}

float3 AirAt(uint2 px, float d, inout float farShaft)
{
    const float  dist = (d < 0.99999 && d > 0.0) ? length(WorldFromDepth(px, d) - CameraPos.xyz) : kAirFar;
    const float2 f = (float2(px) + 0.5) / float2(Size) * float2(kAirW, kAirH) - 0.5;
    const float  sl = log(max(dist, kAirNear) / kAirNear) / log(kAirFar / kAirNear) * (float)kAirD - 1.0;
    const float3 g = float3(clamp(f, float2(0, 0), float2(kAirW - 1, kAirH - 1)), clamp(sl, 0.0, (float)kAirD - 1.0));
    const int3   b = int3(g);
    const int3   e = min(b + 1, int3(kAirW - 1, kAirH - 1, kAirD - 1));
    const float3 w = g - float3(b);
    const float4 c00 = lerp(AirGrid[uint3(b.x, b.y, b.z)], AirGrid[uint3(e.x, b.y, b.z)], w.x);
    const float4 c10 = lerp(AirGrid[uint3(b.x, e.y, b.z)], AirGrid[uint3(e.x, e.y, b.z)], w.x);
    const float4 c01 = lerp(AirGrid[uint3(b.x, b.y, e.z)], AirGrid[uint3(e.x, b.y, e.z)], w.x);
    const float4 c11 = lerp(AirGrid[uint3(b.x, e.y, e.z)], AirGrid[uint3(e.x, e.y, e.z)], w.x);
    float4 v = lerp(lerp(c00, c10, w.y), lerp(c01, c11, w.y), w.z);
    if (sl < 0.0) v.rgb *= saturate(sl + 1.0);
    farShaft = saturate(v.a);
    const float cap = lerp(0.6, 0.35 * (Bounce.y > 0.0 ? Bounce.y : 1.0), saturate(asfloat(Expo.Load(52))));
    const float al  = dot(v.rgb, float3(0.3, 0.6, 0.1));
    return al > 1e-4 ? v.rgb * cap * (1.0 - exp(-al / cap)) / al : v.rgb;
}

float AirTransAt(uint2 px, float d)
{
    const float  dist = (d < 0.99999 && d > 0.0) ? length(WorldFromDepth(px, d) - CameraPos.xyz) : kAirFar;
    const float2 f = (float2(px) + 0.5) / float2(Size) * float2(kAirW, kAirH) - 0.5;
    const float  sl = log(max(dist, kAirNear) / kAirNear) / log(kAirFar / kAirNear) * (float)kAirD - 1.0;
    const float3 g = float3(clamp(f, float2(0, 0), float2(kAirW - 1, kAirH - 1)), clamp(sl, 0.0, (float)kAirD - 1.0));
    const int3   b = int3(g);
    const int3   e = min(b + 1, int3(kAirW - 1, kAirH - 1, kAirD - 1));
    const float3 w = g - float3(b);
    const float t00 = lerp(AirTrans[uint3(b.x, b.y, b.z)].r, AirTrans[uint3(e.x, b.y, b.z)].r, w.x);
    const float t10 = lerp(AirTrans[uint3(b.x, e.y, b.z)].r, AirTrans[uint3(e.x, e.y, b.z)].r, w.x);
    const float t01 = lerp(AirTrans[uint3(b.x, b.y, e.z)].r, AirTrans[uint3(e.x, b.y, e.z)].r, w.x);
    const float t11 = lerp(AirTrans[uint3(b.x, e.y, e.z)].r, AirTrans[uint3(e.x, e.y, e.z)].r, w.x);
    const float t = lerp(lerp(t00, t10, w.y), lerp(t01, t11, w.y), w.z);
    return sl < 0.0 ? lerp(1.0, t, saturate(sl + 1.0)) : saturate(t);
}

static const uint  kHazeDirs = 12;
static const uint  kHazeAt   = 768;
static const float kHazeZ    = 0.07;
float3 HazeRing(uint k) { return asfloat(Expo.Load3(kHazeAt + (k % kHazeDirs) * 12)); }
float3 HazeColor(float3 horizonDir)
{
    const float3 air = AtmosphereColor(horizonDir);
    if (Atmosphere.x < 0.5) return air;
    const float  a = atan2(horizonDir.y, horizonDir.x) / 6.2831853 * kHazeDirs;
    const float  af = a - floor(a);
    const uint   k0 = (uint)((int)floor(a) + (int)kHazeDirs * 4) % kHazeDirs;
    const float3 ring = lerp(HazeRing(k0), HazeRing(k0 + 1), af);
    return dot(ring, 1.0) > 1e-4 ? ring : air;
}
float3 HazeDir(uint k)
{
    const float a = 6.2831853 * k / kHazeDirs;
    return normalize(float3(cos(a), sin(a), kHazeZ));
}

float3 DistanceFog(float3 color, float3 P, float shaft)
{
    if (Fog.y <= 0.0) return color;
    const float3 toP  = P - CameraPos.xyz;
    const float  dist = length(toP);
    if (dist <= Fog.x) return color;
    const float3 horizonDir = normalize(float3(toP.xy, max(length(toP.xy) * 0.02, 1e-3)));
    const float  height = exp(-max(P.z - CameraPos.z, 0.0) * 0.004);
    const float  amount = (1.0 - exp(-(dist - Fog.x) * Fog.y)) * height * (1.0 - saturate(asfloat(Expo.Load(52))));
    return lerp(color, HazeColor(horizonDir) * lerp(Fog.z, 1.0, shaft), saturate(amount));
}

float3 CharacterLight(uint2 px, float3 fallback)
{
    const float2 lp   = (float2(px) - float2(RtJitter.xy)) / RtScale;
    const int2   base = int2(floor(lp));
    const float2 f    = lp - float2(base);
    float3 sum = 0;
    float  n = 0;
    for (int j = -3; j <= 4; ++j)
        for (int i = -3; i <= 4; ++i) {
            const uint2 l = uint2(clamp(base + int2(i, j), int2(0, 0), int2(RtSize) - 1));
            const float a = GAlbedo[RtSrc(l)].a;
            if (a <= 0.75 || a >= 0.9) continue;
            const float2 dd = float2(i, j) - f;
            const float  w  = exp(-dot(dd, dd) * 0.12);
            sum += w * Valid(REBLUR_BackEnd_UnpackRadianceAndNormHitDist(OutDiff[l]).rgb);
            n += w;
        }
    return n > 1e-3 ? sum / n : fallback;
}

void UpsampleLight(uint2 px, float3 P, float3 N, float dist, float tolScale, out float shadow, out float3 diff, out float3 spec)
{
    if (RtScale <= 1) {
        shadow = saturate(Valid(SIGMA_BackEnd_UnpackShadow(SigmaOut[px])));
        diff   = Valid(REBLUR_BackEnd_UnpackRadianceAndNormHitDist(DiffOut[px]).rgb);
        spec   = Valid(REBLUR_BackEnd_UnpackRadianceAndNormHitDist(SpecOut[px]).rgb);
        return;
    }
    const float2 lp   = (float2(px) - float2(RtJitter.xy)) / RtScale;
    const int2   base = int2(floor(lp));
    const float2 f    = lp - float2(base);
    const float  tol  = (0.04 + dist * 0.004) * tolScale;
    float  sum = 0, sh = 0, bestD = 1e30;
    float3 di = 0, sp = 0;
    uint2  nearest = uint2(clamp(base, int2(0, 0), int2(RtSize) - 1));
    for (int j = 0; j < 2; ++j)
        for (int i = 0; i < 2; ++i) {
            const uint2 l   = uint2(clamp(base + int2(i, j), int2(0, 0), int2(RtSize) - 1));
            const uint2 s   = RtSrc(l);
            const float ds  = Depth[s];
            const float pd  = (ds >= 0.99999 || ds <= 0.0) ? 1e20 : abs(dot(WorldFromDepth(s, ds) - P, N));
            if (pd < bestD) { bestD = pd; nearest = l; }
            const float w = (i ? f.x : 1.0 - f.x) * (j ? f.y : 1.0 - f.y) * saturate(1.0 - pd / tol);
            if (w <= 1e-4) continue;
            sum += w;
            sh  += w * saturate(Valid(SIGMA_BackEnd_UnpackShadow(SigmaOut[l])));
            di  += w * Valid(REBLUR_BackEnd_UnpackRadianceAndNormHitDist(DiffOut[l]).rgb);
            sp  += w * Valid(REBLUR_BackEnd_UnpackRadianceAndNormHitDist(SpecOut[l]).rgb);
        }
    if (sum < 0.6) {
        sum = 0; sh = 0; di = 0; sp = 0;
        for (int j = -1; j <= 2; ++j)
            for (int i = -1; i <= 2; ++i) {
                const uint2 l   = uint2(clamp(base + int2(i, j), int2(0, 0), int2(RtSize) - 1));
                const uint2 s   = RtSrc(l);
                const float ds  = Depth[s];
                const float pd  = (ds >= 0.99999 || ds <= 0.0) ? 1e20 : abs(dot(WorldFromDepth(s, ds) - P, N));
                const float2 dd = float2(i, j) - f;
                const float w   = exp(-dot(dd, dd) * 0.9) * saturate(1.0 - pd / tol);
                if (w <= 1e-4) continue;
                sum += w;
                sh  += w * saturate(Valid(SIGMA_BackEnd_UnpackShadow(SigmaOut[l])));
                di  += w * Valid(REBLUR_BackEnd_UnpackRadianceAndNormHitDist(DiffOut[l]).rgb);
                sp  += w * Valid(REBLUR_BackEnd_UnpackRadianceAndNormHitDist(SpecOut[l]).rgb);
            }
    }
    if (sum > 1e-3) {
        shadow = sh / sum; diff = di / sum; spec = sp / sum;
    } else {
        shadow = saturate(Valid(SIGMA_BackEnd_UnpackShadow(SigmaOut[nearest])));
        diff   = Valid(REBLUR_BackEnd_UnpackRadianceAndNormHitDist(DiffOut[nearest]).rgb);
        spec   = Valid(REBLUR_BackEnd_UnpackRadianceAndNormHitDist(SpecOut[nearest]).rgb);
    }
}

[numthreads(8, 8, 1)]
void CSComposite(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= Size)) return;
    const uint2  px    = id.xy;
    const uint2  src   = px;
    if (Osd.w > 0.0 && px.y >= 44 && px.y < 44 + 33 && px.x >= 14 && px.x < 26 + (uint)Osd.w * 18) {
        const int2 q = int2(px) - int2(20, 50);
        float3 tc = float3(0.04, 0.04, 0.05);
        if (q.x >= 0 && q.y >= 0 && q.y < 21) {
            const uint ci = (uint)q.x / 18u;
            const uint ch = ci < 32u ? OsdText[ci / 4u][ci % 4u] : 0u;
            const uint gx = ((uint)q.x % 18u) / 3u, gy = (uint)q.y / 3u;
            if (gx < 5u && ch >= 32u && ch < 96u) {
                const uint2 g = kFont[ch - 32u];
                const uint bit = gy * 5u + gx;
                if (((bit < 32u ? g.x >> bit : g.y >> (bit - 32u)) & 1u) != 0u) tc = float3(1.0, 0.95, 0.4);
            }
        }
        OutFinal[px] = float4(tc, 1.0);
        return;
    }
    if (Osd.x > 0.0 && px.y >= 20 && px.y < 36 && px.x >= 20 && px.x < 20 + 5 * 52) {
        const uint slot = (px.x - 20) / 52;
        if ((px.x - 20) % 52 < 44) {
            float3 oc = float3(0.85, 0.15, 0.15);
            if (slot == 0 && OPT_RT) oc = float3(0.2, 0.85, 0.3);
            if (slot == 1 && OPT_REFLECTIONS) oc = float3(0.2, 0.85, 0.3);
            if (slot == 2 && OPT_SUN) oc = float3(0.2, 0.85, 0.3);
            if (slot == 3 && OPT_LOCAL) oc = float3(0.2, 0.85, 0.3);
            if (slot == 4 && OPT_LIGHTING == 1) oc = float3(0.95, 0.85, 0.2);
            if (slot == 4 && OPT_LIGHTING == 2) oc = float3(0.2, 0.85, 0.3);
            if (slot == 4 && OPT_LIGHTING == 3) oc = float3(0.2, 0.85, 0.95);
            OutFinal[px] = float4(oc, 1.0);
            return;
        }
    }
    if (!OPT_RT) {
        OutFinal[px] = SceneColor[px];
        return;
    }
    const float4 scene = SceneColor[src];
    const float4 g     = GNormal[src];
    const float  d     = Depth[src];
    const bool hasSurface = g.w > 0.0 || any(g.xyz > 0.0);
    if (DebugMode == 8) {
        float shaftUnused = 1.0;
        OutFinal[px] = float4(saturate(AirAt(px, d, shaftUnused) * 2.0), 1.0);
        if (px.y < 24u && px.x < 48u) OutFinal[px] = px.x < 24u ? float4(saturate(asfloat(Expo.Load(52))), 0, 0, 1) : float4(0, saturate(asfloat(Expo.Load(56))), 0, 1);
        return;
    }
    if (!hasSurface) {
        if (Atmosphere.x < 0.5 || Strength.x <= 0.0) {
            OutFinal[px] = scene;
            return;
        }
        const float3 sky   = SkyImage(normalize(WorldFromDepth(px, 0.5) - CameraPos.xyz), px);
        const float3 gSky  = GOpaque[src].rgb;
        const float3 delta = scene.rgb - gSky;
        const float3 hidden = 1.0 - scene.rgb / max(gSky, 0.02);
        const float  cov    = saturate(max(hidden.r, max(hidden.g, hidden.b)) + max(delta.r, max(delta.g, delta.b)) * 4.0);
        const float  lum   = dot(SunColor.rgb, float3(0.3, 0.6, 0.1)) * 0.6 + dot(Sky(float3(0, 0, 1)), float3(0.3, 0.6, 0.1)) * 1.2;
        const float  level = clamp(lum, 0.06, 1.0);
        float dObject = (d < 0.99999 && d > 0.0) ? d : 1.0;
        if (dObject >= 1.0 && cov > 0.01) {
            static const int2 kAround[8] = {int2(1, 0), int2(-1, 0), int2(0, 1), int2(0, -1), int2(1, 1), int2(-1, 1), int2(1, -1), int2(-1, -1)};
            for (int ring = 1; ring <= 3 && dObject >= 1.0; ++ring)
                for (int k = 0; k < 8; ++k) {
                    const float dn = Depth[uint2(clamp(int2(src) + kAround[k] * ring, int2(0, 0), int2(Size) - 1))];
                    if (dn < 0.99999 && dn > 0.0) dObject = min(dObject, dn);
                }
        }
        const bool  hasObject = dObject < 1.0;
        float       unusedShaft = 1.0;
        const float3 airSky = OPT_VOLUMETRIC ? AirAt(px, d, unusedShaft) : float3(0, 0, 0);
        float3 object = max(scene.rgb - gSky * (1.0 - cov), 0.0) * level / max(cov, 0.05);
        float3 airObject = airSky * 0.25;
        if (hasObject) {
            object = DistanceFog(object, WorldFromDepth(px, dObject), 1.0);
            if (OPT_VOLUMETRIC) airObject = AirAt(px, dObject, unusedShaft);
        }
        float3 noSurface = lerp(sky + airSky, object + airObject, cov);
        noSurface *= WindowLight.w > 0.0 ? WindowLight.w : 1.0;
        {
            const float kShoulderS = 0.7;
            noSurface = lerp(noSurface, kShoulderS + (1.0 - kShoulderS) * (1.0 - exp(-(noSurface - kShoulderS) / (1.0 - kShoulderS))), step(kShoulderS, noSurface));
        }
        OutFinal[px] = float4(noSurface, scene.a);
        return;
    }
    if (d >= 0.99999 || d <= 0.0) {
        OutFinal[px] = scene;
        return;
    }
    const float3 gN = g.xyz * 2 - 1;
    const bool   hasN = dot(gN, gN) > 0.05;
    const float3 P0 = WorldFromDepth(src, d);
    float3 N = hasN ? normalize(gN) : normalize(CameraPos.xyz - P0);
    const float rough = hasN ? clamp(g.w, 0.05, 1.0) : 1.0;
    const float3 P = WorldFromDepth(src, d);
    const float3 V = normalize(CameraPos.xyz - P);
    const bool character = GAlbedo[src].a > 0.75 && GAlbedo[src].a < 0.9;
    if (!character) N = TrustedNormal(N, src, P, V);

    float  shadow;
    float3 indRaw, spec;
    UpsampleLight(px, P, N, distance(CameraPos.xyz, P), character ? 6.0 : 1.0, shadow, indRaw, spec);
    if (character) { indRaw = CharacterLight(px, indRaw); spec = 0; }
    if (OPT_SUN && RtScale > 1 && !character && dot(N, SunDir.xyz) > 0.0 && shadow > 0.02 && shadow < 0.98) {
        gRayMin = 0.0005;
        gRayMask = 0xFF;
        float ngBest;
        const float3 NgC = CornerNormal(src, P, V, N, ngBest);
        const float  distC = distance(CameraPos.xyz, P);
        const float3 oC = P + NgC * (0.02 + distC * 0.0015) + V * (0.02 + distC * 0.002);
        float3 st, sb;
        Basis(SunDir.xyz, st, sb);
        static const float2 disk[4] = {float2(0.35, 0.2), float2(-0.2, 0.35), float2(-0.35, -0.2), float2(0.2, -0.35)};
        const float rot = (px.x & 1u) != (px.y & 1u) ? -1.0 : 1.0;
        float visible = 0;
        if (dot(NgC, SunDir.xyz) > 0.0)
            for (int k = 0; k < 4; ++k) {
                const float3 L = normalize(SunDir.xyz + (st * disk[k].x + sb * disk[k].y * rot) * SunDir.w);
                if (TraceFromFloor(oC, L, 1000.0, NgC) >= NRD_FP16_MAX) visible += 0.25;
            }
        shadow = visible;
    }
    float3 direct = 0;
    if (shadow > 0.001 && !(GAlbedo[src].a > 0.398 && GAlbedo[src].a < 0.42)) direct = SunColor.rgb * saturate(dot(N, SunDir.xyz)) * shadow * CloudShadow(P);
    const float3 ind    = indRaw * Strength.z;
    const float  F0     = lerp(0.04, 0.08, saturate((0.5 - rough) / 0.35));
    const float  F      = (F0 + (1 - F0) * pow(1 - saturate(dot(N, V)), 5)) * (1 - rough);

    const float3 opaque = GOpaque[src].rgb;
    const float3 albedo = GAlbedo[src].rgb;
    const float  sunlit = saturate(dot(direct, float3(0.3, 0.6, 0.1)));
    const float  keep  = Strength.y + (GAlbedo[src].a < 0.75 ? 0.35 * (1.0 - sunlit) : 0.0);
    const float  rayLight = saturate(dot(direct + ind, float3(0.3, 0.6, 0.1)) * 2.0);
    const float  baked    = saturate(dot(opaque, float3(0.3, 0.6, 0.1)) / max(dot(albedo, float3(0.3, 0.6, 0.1)), 0.02));
    const float3 pools    = opaque * Atmosphere.z * Atmosphere.w * baked * baked * baked * (1.0 - rayLight);
    const float3 lit = albedo * min(direct + ind, 4.0) + opaque * keep + pools + spec * F * Strength.w;
    float3 over = scene.rgb - opaque;
    const float3 ratio = scene.rgb / max(opaque, 0.02);
    const float  rMin = min(ratio.r, min(ratio.g, ratio.b)), rMax = max(ratio.r, max(ratio.g, ratio.b));
    if (rMax < 0.985 && rMax - rMin < 0.08 && dot(opaque, 1.0) > 0.06) over = 0;
    OutFinal[px] = float4(opaque + over + (lit - opaque) * Strength.x * GOpaque[src].a, scene.a);
    if (FullRT != 0) {
        const float  lumL  = dot(SunColor.rgb, float3(0.3, 0.6, 0.1)) * 0.6 + dot(Sky(float3(0, 0, 1)), float3(0.3, 0.6, 0.1)) * 1.2;
        const float  leaf  = (GAlbedo[src].a > 0.42 && GAlbedo[src].a < 0.75) ? 1.0 : 0.0;
        const bool   pane  = GAlbedo[src].a > 0.398 && GAlbedo[src].a < 0.42;
        const float3 paneLight = pane ? Sky(-V) * 2.4 + SunColor.rgb * saturate(-dot(N, SunDir.xyz)) * shadow * 0.25 : float3(0, 0, 0);
        const float3 through = paneLight;
        const float  offShare = (OPT_SUN ? 0.0 : 0.5) + (OPT_LIGHTING != 0 ? 0.0 : 0.5);
        const float3 painted  = 0.0;
        const float  indoors  = saturate(asfloat(Expo.Load(52)));
        const float3 roomFill = 0.0;
        const float3 tracedLight = direct + ind + through + painted;
        const float  fillLum = dot(roomFill, float3(0.3, 0.6, 0.1));
        const float3 rayLight = tracedLight + roomFill * (fillLum > 1e-3 ? saturate(1.0 - dot(tracedLight, float3(0.3, 0.6, 0.1)) / fillLum) : 0.0);
        const float3 full  = albedo * min(rayLight, 4.0) + spec * F * Strength.w;
        const bool   blendedSurface = GOpaque[src].a < 0.99 || (GAlbedo[src].a > 0.92 && GAlbedo[src].a < 0.98);
        const float3 overlay = max(scene.rgb - opaque, 0.0);
        const float  overMax = max(overlay.r, max(overlay.g, overlay.b));
        const float  overSat = overMax > 1e-3 ? (overMax - min(overlay.r, min(overlay.g, overlay.b))) / overMax : 0.0;
        const bool   glossy  = N.z > 0.85 || rough <= 0.46;
        const float  overKeep = smoothstep(0.5, 0.8, overSat);
        const float  drawMark = GAlbedo[src].a;
        const float3 extra = (drawMark > 0.90 && drawMark < 0.91) ? overlay : float3(0, 0, 0);
        const bool  flatFloor = N.z > 0.85 && GOpaque[src].a >= 0.4;
        const float cover = (flatFloor || leaf > 0.5) ? 1.0 : smoothstep(0.3, 0.6, GOpaque[src].a);
        const float  matA = GAlbedo[src].a;
        const float  emit = (matA > 0.04 && matA < 0.394) ? (matA - 0.05) / 0.35 * 255.0 / 8.0 : 0.0;
        const float3 lightHere = min(rayLight, 4.0);
        const float3 rayImage  = albedo * lightHere;
        if (((px.x | px.y) & 3) == 0 && emit <= 0.0 && cover > 0.5 && all(isfinite(rayImage))) {
            uint prev;
            const uint3 tgt = (uint3)(saturate(opaque) * 255.0);
            const uint3 ray = (uint3)(min(rayImage, 4.0) * 255.0);
            Expo.InterlockedAdd(0, tgt.r, prev); Expo.InterlockedAdd(4, tgt.g, prev); Expo.InterlockedAdd(8, tgt.b, prev);
            Expo.InterlockedAdd(12, ray.r, prev); Expo.InterlockedAdd(16, ray.g, prev); Expo.InterlockedAdd(20, ray.b, prev);
            Expo.InterlockedAdd(24, 1u, prev);
            Expo.InterlockedAdd(48, (uint)(dot(lightHere, float3(0.3, 0.6, 0.1)) * 64.0), prev);
        }
        float3 gain = asfloat(Expo.Load3(32));
        if (!(gain.r > 0.0) || !(gain.g > 0.0) || !(gain.b > 0.0)) gain = 1.0;
        float3 c = (full + extra) * gain + albedo * emit;
        if (RR_ACTIVE) {
            float3 rrL = Valid(RROut[px].rgb);
            float3 surfaceAlbedo = albedo;
            if (GAlbedo[src].a > 0.42 && GAlbedo[src].a < 0.75) {
                surfaceAlbedo = saturate(albedo / max(GOpaque[src].a, 0.75));
                const float myZ = abs(ViewZFromDepth(d));
                for (int j = -1; j <= 1; ++j)
                    for (int i = -1; i <= 1; ++i) {
                        const uint2 q = uint2(clamp(int2(px) + int2(i, j) * 2, int2(0, 0), int2(Size) - 1));
                        const float dq = Depth[q];
                        if (dq >= 0.99999 || dq <= 0.0 || abs(abs(ViewZFromDepth(dq)) - myZ) > 0.5 + myZ * 0.05) continue;
                        const float3 l2 = Valid(RROut[q].rgb);
                        if (dot(l2, float3(0.3, 0.6, 0.1)) > dot(rrL, float3(0.3, 0.6, 0.1))) rrL = l2;
                    }
                const float openAir = 1.0 - saturate(asfloat(Expo.Load(52)));
                rrL = max(rrL, (Sky(float3(0, 0, 1)) * 0.7 + SunColor.rgb * 0.12) * openAir * (1.0 - Atmosphere.w));
            }
            if (drawMark >= 0.91 && drawMark < 0.92) surfaceAlbedo = saturate(albedo * clamp(scene.rgb / max(opaque, 0.02), 0.0, 1.6));
            if (IsMirror(GAlbedo[src].a)) surfaceAlbedo = 1.0;
            c = Pad2.y != 0u ? rrL : surfaceAlbedo * (rrL + emit);
        }
        c *= WindowLight.w > 0.0 ? WindowLight.w : 1.0;
        float farShaft = 1.0;
        float3 airHere = 0;
        if (OPT_VOLUMETRIC) { airHere = AirAt(px, d, farShaft); c = c * AirTransAt(px, d) + airHere; }
        const float kShoulder = 0.8;
        c = lerp(c, kShoulder + (1.0 - kShoulder) * (1.0 - exp(-(c - kShoulder) / (1.0 - kShoulder))), step(kShoulder, c));
        if (RR_ACTIVE) {
            const float3 glowHere = extra * (SharcInfo2.y > 0.0 ? SharcInfo2.y : 1.0);
            c = c * lerp(1.0, 0.3, smoothstep(0.02, 0.35, dot(glowHere, float3(0.3, 0.6, 0.1)))) + glowHere;
        }
        const float3 fringe = scene.rgb * clamp(dot(lightHere * gain, float3(0.3, 0.6, 0.1)), 0.05, 1.2) + airHere;
        gDebug9 = float3(cover, GOpaque[src].a, dot(c, float3(0.3, 0.6, 0.1)));
        OutFinal[px] = float4(DistanceFog(lerp(fringe, c, cover), P, farShaft), scene.a);
    }

    if (DebugMode != 0) {
        float3 o = 0;
        if (DebugMode == 1) o = OutPenumbra[px / RtScale] >= NRD_FP16_MAX * 0.5 ? 1.0 : 0.0;
        if (DebugMode == 2) o = RR_ACTIVE ? saturate(Valid(RROut[px].rgb) * 0.5) : shadow;
        if (DebugMode == 3) o = N * 0.5 + 0.5;
        if (DebugMode == 4) o = saturate(Valid(REBLUR_BackEnd_UnpackRadianceAndNormHitDist(OutDiff[px / RtScale]).rgb) * 0.5);
        if (DebugMode == 5) o = saturate(Valid(REBLUR_BackEnd_UnpackRadianceAndNormHitDist(OutDiff[px / RtScale]).rgb) * 0.5);
        if (DebugMode == 9) o = saturate(abs(OutMotion[px / RtScale].xyy) * 40.0);
        if (DebugMode == 9 && false) o = gDebug9;
        if (DebugMode == 6) o = albedo;
        if (DebugMode == 7) o = saturate((direct + ind) * 0.5);
        OutFinal[px] = float4(o, scene.a);
    }
}

[numthreads(1, 1, 1)]
void CSExposure(uint3 id : SV_DispatchThreadID)
{
    const uint n = Expo.Load(24);
    float3 gain = asfloat(Expo.Load3(32));
    if (!(gain.r > 0.0) || !(gain.g > 0.0) || !(gain.b > 0.0)) gain = 1.0;
    float ambient = asfloat(Expo.Load(44));
    if (n > 64) {
        const float3 meanT = float3(Expo.Load3(0)) / (float)n / 255.0;
        const float3 meanR = max(float3(Expo.Load3(12)) / (float)n / 255.0, 1e-3);
        const float3 lumW  = float3(0.3, 0.6, 0.1);
        const float  level = clamp(dot(meanT, lumW) / dot(meanR, lumW), Exposure.y, Exposure.z);
        const float3 tint  = clamp((meanT / meanR) / max(dot(meanT, lumW) / dot(meanR, lumW), 1e-3), 0.6, 1.6);
        float3 target = lerp(1.0, level * tint, Exposure.x);
        gain = lerp(gain, target, Exposure.w);
        ambient = lerp(ambient, (float)Expo.Load(48) / (float)n / 64.0, 0.05);
    }
    for (uint a = 0; a <= 24; a += 4) Expo.Store(a, 0u);
    Expo.Store(48, 0u);
    Expo.Store3(32, asuint(gain));
    Expo.Store(44, asuint(ambient));
    if (Atmosphere.x > 0.5) {
        for (uint hk = 0; hk < kHazeDirs; ++hk) {
            const float3 d = HazeDir(hk);
            const float4 c = MarchClouds(CameraPos.xyz, d, uint2(hk * 7u, 3u));
            const float3 now = AtmosphereColor(d) * c.a + c.rgb;
            const float3 was = HazeRing(hk);
            Expo.Store3(kHazeAt + hk * 12, asuint(dot(was, 1.0) > 1e-4 ? lerp(was, now, 0.1) : now));
        }
    }
    gRayMin = 0.0005;
    gGlassPortal = false;
    float closed = 0;
    for (int k = 0; k < 9; ++k) {
        const float a = 6.2831853 * k / 8.0;
        const float3 dk = k == 0 ? float3(0, 0, 1) : normalize(float3(cos(a), sin(a), 1.0));
        const Hit roofHit = TraceFaces(CameraPos.xyz, dk, 40.0, RAY_FLAG_NONE);
        if (roofHit.t < NRD_FP16_MAX || roofHit.glass) closed += 1.0;
    }
    const float roofNow = smoothstep(7.5, 9.0, closed);
    const float roof = asfloat(Expo.Load(52));
    Expo.Store(52, asuint(lerp(roof, roofNow, roofNow < roof ? 0.3 : 0.012)));
    const float outsideNow = 1.0 - roofNow, outsideWas = asfloat(Expo.Load(56));
    Expo.Store(56, asuint(lerp(outsideWas, outsideNow, outsideNow < outsideWas ? 0.3 : 0.012)));
    gRayMask = 0x01;
    uint seed = Hash(asuint(ProjParams.w) * 7919u + 12345u);
    const int lightCount = min((int)LightCount, 32);
    float lampWas[32];
    for (int lw = 0; lw < 32; ++lw) lampWas[lw] = LightVisibility(lw);
    float windowWas[128];
    for (int ww = 0; ww < 128; ++ww) windowWas[ww] = PortalVisibility(ww);
    for (int i = 0; i < lightCount; ++i) {
        const int was = LightNumberMapped((uint)i, true);
        float seen = 0;
        for (int k = 0; k < 8; ++k) {
            const uint2 px = min(uint2((uint)(Rand(seed) * (float)Size.x), (uint)(Rand(seed) * (float)Size.y)), Size - 1u);
            const float d = Depth[px];
            if (d >= 0.99999 || d <= 0.0) continue;
            const float3 gn = GNormal[px].xyz * 2.0 - 1.0;
            if (dot(gn, gn) < 0.05) continue;
            const float3 N = normalize(gn);
            const float3 P = WorldFromDepth(px, d) + N * 0.05;
            float3 l; float dist;
            if (LightWeight(i, P, N, l, dist) <= 0.0) continue;
            if (TraceAnyFace(P, l, max(dist - 0.6, 0.01), N) >= NRD_FP16_MAX) seen += 1.0;
        }
        Expo.Store(64 + i * 4, asuint(was >= 0 && was < 32 ? lerp(lampWas[was], seen / 8.0, 0.1) : seen / 8.0));
    }
    gGlassPortal = true;
    const int windowCount = min((int)PortalInfo.x, 128);
    for (int i = 0; i < windowCount; ++i) {
        const int was = LightNumberMapped(32u + (uint)i, true) - 32;
        float seen = 0;
        for (int k = 0; k < 4; ++k) {
            const uint2 px = min(uint2((uint)(Rand(seed) * (float)Size.x), (uint)(Rand(seed) * (float)Size.y)), Size - 1u);
            const float d = Depth[px];
            if (d >= 0.99999 || d <= 0.0) continue;
            const float3 gn = GNormal[px].xyz * 2.0 - 1.0;
            if (dot(gn, gn) < 0.05) continue;
            const float3 N = normalize(gn);
            const float3 P = WorldFromDepth(px, d) + N * 0.05;
            const float3 x = PortalC[i].xyz + PortalU[i].xyz * (Rand(seed) * 2.0 - 1.0) + PortalV[i].xyz * (Rand(seed) * 2.0 - 1.0);
            const float3 to = x - P;
            const float  dist = length(to);
            if (dist < 0.01 || dot(N, to) <= 0.0) continue;
            if (TraceAnyFace(P, to / dist, max(dist - 0.05, 0.01), N) >= NRD_FP16_MAX) seen += 1.0;
        }
        Expo.Store(192 + i * 4, asuint(was >= 0 && was < 128 ? lerp(windowWas[was], seen / 4.0, 0.1) : seen / 4.0));
    }
}

[numthreads(8, 8, 1)]
void CSNoisy(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= RtSize)) return;
    const uint2  lp = id.xy;
    const uint2  px = RtSrc(lp);
    const float  d  = Depth[px];
    const float4 g  = GNormal[px];
    OutRRMv[lp] = OutMotion[lp] * float2(RtSize);
    const bool hasSurface = (g.w > 0.0 || any(g.xyz > 0.0)) && d < 0.99999 && d > 0.0;
    if (!hasSurface) {
        float3 fill = 0;
        float  fw = 0;
        for (int j = -1; j <= 1; ++j)
            for (int i = -1; i <= 1; ++i) {
                const uint2 l  = uint2(clamp(int2(lp) + int2(i, j), int2(0, 0), int2(RtSize) - 1));
                const uint2 s  = RtSrc(l);
                const float ds = Depth[s];
                if (ds >= 0.99999 || ds <= 0.0) continue;
                const float litN = OutPenumbra[l] >= NRD_FP16_MAX * 0.5 ? 1.0 : 0.0;
                fill += Valid(REBLUR_BackEnd_UnpackRadianceAndNormHitDist(OutDiff[l]).rgb) * Strength.z + SunColor.rgb * litN * 0.6;
                fw += 1;
            }
        OutRRColor[lp] = float4(fw > 0 ? fill / fw : Sky(float3(0, 0, 1)), 1);
        OutRRDiff[lp] = Pad2.y != 0u ? float4(0.5, 0.5, 0.5, 1) : float4(1, 1, 1, 1);
        OutRRSpec[lp] = 0; OutRRNormal[lp] = float4(0, 0, 1, 1);
        return;
    }
    const float3 P = WorldFromDepth(px, d);
    const float3 V = normalize(CameraPos.xyz - P);
    const float3 gN = g.xyz * 2 - 1;
    float3 N = dot(gN, gN) > 0.05 ? normalize(gN) : V;
    const float  matA  = GAlbedo[px].a;
    const bool   mirror = IsMirror(matA);
    const float  rough = mirror ? 0.02 : clamp(g.w, 0.05, 1.0);
    const bool   character = matA > 0.75 && matA < 0.9;
    const bool   pane = matA > 0.398 && matA < 0.42;
    if (!character) N = TrustedNormal(N, px, P, V);
    const float3 albedo = GAlbedo[px].rgb;
    const float  lit = OutPenumbra[lp] >= NRD_FP16_MAX * 0.5 ? 1.0 : 0.0;
    const float3 direct = (OPT_SUN && !pane) ? SunColor.rgb * saturate(dot(N, SunDir.xyz)) * lit * CloudShadow(P) : float3(0, 0, 0);
    float3 ind = Valid(REBLUR_BackEnd_UnpackRadianceAndNormHitDist(OutDiff[lp]).rgb) * Strength.z;
    {
        float most = 0;
        for (int j = -1; j <= 1; ++j)
            for (int i = -1; i <= 1; ++i) {
                if (i == 0 && j == 0) continue;
                const uint2 l = uint2(clamp(int2(lp) + int2(i, j), int2(0, 0), int2(RtSize) - 1));
                most = max(most, dot(Valid(REBLUR_BackEnd_UnpackRadianceAndNormHitDist(OutDiff[l]).rgb), float3(0.3, 0.6, 0.1)) * Strength.z);
            }
        const float limit = most * 2.0 + 0.05;
        const float level = dot(ind, float3(0.3, 0.6, 0.1));
        const float moving = saturate(length(OutMotion[lp] * float2(RtSize)) * 2.0 - 0.1);
        if (level > limit) ind *= lerp(1.0, limit / level, moving);
    }
    const float3 spec = Valid(REBLUR_BackEnd_UnpackRadianceAndNormHitDist(OutSpec[lp]).rgb);
    const float  F0 = lerp(0.04, 0.08, saturate((0.5 - rough) / 0.35));
    const float  F  = (F0 + (1 - F0) * pow(1 - saturate(dot(N, V)), 5)) * (1 - rough);
    const float3 paneLight = pane ? Sky(-V) * 2.4 + SunColor.rgb * saturate(-dot(N, SunDir.xyz)) * lit * 0.25 : float3(0, 0, 0);
    const float  emit = (matA > 0.04 && matA < 0.394) ? (matA - 0.05) / 0.35 * 255.0 / 8.0 : 0.0;
    const bool   withAlbedo = Pad2.y != 0u;
    const float3 color = mirror ? spec * 0.9
                       : withAlbedo ? albedo * min(direct + ind + paneLight, 4.0) + spec * F * Strength.w + albedo * emit
                                    : min(direct + ind + paneLight, 4.0) + spec * F * Strength.w;
    const float  cl = dot(color, float3(0.3, 0.6, 0.1));
    const float3 clamped = cl > 6.0 ? color * (6.0 / cl) : color;
    OutRRColor[lp]  = float4(Valid(clamped), 1);
    OutRRDiff[lp]   = mirror ? float4(0, 0, 0, 1) : (withAlbedo ? float4(albedo, 1) : float4(1, 1, 1, 1));
    OutRRSpec[lp]   = mirror ? float4(0.9, 0.9, 0.9, 1) : float4((F * Strength.w).xxx, 1);
    OutRRNormal[lp] = float4(N, rough);
}
