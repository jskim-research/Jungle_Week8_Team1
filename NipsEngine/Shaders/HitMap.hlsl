#include "Common.hlsl"

#ifndef FORWARD_PLUS_TILE_SIZE_X
    #define FORWARD_PLUS_TILE_SIZE_X 16
#endif

#ifndef FORWARD_PLUS_TILE_SIZE_Y
    #define FORWARD_PLUS_TILE_SIZE_Y 16
#endif

cbuffer ForwardPlusConstants : register(b11)
{
    uint2 ScreenSize;
    uint2 TileCount;
    uint bEnable25DMask;
    float3 ForwardPlusPadding;
};

// Slots for 2.5D grids
StructuredBuffer<uint2> TilePointLightGrid : register(t10);
StructuredBuffer<uint2> TileSpotLightGrid : register(t12);

struct VSOutput
{
    float4 Pos : SV_POSITION;
    float2 UV : TEXCOORD0;
};

VSOutput HitMapVS(uint VertexID : SV_VertexID)
{
    VSOutput Output;
    // Full screen triangle
    Output.UV = float2((VertexID << 1) & 2, VertexID & 2);
    Output.Pos = float4(Output.UV * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    return Output;
}

float3 EvaluateHitmapRamp(float t)
{
    t = saturate(t);

    const float3 c0 = float3(0.02f, 0.02f, 0.04f);
    const float3 c1 = float3(0.06f, 0.22f, 0.68f);
    const float3 c2 = float3(0.00f, 0.72f, 0.84f);
    const float3 c3 = float3(0.96f, 0.86f, 0.18f);
    const float3 c4 = float3(0.92f, 0.20f, 0.10f);

    if (t < 0.25f)
    {
        return lerp(c0, c1, t / 0.25f);
    }

    if (t < 0.50f)
    {
        return lerp(c1, c2, (t - 0.25f) / 0.25f);
    }

    if (t < 0.75f)
    {
        return lerp(c2, c3, (t - 0.50f) / 0.25f);
    }

    return lerp(c3, c4, (t - 0.75f) / 0.25f);
}

float4 HitMapPS(VSOutput Input) : SV_TARGET
{
    if (TileCount.x == 0u || TileCount.y == 0u || ScreenSize.x == 0u || ScreenSize.y == 0u)
    {
        return float4(0.0f, 0.0f, 0.0f, 0.0f);
    }

    const float2 localUV = saturate(Input.UV);
    const float2 localPixel = min(localUV * float2(ScreenSize), float2(ScreenSize) - float2(0.5f, 0.5f));
    const uint2 tileCoord = min(uint2(localPixel / float2(FORWARD_PLUS_TILE_SIZE_X, FORWARD_PLUS_TILE_SIZE_Y)),
                                TileCount - uint2(1u, 1u));
    const uint tileIndex = tileCoord.x + tileCoord.y * TileCount.x;

    const uint pointCount = TilePointLightGrid[tileIndex].y;
    const uint spotCount = TileSpotLightGrid[tileIndex].y;
    const uint totalCount = pointCount + spotCount;

    if (totalCount == 0u)
    {
        // Still return something if grid line is needed, or just discard/zero
        return float4(0.0f, 0.0f, 0.0f, 0.0f);
    }

    // Normalized count for ramp (log scale to see small counts better)
    const float normalizedCount = saturate(log2((float)totalCount + 1.0f) / log2(33.0f));
    float3 overlayColor = EvaluateHitmapRamp(normalizedCount);

    // Grid lines for tiles
    const float2 tileCoordPixel = frac(localPixel / float2(FORWARD_PLUS_TILE_SIZE_X, FORWARD_PLUS_TILE_SIZE_Y));
    const float2 tileEdgeDistance =
        min(tileCoordPixel, 1.0f - tileCoordPixel) * float2(FORWARD_PLUS_TILE_SIZE_X, FORWARD_PLUS_TILE_SIZE_Y);
    const float edgeDistance = min(tileEdgeDistance.x, tileEdgeDistance.y);
    const float gridLine = 1.0f - smoothstep(0.3f, 0.6f, edgeDistance);

    const float3 highlightedColor = lerp(overlayColor, 1.0f.xxx, gridLine * 0.3f);
    const float overlayStrength = lerp(0.10f, 0.45f, normalizedCount);

    return float4(highlightedColor, overlayStrength); // Using alpha for blending
}
