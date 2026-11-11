// Skinned shader modelled on Bully's BullyDef_80000000: same constants, registers and bone fetch.

float4   mBone[195]       : register(c0);
float4x4 mViewProjection  : register(c195);
float4   vPosition_Camera : register(c199);

struct VSOut {
    float4 pos  : POSITION;
    float2 uv   : TEXCOORD0;
    float4 wpos : TEXCOORD1;
    float3 nrm  : TEXCOORD4;
    float4 col  : COLOR0;
};

VSOut VSSkin(float3 pos : POSITION, float3 weight : BLENDWEIGHT, float4 index : BLENDINDICES, float3 nrm : NORMAL,
             float2 uv : TEXCOORD0, float4 col : COLOR0)
{
    const int4 i = int4(index.zyxw * 765.005859);
    const float w3 = 1.0 - weight.x - weight.y - weight.z;
    const float4 r0 = weight.x * mBone[i.x]     + weight.y * mBone[i.y]     + weight.z * mBone[i.z]     + w3 * mBone[i.w];
    const float4 r1 = weight.x * mBone[i.x + 1] + weight.y * mBone[i.y + 1] + weight.z * mBone[i.z + 1] + w3 * mBone[i.w + 1];
    const float4 r2 = weight.x * mBone[i.x + 2] + weight.y * mBone[i.y + 2] + weight.z * mBone[i.z + 2] + w3 * mBone[i.w + 2];
    const float4 p = float4(pos, 1.0);
    const float4 wp = float4(dot(p, r0), dot(p, r1), dot(p, r2), 1.0);
    VSOut o;
    o.pos  = mul(wp, mViewProjection);
    o.uv   = uv;
    o.wpos = wp;
    o.nrm  = float3(dot(nrm, r0.xyz), dot(nrm, r1.xyz), dot(nrm, r2.xyz));
    o.col  = col * saturate(1.0 - distance(wp.xyz, vPosition_Camera.xyz) * 1e-5);
    return o;
}

float4 PSSkin(float4 col : COLOR0) : COLOR0 { return col; }

float4x4 mWorld : register(c200);

struct VSAlphaOut {
    float4 pos : POSITION;
    float2 uv  : TEXCOORD0;
    float3 nrm : TEXCOORD4;
    float4 col : COLOR0;
};

VSAlphaOut VSAlpha(float3 pos : POSITION, float3 nrm : NORMAL, float2 uv : TEXCOORD0, float4 col : COLOR0)
{
    const float4 wp = mul(float4(pos, 1.0), mWorld);
    VSAlphaOut o;
    o.pos = mul(wp, mViewProjection);
    o.uv  = uv;
    o.nrm = mul(nrm, (float3x3)mWorld);
    o.col = col * saturate(1.0 - distance(wp.xyz, vPosition_Camera.xyz) * 1e-5);
    return o;
}

sampler2D tBase : register(s0);

float4 PSAlpha(float2 uv : TEXCOORD0, float4 col : COLOR0) : COLOR0
{
    const float4 t = tex2D(tBase, uv);
    return float4(col.rgb * t.rgb, t.a * col.a);
}

float4 PSAlphaClip(float2 uv : TEXCOORD0, float4 col : COLOR0) : COLOR0
{
    const float4 t = tex2D(tBase, uv);
    clip(t.a - 0.5);
    return float4(col.rgb * t.rgb, 1.0);
}

samplerCUBE tReflection : register(s1);
float4 PSGlass(float2 uv : TEXCOORD0, float4 col : COLOR0, float3 nrm : TEXCOORD4) : COLOR0
{
    const float4 t = tex2D(tBase, uv);
    return float4(col.rgb * t.rgb + texCUBE(tReflection, nrm).rgb * 0.0001, t.a);
}
