#include "Common.hlsl"

struct VSInput
{
    float3 position : POSITION;
    float4 color : COLOR;
};

struct FDepthOnlyVSOutput
{
    float4 position : SV_POSITION;
};

float4 DepthPrepassVS(VSInput input) : SV_POSITION
{
    return ApplyMVP(input.position);
}

void DepthPrepassPS(FDepthOnlyVSOutput input)
{
}