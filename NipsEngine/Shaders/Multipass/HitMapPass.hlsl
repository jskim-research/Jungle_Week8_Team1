#include "../Common.hlsl"

Texture2D<float4> HitMapTexture : register(t8);

struct VS_OUTPUT
{
    float4 Pos : SV_POSITION;
    float2 UV : TEXCOORD0;
};

VS_OUTPUT VSMain(uint vI : SV_VertexID)
{
    VS_OUTPUT vout;
    vout.UV = float2((vI << 1) & 2, vI & 2);
    vout.Pos = float4(vout.UV * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    return vout;
}

float4 PSMain(VS_OUTPUT input) : SV_Target
{
    float4 hitColor = HitMapTexture.Sample(PointSampler, input.UV);
    
    // Alpha blending logic is handled by the blend state, 
    // but we can also do manual overlay if needed.
    return hitColor;
}