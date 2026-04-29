#include "Common.hlsl"

struct VSInput
{
    float3 position : POSITION;
    float4 color : COLOR;
};

struct PSInput
{
    float4 position : SV_POSITION;
    float4 color : COLOR;
};


cbuffer PSM : register(b2)
{
    uint isPSM;
    float3 Padding0;
    row_major float4x4 PSM;
}

PSInput VS(VSInput input)
{
    PSInput output;

    if (isPSM != 0u)
    {
        float4 world = mul(float4(input.position, 1.0f), Model);

        output.position = mul(world, PSM);

    }
    else
    {
        output.position = ApplyMVP(input.position);
    }
    output.color = input.color;
    return output;
}

float4 PS(PSInput input) : SV_TARGET
{
    return lerp(input.color, float4(WireframeRGB, 1.0f), bIsWireframe);
}
