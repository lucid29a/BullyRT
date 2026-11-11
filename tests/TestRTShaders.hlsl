// Mimics Bully's Gamebryo shaders: same constant names and register layout (CTAB is kept).

float4x4 mWorld            : register(c0);
float4x4 mViewProjection   : register(c4);
float4   vPosition_Camera  : register(c8);

struct VSOut { float4 pos : POSITION; float4 col : COLOR0; float3 nrm : TEXCOORD4; };

VSOut VSScene(float3 pos : POSITION, float3 nrm : NORMAL, float4 col : COLOR0)
{
    VSOut o;
    float4 wp = mul(float4(pos, 1.0), mWorld);
    o.pos = mul(wp, mViewProjection);
    o.col = col * saturate(1.0 - distance(wp.xyz, vPosition_Camera.xyz) * 1e-5);
    o.nrm = mul(nrm, (float3x3)mWorld);
    return o;
}

float4 vPosition_PointLight0    : register(c10);
float4 cDiffuse_PointLight0     : register(c11);
float4 vAttenuation_PointLight0 : register(c12);
float4 PSPointLit(float4 col : COLOR0) : COLOR0
{
    return float4(col.rgb * saturate(0.9 + 1e-5 * (vPosition_PointLight0.x + cDiffuse_PointLight0.x + vAttenuation_PointLight0.x)), 1.0);
}

float4 VSNoCam(float3 pos : POSITION, float3 nrm : NORMAL, float4 col : COLOR0, out float4 c : COLOR0, out float3 n : TEXCOORD4) : POSITION
{
    c = col;
    n = mul(nrm, (float3x3)mWorld);
    return mul(mul(float4(pos, 1.0), mWorld), mViewProjection);
}

float4 vDirection_ShadowSpotLight0 : register(c0);

float4 PSScene(float4 col : COLOR0) : COLOR0
{
    return float4(col.rgb * saturate(0.999 + 0.001 * abs(vDirection_ShadowSpotLight0.y)), 1.0);
}

struct VSOutW { float4 pos : POSITION; float4 col : COLOR0; float4 wpos : TEXCOORD1; float3 nrm : TEXCOORD4; };
VSOutW VSSceneW(float3 pos : POSITION, float3 nrm : NORMAL, float4 col : COLOR0)
{
    VSOutW o;
    float4 wp = mul(float4(pos, 1.0), mWorld);
    o.pos  = mul(wp, mViewProjection);
    o.col  = col * saturate(1.0 - distance(wp.xyz, vPosition_Camera.xyz) * 1e-5);
    o.wpos = wp;
    o.nrm  = mul(nrm, (float3x3)mWorld);
    return o;
}

row_major float4x4 mWorldToShadowMap_ShadowSpotLight0 : register(c0);
float  fShadowBias_ShadowSpotLight0 : register(c4);
float4 vPosition_ShadowSpotLight0   : register(c5);
sampler2D tShadowMap_ShadowSpotLight0 : register(s0);

float4 PSShadowRecv(float4 col : COLOR0, float4 wpos : TEXCOORD1) : COLOR0
{
    float4 sc  = mul(mWorldToShadowMap_ShadowSpotLight0, float4(wpos.xyz, 1.0));
    float2 uv  = sc.xy / sc.w * float2(0.5, -0.5) + 0.5;
    float  dep = saturate(sc.z / sc.w - fShadowBias_ShadowSpotLight0);
    float  lit = tex2D(tShadowMap_ShadowSpotLight0, uv).r >= dep ? 1.0 : 0.0;
    float  toLight = saturate(1.0 - length(vPosition_ShadowSpotLight0.xyz - wpos.xyz) * 1e-6);
    return float4(col.rgb * (0.35 + 0.65 * lit * toLight), 1.0);
}

float4 VSHud(float3 pos : POSITION, float3 nrm : NORMAL, float4 col : COLOR0, out float4 c : COLOR0) : POSITION
{
    c = col;
    return float4(pos, 1.0);
}

float4 PSHud(float4 col : COLOR0) : COLOR0 { return col; }
