/*
    TileLightCulling25D.hlsl

    Forward+ 2.5D tile light culling compute shader.

    Design goals for this engine:
    - One thread group handles one 2D screen tile.
    - DepthPrePass 결과를 읽어 tile-local min/max view-space Z와 32-bit depth mask를 만든다.
    - Point / Spot light를 타일의 truncated frustum에 대해 conservative 하게 걸러낸다.
    - 결과는 UberLit.hlsl가 소비하는 Grid + IndexList 구조로 쓴다.

    Notes:
    - v1은 perspective 카메라를 기준으로 작성했다.
    - Spot light는 일단 radius 기반 bounding sphere로 처리한다.
    - C++ 쪽에서 이 파일의 slot / buffer 크기를 정확히 맞춰줘야 한다.
*/

#include "Common.hlsl"

#ifndef FORWARD_PLUS_TILE_SIZE_X
    #define FORWARD_PLUS_TILE_SIZE_X 16
#endif

#ifndef FORWARD_PLUS_TILE_SIZE_Y
    #define FORWARD_PLUS_TILE_SIZE_Y 16
#endif

#ifndef FORWARD_PLUS_MAX_POINT_LIGHTS_PER_TILE
    #define FORWARD_PLUS_MAX_POINT_LIGHTS_PER_TILE 256
#endif

#ifndef FORWARD_PLUS_MAX_SPOT_LIGHTS_PER_TILE
    #define FORWARD_PLUS_MAX_SPOT_LIGHTS_PER_TILE 256
#endif

#define FORWARD_PLUS_DEPTH_SLICE_COUNT 32
#define FORWARD_PLUS_THREAD_COUNT (FORWARD_PLUS_TILE_SIZE_X * FORWARD_PLUS_TILE_SIZE_Y)

static const float kFloatMax = 3.402823466e+38f;
static const float kEpsilon = 1.0e-5f;

struct FPointLightInfo
{
    float3 Position;
    float  Radius;
    float3 Color;
    float  Intensity;
};

struct FSpotLightInfo
{
    float3 Position;
    float  Radius;

    float3 Color;
    float  Intensity;

    float3 Direction;
    float  InnerConeCos;

    float  OuterConeCos;
    float3 Padding;
};

struct FTileFrustum
{
    float3 LeftPlaneNormal;
    float3 RightPlaneNormal;
    float3 TopPlaneNormal;
    float3 BottomPlaneNormal;
};

struct FSpotConeBounds
{
    float3 ApexVS;
    float Height;

    float3 AxisVS;
    float BaseRadius;

    float3 BaseCenterVS;
    float BroadPhaseRadius;

    float3 BroadPhaseCenterVS;
    float Padding;
};

cbuffer ForwardPlusConstants : register(b11)
{
    uint2 ViewportMin;
    uint2 ViewportSize;
    uint2 DepthTextureSize;
    uint2 TileCount;
    uint bEnable25DMask;
    float3 Padding;
};

cbuffer Lighting : register(b13)
{
    float3 UnusedAmbientColor;
    float UnusedAmbientIntensity;
    uint DirectionalLightCount;
    uint PointLightCount;
    uint SpotLightCount;
    float LightingPad;
};

Texture2D<float> SceneDepth : register(t0);
StructuredBuffer<FPointLightInfo> PointLights : register(t1);
StructuredBuffer<FSpotLightInfo> SpotLights : register(t2);

RWStructuredBuffer<uint2> TilePointLightGrid : register(u0);
RWStructuredBuffer<uint> TilePointLightIndices : register(u1);
RWStructuredBuffer<uint2> TileSpotLightGrid : register(u2);
RWStructuredBuffer<uint> TileSpotLightIndices : register(u3);

groupshared uint gMinDepthBits;
groupshared uint gMaxDepthBits;
groupshared uint gTileDepthMask;
groupshared uint gHasValidDepth;

groupshared float gTileMinDepth;
groupshared float gTileMaxDepth;

groupshared float3 gLeftPlaneNormal;
groupshared float3 gRightPlaneNormal;
groupshared float3 gTopPlaneNormal;
groupshared float3 gBottomPlaneNormal;

groupshared uint gPointLightCount;
groupshared uint gSpotLightCount;
groupshared uint gPointLightIndices[FORWARD_PLUS_MAX_POINT_LIGHTS_PER_TILE];
groupshared uint gSpotLightIndices[FORWARD_PLUS_MAX_SPOT_LIGHTS_PER_TILE];

float3 SafeNormalize(float3 v)
{
    float lenSq = dot(v, v);
    if (lenSq <= kEpsilon)
    {
        return float3(0.0f, 0.0f, 1.0f);
    }

    return v * rsqrt(lenSq);
}

float GetViewDepth(float3 viewPos)
{
    // This engine keeps forward on +X even in view space.
    return viewPos.x;
}

float2 PixelCoordToViewportUV(uint2 pixelCoord)
{
    float2 localPixel = (float2(pixelCoord) - float2(ViewportMin)) + 0.5f;
    float2 safeViewportSize = max(float2(ViewportSize), float2(1.0f, 1.0f));
    return localPixel / safeViewportSize;
}

float3 ReconstructViewPosition(float2 uv, float deviceDepth)
{
    float2 ndc;
    ndc.x = uv.x * 2.0f - 1.0f;
    ndc.y = 1.0f - uv.y * 2.0f;

    float4 clip = float4(ndc, deviceDepth, 1.0f);
    float4 viewH = mul(clip, InvProjection);
    return viewH.xyz / max(viewH.w, kEpsilon);
}

float3 ReconstructViewPosition(uint2 pixelCoord, float deviceDepth)
{
    return ReconstructViewPosition(PixelCoordToViewportUV(pixelCoord), deviceDepth);
}

float3 BuildViewRayFromUV(float2 uv)
{
    return ReconstructViewPosition(uv, 1.0f);
}

FTileFrustum BuildTileFrustum(uint2 tilePixelMin, uint2 tilePixelMaxExclusive)
{
    FTileFrustum frustum;

    float2 safeViewportSize = max(float2(ViewportSize), float2(1.0f, 1.0f));
    float2 tileMinUV = (float2(tilePixelMin) - float2(ViewportMin)) / safeViewportSize;
    float2 tileMaxUV = (float2(tilePixelMaxExclusive) - float2(ViewportMin)) / safeViewportSize;

    float3 pTL = BuildViewRayFromUV(float2(tileMinUV.x, tileMinUV.y));
    float3 pTR = BuildViewRayFromUV(float2(tileMaxUV.x, tileMinUV.y));
    float3 pBL = BuildViewRayFromUV(float2(tileMinUV.x, tileMaxUV.y));
    float3 pBR = BuildViewRayFromUV(float2(tileMaxUV.x, tileMaxUV.y));

    // All side planes pass through the view-space origin.
    // Normals point inward so "dot(n, center) >= -radius" means inside.
    frustum.LeftPlaneNormal = SafeNormalize(cross(pTL, pBL));
    frustum.RightPlaneNormal = SafeNormalize(cross(pBR, pTR));
    frustum.TopPlaneNormal = SafeNormalize(cross(pTR, pTL));
    frustum.BottomPlaneNormal = SafeNormalize(cross(pBL, pBR));

    return frustum;
}

bool SphereIntersectsTileFrustum(float3 centerVS, float radius, FTileFrustum frustum, float tileMinDepth, float tileMaxDepth)
{
    float centerDepth = GetViewDepth(centerVS);

    if (centerDepth + radius < tileMinDepth)
    {
        return false;
    }

    if (centerDepth - radius > tileMaxDepth)
    {
        return false;
    }

    if (dot(frustum.LeftPlaneNormal, centerVS) < -radius)
    {
        return false;
    }

    if (dot(frustum.RightPlaneNormal, centerVS) < -radius)
    {
        return false;
    }

    if (dot(frustum.TopPlaneNormal, centerVS) < -radius)
    {
        return false;
    }

    if (dot(frustum.BottomPlaneNormal, centerVS) < -radius)
    {
        return false;
    }

    return true;
}

uint BuildDepthSliceMask(float rangeMinDepth, float rangeMaxDepth, float tileMinDepth, float tileMaxDepth)
{
    float clampedMinDepth = max(rangeMinDepth, tileMinDepth);
    float clampedMaxDepth = min(rangeMaxDepth, tileMaxDepth);

    if (clampedMaxDepth < clampedMinDepth)
    {
        return 0u;
    }

    float tileDepthExtent = tileMaxDepth - tileMinDepth;
    if (tileDepthExtent <= kEpsilon)
    {
        return 1u;
    }

    float normalizedMinDepth = saturate((clampedMinDepth - tileMinDepth) / tileDepthExtent);
    float normalizedMaxDepth = saturate((clampedMaxDepth - tileMinDepth) / tileDepthExtent);

    uint sliceMin = min((uint)floor(normalizedMinDepth * (FORWARD_PLUS_DEPTH_SLICE_COUNT - 1)),
                        FORWARD_PLUS_DEPTH_SLICE_COUNT - 1);
    uint sliceMax = min((uint)ceil(normalizedMaxDepth * (FORWARD_PLUS_DEPTH_SLICE_COUNT - 1)),
                        FORWARD_PLUS_DEPTH_SLICE_COUNT - 1);

    uint mask = 0u;

    [loop]
    for (uint slice = sliceMin; slice <= sliceMax; ++slice)
    {
        mask |= (1u << slice);

        if (slice == FORWARD_PLUS_DEPTH_SLICE_COUNT - 1)
        {
            break;
        }
    }

    return mask;
}

bool SpherePasses25DMask(float3 centerVS, float radius, float tileMinDepth, float tileMaxDepth, uint tileDepthMask)
{
    float centerDepth = GetViewDepth(centerVS);
    uint lightMask = BuildDepthSliceMask(centerDepth - radius, centerDepth + radius, tileMinDepth, tileMaxDepth);
    if (lightMask == 0u)
    {
        return false;
    }

    if (tileDepthMask == 0u)
    {
        return true;
    }

    return (lightMask & tileDepthMask) != 0u;
}

FSpotConeBounds BuildSpotConeBounds(FSpotLightInfo light)
{
    FSpotConeBounds bounds;

    bounds.ApexVS = mul(float4(light.Position, 1.0f), View).xyz;
    bounds.Height = max(light.Radius, kEpsilon);
    bounds.AxisVS = SafeNormalize(mul(float4(light.Direction, 0.0f), View).xyz);

    float cosTheta = clamp(light.OuterConeCos, 0.001f, 0.9999f);
    float sinTheta = sqrt(saturate(1.0f - cosTheta * cosTheta));
    bounds.BaseRadius = bounds.Height * (sinTheta / cosTheta);
    bounds.BaseCenterVS = bounds.ApexVS + bounds.AxisVS * bounds.Height;

    if (bounds.BaseRadius <= bounds.Height)
    {
        bounds.BroadPhaseRadius =
            (bounds.Height * bounds.Height + bounds.BaseRadius * bounds.BaseRadius) / (2.0f * bounds.Height);
        bounds.BroadPhaseCenterVS = bounds.ApexVS + bounds.AxisVS * bounds.BroadPhaseRadius;
    }
    else
    {
        bounds.BroadPhaseRadius = bounds.BaseRadius;
        bounds.BroadPhaseCenterVS = bounds.BaseCenterVS;
    }

    bounds.Padding = 0.0f;
    return bounds;
}

float ComputeConePlaneMaxDistance(FSpotConeBounds bounds, float3 planeNormal, float planeOffset)
{
    float apexDistance = dot(planeNormal, bounds.ApexVS) + planeOffset;
    
    float axisDot = dot(planeNormal, bounds.AxisVS);
    float radialProjection = sqrt(saturate(1.0f - axisDot * axisDot));
    float baseDistance = dot(planeNormal, bounds.BaseCenterVS) + planeOffset;
    float baseSupport = baseDistance + bounds.BaseRadius * radialProjection;
    
    return max(apexDistance, baseSupport);
}

bool ConeIntersectsTileFrustum(FSpotConeBounds bounds, FTileFrustum frustum, float tileMinDepth, float tileMaxDepth)
{
    if (ComputeConePlaneMaxDistance(bounds, frustum.LeftPlaneNormal, 0.0f) < 0.0f)
    {
        return false;
    }

    if (ComputeConePlaneMaxDistance(bounds, frustum.RightPlaneNormal, 0.0f) < 0.0f)
    {
        
        return false;
    }

    if (ComputeConePlaneMaxDistance(bounds, frustum.TopPlaneNormal, 0.0f) < 0.0f)
    {
        return false;
    }

    if (ComputeConePlaneMaxDistance(bounds, frustum.BottomPlaneNormal, 0.0f) < 0.0f)
    {
        return false;
    }

    if (ComputeConePlaneMaxDistance(bounds, float3(1.0f, 0.0f, 0.0f), -tileMinDepth) < 0.0f)
    {
        return false;
    }

    if (ComputeConePlaneMaxDistance(bounds, float3(-1.0f, 0.0f, 0.0f), tileMaxDepth) < 0.0f)
    {
        return false;
    }

    return true;
}

bool ConePasses25DMask(FSpotConeBounds bounds, float tileMinDepth, float tileMaxDepth, uint tileDepthMask)
{
    float radialDepth = sqrt(saturate(1.0f - bounds.AxisVS.x * bounds.AxisVS.x));
    float apexDepth = GetViewDepth(bounds.ApexVS);
    float baseDepth = GetViewDepth(bounds.BaseCenterVS);
    float coneMinDepth = min(apexDepth, baseDepth - bounds.BaseRadius * radialDepth);
    float coneMaxDepth = max(apexDepth, baseDepth + bounds.BaseRadius * radialDepth);

    uint lightMask = BuildDepthSliceMask(coneMinDepth, coneMaxDepth, tileMinDepth, tileMaxDepth);
    if (lightMask == 0u)
    {
        return false;
    }

    if (tileDepthMask == 0u)
    {
        return true;
    }

    return (lightMask & tileDepthMask) != 0u;
}

[numthreads(FORWARD_PLUS_TILE_SIZE_X, FORWARD_PLUS_TILE_SIZE_Y, 1)]
void TileLightCulling25DCS(
    uint3 GroupID : SV_GroupID,
    uint3 GroupThreadID : SV_GroupThreadID,
    uint GroupIndex : SV_GroupIndex)
{
    uint flatThreadIndex = GroupIndex;
    uint tileIndex = GroupID.y * TileCount.x + GroupID.x;

    uint2 tilePixelMin = ViewportMin + GroupID.xy * uint2(FORWARD_PLUS_TILE_SIZE_X, FORWARD_PLUS_TILE_SIZE_Y);
    uint2 tilePixelMaxExclusive = min(tilePixelMin + uint2(FORWARD_PLUS_TILE_SIZE_X, FORWARD_PLUS_TILE_SIZE_Y),
                                      ViewportMin + ViewportSize);

    uint pointStartOffset = tileIndex * FORWARD_PLUS_MAX_POINT_LIGHTS_PER_TILE;
    uint spotStartOffset = tileIndex * FORWARD_PLUS_MAX_SPOT_LIGHTS_PER_TILE;

    if (GroupIndex == 0u)
    {
        gMinDepthBits = asuint(kFloatMax);
        gMaxDepthBits = asuint(0.0f);
        gTileDepthMask = 0u;
        gHasValidDepth = 0u;
        gTileMinDepth = 0.0f;
        gTileMaxDepth = 0.0f;
        gPointLightCount = 0u;
        gSpotLightCount = 0u;
    }

    GroupMemoryBarrierWithGroupSync();

    bool bPixelInsideViewport = false;
    bool bHasDepthSample = false;
    float viewDepth = 0.0f;

    uint2 pixelCoord = tilePixelMin + GroupThreadID.xy;
    if (pixelCoord.x < (ViewportMin.x + ViewportSize.x) &&
        pixelCoord.y < (ViewportMin.y + ViewportSize.y) &&
        pixelCoord.x < DepthTextureSize.x &&
        pixelCoord.y < DepthTextureSize.y)
    {
        bPixelInsideViewport = true;

        float deviceDepth = SceneDepth.Load(int3(pixelCoord, 0));
        if (deviceDepth < 1.0f)
        {
            float3 viewPos = ReconstructViewPosition(pixelCoord, deviceDepth);
            viewDepth = max(GetViewDepth(viewPos), 0.0f);
            bHasDepthSample = true;

            InterlockedMin(gMinDepthBits, asuint(viewDepth));
            InterlockedMax(gMaxDepthBits, asuint(viewDepth));
        }
    }

    GroupMemoryBarrierWithGroupSync();

    if (GroupIndex == 0u)
    {
        gHasValidDepth = (gMinDepthBits != asuint(kFloatMax)) ? 1u : 0u;

        if (gHasValidDepth != 0u)
        {
            gTileMinDepth = asfloat(gMinDepthBits);
            gTileMaxDepth = asfloat(gMaxDepthBits);

            FTileFrustum tileFrustum = BuildTileFrustum(tilePixelMin, tilePixelMaxExclusive);
            gLeftPlaneNormal = tileFrustum.LeftPlaneNormal;
            gRightPlaneNormal = tileFrustum.RightPlaneNormal;
            gTopPlaneNormal = tileFrustum.TopPlaneNormal;
            gBottomPlaneNormal = tileFrustum.BottomPlaneNormal;
        }
        else
        {
            TilePointLightGrid[tileIndex] = uint2(pointStartOffset, 0u);
            TileSpotLightGrid[tileIndex] = uint2(spotStartOffset, 0u);
        }
    }

    GroupMemoryBarrierWithGroupSync();

    if (gHasValidDepth == 0u)
    {
        return;
    }

    if (bPixelInsideViewport && bHasDepthSample)
    {
        uint pixelMask = BuildDepthSliceMask(viewDepth, viewDepth, gTileMinDepth, gTileMaxDepth);
        InterlockedOr(gTileDepthMask, pixelMask);
    }

    GroupMemoryBarrierWithGroupSync();

    FTileFrustum sharedFrustum;
    sharedFrustum.LeftPlaneNormal = gLeftPlaneNormal;
    sharedFrustum.RightPlaneNormal = gRightPlaneNormal;
    sharedFrustum.TopPlaneNormal = gTopPlaneNormal;
    sharedFrustum.BottomPlaneNormal = gBottomPlaneNormal;

    [loop]
    for (uint lightIndex = flatThreadIndex; lightIndex < PointLightCount; lightIndex += FORWARD_PLUS_THREAD_COUNT)
    {
        FPointLightInfo light = PointLights[lightIndex];
        float3 centerVS = mul(float4(light.Position, 1.0f), View).xyz;
        float radius = light.Radius;

        if (!SphereIntersectsTileFrustum(centerVS, radius, sharedFrustum, gTileMinDepth, gTileMaxDepth))
        {
            continue;
        }

        if (bEnable25DMask != 0u &&
            !SpherePasses25DMask(centerVS, radius, gTileMinDepth, gTileMaxDepth, gTileDepthMask))
        {
            continue;
        }

        uint writeIndex = 0u;
        InterlockedAdd(gPointLightCount, 1u, writeIndex);
        if (writeIndex < FORWARD_PLUS_MAX_POINT_LIGHTS_PER_TILE)
        {
            gPointLightIndices[writeIndex] = lightIndex;
        }
    }

   [loop]
    for (uint lightIndex = flatThreadIndex; lightIndex < SpotLightCount; lightIndex += FORWARD_PLUS_THREAD_COUNT)
    {
        FSpotLightInfo light = SpotLights[lightIndex];

        FSpotConeBounds bounds = BuildSpotConeBounds(light);

        if (!SphereIntersectsTileFrustum(bounds.BroadPhaseCenterVS, bounds.BroadPhaseRadius, sharedFrustum,
                                         gTileMinDepth, gTileMaxDepth))
        {
            continue;
        }

        if (!ConeIntersectsTileFrustum(bounds, sharedFrustum, gTileMinDepth, gTileMaxDepth))
        {
            continue;
        }

        if (bEnable25DMask != 0u &&
            !ConePasses25DMask(bounds, gTileMinDepth, gTileMaxDepth, gTileDepthMask))
        {
            continue;
        }

        uint writeIndex = 0u;
        InterlockedAdd(gSpotLightCount, 1u, writeIndex);
        if (writeIndex < FORWARD_PLUS_MAX_SPOT_LIGHTS_PER_TILE)
        {
            gSpotLightIndices[writeIndex] = lightIndex;
        }
    }

    GroupMemoryBarrierWithGroupSync();

    uint pointCount = min(gPointLightCount, FORWARD_PLUS_MAX_POINT_LIGHTS_PER_TILE);
    uint spotCount = min(gSpotLightCount, FORWARD_PLUS_MAX_SPOT_LIGHTS_PER_TILE);

    if (GroupIndex == 0u)
    {
        TilePointLightGrid[tileIndex] = uint2(pointStartOffset, pointCount);
        TileSpotLightGrid[tileIndex] = uint2(spotStartOffset, spotCount);
    }

    [loop]
    for (uint writeIndex = flatThreadIndex; writeIndex < pointCount; writeIndex += FORWARD_PLUS_THREAD_COUNT)
    {
        TilePointLightIndices[pointStartOffset + writeIndex] = gPointLightIndices[writeIndex];
    }

    [loop]
    for (uint writeIndex = flatThreadIndex; writeIndex < spotCount; writeIndex += FORWARD_PLUS_THREAD_COUNT)
    {
        TileSpotLightIndices[spotStartOffset + writeIndex] = gSpotLightIndices[writeIndex];
    }
}
