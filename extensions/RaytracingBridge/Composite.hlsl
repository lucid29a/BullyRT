// RaytracingBridge (game side) passes.
// PSDepth     : game depth (D24S8 copy viewed as R24_UNORM_X8) -> shared R32_FLOAT for the bridge
// PSVis       : internal mode - writes the light factor sampled by the patched game shaders
// PSComposite : overlay mode  - multiplies the finished scene (blend: dest * src)

cbuffer Params : register(b0)
{
    float AoStrength;
    float ShadowStrength;
    float ShadowAmbient;
    float Debug;
    float TargetWidth;
    float TargetHeight;
    float2 Pad;
};

Texture2D<float>  DepthIn : register(t0);
Texture2D<float2> RtIn    : register(t0);

float4 VSMain(uint id : SV_VertexID) : SV_Position
{
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}

float PSDepth(float4 pos : SV_Position) : SV_Target
{
    return DepthIn.Load(int3(pos.xy, 0));
}

float LightFactor(float2 rt)
{
    float ao  = lerp(1.0, rt.x, AoStrength);
    float sun = lerp(1.0, lerp(ShadowAmbient, 1.0, rt.y), ShadowStrength);
    float f   = ao * sun;
    if (Debug > 0.5 && Debug < 1.5) f = rt.x;
    if (Debug > 1.5) f = rt.y;
    return f;
}

float4 PSVis(float4 pos : SV_Position) : SV_Target
{
    uint w, h;
    RtIn.GetDimensions(w, h);
    float2 uv = pos.xy / float2(TargetWidth, TargetHeight);
    float2 rt = RtIn.Load(int3(min(uint2(uv * float2(w, h)), uint2(w - 1, h - 1)), 0));
    float  f  = LightFactor(rt);
    return float4(f, f, f, 1.0);
}

float4 PSComposite(float4 pos : SV_Position) : SV_Target
{
    float f = LightFactor(RtIn.Load(int3(pos.xy, 0)));
    return float4(f, f, f, 1.0);
}
