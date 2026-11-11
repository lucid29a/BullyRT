struct VSIn  { float3 pos : POSITION; float4 col : COLOR0; };
struct VSOut { float4 pos : POSITION; float4 col : COLOR0; };

VSOut VSMain(VSIn i)
{
    VSOut o;
    o.pos = float4(i.pos, 1.0);
    o.col = i.col;
    return o;
}

float4 PSMain(float4 col : COLOR0) : COLOR0
{
    return col;
}

float4 PSOverride(float4 col : COLOR0) : COLOR0
{
    return float4(0.0, 1.0, 0.0, 1.0);
}
