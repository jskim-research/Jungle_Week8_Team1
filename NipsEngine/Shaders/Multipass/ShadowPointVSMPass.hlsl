#include "../Common.hlsl"

cbuffer ShadowPointVSMConstants : register(b10)
{
    float ShadowFar;
    float3 Padding0;
}

struct VSInput
{
    float3 Position : POSITION;
};

struct PSInput
{
    float4 ClipPos : SV_POSITION;
    float3 LightViewPos : TEXCOORD0;
};

PSInput mainVS(VSInput input)
{
    PSInput output;

    float4 worldPos = mul(float4(input.Position, 1.0f), Model);
    float4 lightViewPos = mul(worldPos, View);
    output.ClipPos = mul(lightViewPos, Projection);
    output.LightViewPos = lightViewPos.xyz;
    return output;
}

float2 mainPS(PSInput input) : SV_TARGET
{
    float distanceToLight = length(input.LightViewPos);
    float clampedDistance = min(distanceToLight, max(ShadowFar, 1.0e-4f));
    return float2(clampedDistance, clampedDistance * clampedDistance);
}
