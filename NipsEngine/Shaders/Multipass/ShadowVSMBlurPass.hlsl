#include "../Common.hlsl"

Texture2DArray MomentsInput : register(t0);
SamplerState LinearClampSampler : register(s0);

cbuffer ShadowVSMBlurConstants : register(b10)
{
    float2 BlurDirection;
    int SliceIndex;
    float Padding0;
}

struct VSOutput
{
    float4 ClipPos : SV_POSITION;
};

VSOutput mainVS(uint vertexID : SV_VertexID)
{
    VSOutput output;

    float2 pos;
    if (vertexID == 0)
        pos = float2(-1.0f, -1.0f);
    else if (vertexID == 1)
        pos = float2(-1.0f, 3.0f);
    else
        pos = float2(3.0f, -1.0f);

    output.ClipPos = float4(pos, 0.0f, 1.0f);
    return output;
}

float2 mainPS(VSOutput input) : SV_TARGET
{
    uint width = 0;
    uint height = 0;
    uint layers = 0;
    MomentsInput.GetDimensions(width, height, layers);

    float2 invResolution = 1.0f / max(float2(width, height), float2(1.0f, 1.0f));
    int2 pixelCoord = clamp(int2(input.ClipPos.xy), int2(0, 0), int2((int)width - 1, (int)height - 1));
    float2 uv = (float2(pixelCoord) + 0.5f) * invResolution;

    const float weights[5] = { 0.0625f, 0.25f, 0.375f, 0.25f, 0.0625f };

    float2 result = 0.0f;
    [unroll]
    for (int tap = -2; tap <= 2; ++tap)
    {
        result += MomentsInput.SampleLevel(
            LinearClampSampler,
            float3(uv + BlurDirection * tap, SliceIndex),
            0.0f).rg * weights[tap + 2];
    }

    return result;
}
