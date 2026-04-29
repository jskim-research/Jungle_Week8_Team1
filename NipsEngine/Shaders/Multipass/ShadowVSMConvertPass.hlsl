#include "../Common.hlsl"

Texture2DArray DepthShadowInput : register(t0);

cbuffer ShadowVSMConvertConstants : register(b10)
{
    int SliceIndex;
    int LinearizeDepth;
    float DepthLinearizeA;
    float DepthLinearizeB;
    float InvDepthRange;
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
    DepthShadowInput.GetDimensions(width, height, layers);

    int2 pixelCoord = clamp(int2(input.ClipPos.xy), int2(0, 0), int2((int)width - 1, (int)height - 1));
    float depth = DepthShadowInput.Load(int4(pixelCoord, SliceIndex, 0)).r;
    if (LinearizeDepth != 0)
    {
        float denom = max(DepthLinearizeA - depth, 1.0e-5f);
        float faceDepth = DepthLinearizeB / denom;
        float2 uv = (float2(pixelCoord) + 0.5f) / max(float2(width, height), float2(1.0f, 1.0f));
        float2 ndc = uv * 2.0f - 1.0f;
        float radialScale = sqrt(1.0f + dot(ndc, ndc));
        depth = saturate((faceDepth * radialScale) * InvDepthRange);
    }

    return float2(depth, depth * depth);
}
