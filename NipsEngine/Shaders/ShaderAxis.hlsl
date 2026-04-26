#include "Common.hlsl"

cbuffer GridHelperBuffer : register(b2)
{
    float GridSize;
    float Range;
    float LineThickness;
    float MajorLineInterval;

    float MajorLineThickness;
    float MinorIntensity;
    float MajorIntensity;
    float MaxDistance;

    float AxisThickness;
    float AxisLength;
    float AxisIntensity;
    float GridHelperPadding0;
};

struct VS_OUTPUT
{
    float4 Pos : SV_POSITION;
    float3 WorldPos : TEXCOORD0;
    float AxisCoord : TEXCOORD1;
    float AxisT : TEXCOORD2;
};

VS_OUTPUT VS(uint VertexID : SV_VertexID)
{
    VS_OUTPUT output;

    static const float2 QuadVertices[6] =
    {
        float2(-1.0f, -1.0f),
        float2(1.0f, -1.0f),
        float2(-1.0f, 1.0f),
        float2(1.0f, -1.0f),
        float2(-1.0f, 1.0f),
        float2(1.0f, 1.0f),
    };

    const uint planeID = VertexID / 6;
    const uint localID = VertexID % 6;
    const float2 q = QuadVertices[localID];

    const float axisT = q.x;
    const float stripHalfWidth = max(GridSize * 0.1f * max(AxisThickness, 1.0f), 0.01f);

    float3 worldPos = 0.0f.xxx;
    if (planeID == 0)
    {
        worldPos = float3(0.0f, q.y * stripHalfWidth, axisT * AxisLength);
        output.AxisCoord = worldPos.y;
    }
    else
    {
        worldPos = float3(q.y * stripHalfWidth, 0.0f, axisT * AxisLength);
        output.AxisCoord = worldPos.x;
    }

    output.WorldPos = worldPos;
    output.AxisT = axisT;
    output.Pos = mul(float4(worldPos, 1.0f), mul(View, Projection));
    return output;
}

float4 PS(VS_OUTPUT input) : SV_Target
{
    const float dist = distance(input.WorldPos, CameraPosition.xyz);
    if (dist < 0.5f)
    {
        discard;
    }
    
    float derivative = max(fwidth(input.AxisCoord), 0.0001f);
    float axisDrawWidth = derivative * max(AxisThickness * 3.0f, 1.0f);
    float axisAlpha = 1.0f - smoothstep(0.0f, axisDrawWidth, abs(input.AxisCoord));

    const float endFade = 1.0f - smoothstep(0.85f, 1.0f, abs(input.AxisT));
    axisAlpha *= endFade;

    const float fade = pow(saturate(1.0f - dist / MaxDistance), 2.0f);
    const float finalAlpha = axisAlpha * fade * AxisIntensity;

    if (finalAlpha < 0.01f)
    {
        discard;
    }

    return float4(0.2f, 0.2f, 1.0f, finalAlpha);
}
