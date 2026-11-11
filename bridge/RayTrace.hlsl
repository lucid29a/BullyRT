// BullyRTBridge - screen-space driven ray tracing with DXR 1.1 inline ray queries.
// For every pixel: reconstruct the world position from the game's depth, derive a normal from
// neighbouring depths, then trace sun shadow and ambient occlusion rays against the TLAS.
// Output: R = ambient occlusion (1 = open), G = sun visibility (1 = lit).

RaytracingAccelerationStructure Scene : register(t0);
Texture2D<float>                Depth : register(t1);
RWTexture2D<float2>             Output : register(u0);
Texture2D<float2>               Raw    : register(t2);

cbuffer Frame : register(b0)
{
    row_major float4x4 InvViewProj;
    float4 CameraPos;
    float4 SunDir;
    uint2  Size;
    uint   Effects;
    uint   AoSamples;
    float  AoRadius;
    float  ShadowMaxDistance;
    float  NormalBias;
    uint   FrameIndex;
};

float3 WorldFromDepth(uint2 p, float d)
{
    float2 uv  = (float2(p) + 0.5) / float2(Size);
    float4 ndc = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, d, 1.0);
    float4 w   = mul(InvViewProj, ndc);
    return w.xyz / w.w;
}

uint Hash(uint x)
{
    x ^= x >> 16; x *= 0x7feb352d;
    x ^= x >> 15; x *= 0x846ca68b;
    x ^= x >> 16;
    return x;
}

float Rand(inout uint s) { s = Hash(s); return (s & 0xFFFFFF) / 16777216.0; }

bool Occluded(float3 origin, float3 dir, float tmax)
{
    RayDesc ray;
    ray.Origin    = origin;
    ray.Direction = dir;
    ray.TMin      = 0.0;
    ray.TMax      = tmax;
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_FORCE_OPAQUE> q;
    q.TraceRayInline(Scene, RAY_FLAG_NONE, 0xFF, ray);
    q.Proceed();
    return q.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= Size)) return;

    float d = Depth.Load(int3(id.xy, 0));
    if (d >= 0.99999 || d <= 0.0) { Output[id.xy] = float2(1, 1); return; }

    float3 P = WorldFromDepth(id.xy, d);

    uint2  pxR = uint2(min(id.x + 1, Size.x - 1), id.y), pxL = uint2(id.x > 0 ? id.x - 1 : 0, id.y);
    uint2  pyD = uint2(id.x, min(id.y + 1, Size.y - 1)), pyU = uint2(id.x, id.y > 0 ? id.y - 1 : 0);
    float3 R = WorldFromDepth(pxR, Depth.Load(int3(pxR, 0))) - P;
    float3 L = P - WorldFromDepth(pxL, Depth.Load(int3(pxL, 0)));
    float3 D = WorldFromDepth(pyD, Depth.Load(int3(pyD, 0))) - P;
    float3 U = P - WorldFromDepth(pyU, Depth.Load(int3(pyU, 0)));
    float3 dx = dot(R, R) < dot(L, L) ? R : L;
    float3 dy = dot(D, D) < dot(U, U) ? D : U;
    float3 N  = normalize(cross(dy, dx));
    float3 toCam = CameraPos.xyz - P;
    if (dot(N, toCam) < 0) N = -N;

    float  dist   = length(toCam);
    float3 origin = P + N * (NormalBias * (1.0 + dist * 0.01));

    float sun = 1.0;
    if (Effects & 2) {
        if (SunDir.w > 0.5) {
            float3 Lv = SunDir.xyz - origin;
            float  dl = length(Lv);
            float3 Ls = Lv / max(dl, 1e-4);
            if (dot(N, Ls) > 0.0) sun = Occluded(origin, Ls, max(dl - 0.05, 0.0)) ? 0.0 : 1.0;
        } else {
            float3 Ls = normalize(SunDir.xyz);
            if (dot(N, Ls) > 0.0) {
                float3 st = normalize(abs(Ls.z) < 0.99 ? cross(Ls, float3(0, 0, 1)) : cross(Ls, float3(1, 0, 0)));
                float3 sb = cross(Ls, st);
                uint   sseed = Hash(id.x * 7919u + id.y * 104729u + 17u);
                uint   n = max(AoSamples, 1u), lit = 0;
                [loop] for (uint i = 0; i < n; ++i) {
                    float r = sqrt(Rand(sseed)) * (-SunDir.w), a = 6.2831853 * Rand(sseed);
                    float3 dir = normalize(Ls + st * (r * cos(a)) + sb * (r * sin(a)));
                    lit += Occluded(origin, dir, ShadowMaxDistance) ? 0u : 1u;
                }
                sun = float(lit) / float(n);
            }
        }
    }

    float ao = 1.0;
    if ((Effects & 1) && AoSamples > 0) {
        uint seed = Hash(id.x * 1973u + id.y * 9277u);
        float3 t = normalize(abs(N.y) < 0.99 ? cross(N, float3(0, 1, 0)) : cross(N, float3(1, 0, 0)));
        float3 b = cross(N, t);
        uint hits = 0;
        [loop] for (uint i = 0; i < AoSamples; ++i) {
            float u1 = Rand(seed), u2 = Rand(seed);
            float r = sqrt(u1), phi = 6.2831853 * u2;
            float3 dir = normalize(t * (r * cos(phi)) + b * (r * sin(phi)) + N * sqrt(max(0.0, 1.0 - u1)));
            hits += Occluded(origin, dir, AoRadius) ? 1u : 0u;
        }
        ao = 1.0 - float(hits) / float(AoSamples);
    }

    Output[id.xy] = float2(ao, sun);
}

[numthreads(8, 8, 1)]
void CSBlur(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= Size)) return;
    float2 c = Raw.Load(int3(id.xy, 0));
    float  d = Depth.Load(int3(id.xy, 0));
    if (d >= 0.99999 || d <= 0.0) { Output[id.xy] = c; return; }
    float3 P    = WorldFromDepth(id.xy, d);
    float  dist = length(CameraPos.xyz - P);
    float  tol  = max(0.05, dist * 0.02);

    float2 sum = 0.0;
    float  wsum = 0.0;
    [unroll] for (int y = -3; y <= 3; ++y) {
        [unroll] for (int x = -3; x <= 3; ++x) {
            int2   q  = clamp(int2(id.xy) + int2(x, y), int2(0, 0), int2(Size) - 1);
            float  dq = Depth.Load(int3(q, 0));
            float3 Q  = WorldFromDepth(uint2(q), dq);
            float  w  = exp(-length(Q - P) / tol) * exp(-(x * x + y * y) / 18.0);
            sum  += Raw.Load(int3(q, 0)) * w;
            wsum += w;
        }
    }
    Output[id.xy] = sum / max(wsum, 1e-4);
}
