// BullyDX12 PostFX - native D3D12 full-screen pass running on the game's final image.
// Contrast-adaptive sharpening, vibrance/saturation/contrast grading and vignette.

cbuffer Params : register(b0)
{
    float2 InvSize;
    float  Sharpen;
    float  Saturation;
    float  Vibrance;
    float  Contrast;
    float  Vignette;
    float  Gamma;
};

Texture2D<float4> Scene : register(t0);

struct VSOut
{
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

VSOut VSMain(uint id : SV_VertexID)
{
    VSOut o;
    o.uv  = float2((id << 1) & 2, id & 2);
    o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}

float Luma(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

float4 PSMain(VSOut i) : SV_Target
{
    int2 p = int2(i.pos.xy);

    float3 c = Scene.Load(int3(p, 0)).rgb;
    float3 n = Scene.Load(int3(p + int2(0, -1), 0)).rgb;
    float3 s = Scene.Load(int3(p + int2(0,  1), 0)).rgb;
    float3 w = Scene.Load(int3(p + int2(-1, 0), 0)).rgb;
    float3 e = Scene.Load(int3(p + int2( 1, 0), 0)).rgb;

    float3 mn = min(c, min(min(n, s), min(w, e)));
    float3 mx = max(c, max(max(n, s), max(w, e)));
    float3 amp = saturate(min(mn, 1.0 - mx) / max(mx, 1e-4));
    amp = sqrt(amp);
    float3 wgt = -amp * lerp(0.125, 0.2, saturate(Sharpen));
    float3 col = saturate((c + (n + s + w + e) * wgt) / (1.0 + 4.0 * wgt));
    col = lerp(c, col, step(1e-4, Sharpen));

    float l = Luma(col);
    col = lerp(l.xxx, col, Saturation);
    float sat = max(col.r, max(col.g, col.b)) - min(col.r, min(col.g, col.b));
    col = lerp(Luma(col).xxx, col, 1.0 + Vibrance * (1.0 - sat));
    col = (col - 0.5) * Contrast + 0.5;
    col = pow(saturate(col), 1.0 / max(Gamma, 0.01));

    float2 d = i.uv - 0.5;
    col *= 1.0 - Vignette * dot(d, d) * 2.0;

    return float4(saturate(col), 1.0);
}
