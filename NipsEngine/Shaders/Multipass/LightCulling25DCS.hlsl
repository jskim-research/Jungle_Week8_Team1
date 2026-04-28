/*
    TileLightCulling25D.hlsl
    +X Forward coordinate system optimized.
*/

#include "../Common.hlsl"

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
    float Radius;
    float3 Color;
    float Intensity;
    int ShadowIndex;
    float3 PointLightInfoPadding;
};
struct FSpotLightInfo { float3 Position; float Radius; float3 Color; float Intensity; float3 Direction; float InnerConeCos; float OuterConeCos; float3 Padding; };
struct FTileFrustum { float3 LeftNormal; float3 RightNormal; float3 TopNormal; float3 BottomNormal; };
struct FSpotConeBounds { float3 ApexVS; float Height; float3 AxisVS; float BaseRadius; float3 BaseCenterVS; float BroadPhaseRadius; float3 BroadPhaseCenterVS; float Padding; };

cbuffer ForwardPlusConstants : register(b11) { uint2 ScreenSize; uint2 TileCount; uint bEnable25DMask; float3 ForwardPlusPadding; };
cbuffer Lighting : register(b13) { float3 UnusedAmbientColor; float UnusedAmbientIntensity; uint DirectionalLightCount; uint PointLightCount; uint SpotLightCount; float LightingPad; };

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
groupshared float3 gPlanes[4]; // Left, Right, Top, Bottom
groupshared uint gPointLightCount;
groupshared uint gSpotLightCount;
groupshared uint gPointLightIndices[FORWARD_PLUS_MAX_POINT_LIGHTS_PER_TILE];
groupshared uint gSpotLightIndices[FORWARD_PLUS_MAX_SPOT_LIGHTS_PER_TILE];

float3 SafeNormalize(float3 v) { float lenSq = dot(v, v); return (lenSq <= kEpsilon) ? float3(1,0,0) : v * rsqrt(lenSq); }
float GetViewDepth(float3 viewPos) { return viewPos.x; }

float3 ReconstructViewPosition(uint2 pixelCoord, float deviceDepth)
{
    float2 uv = (float2(pixelCoord) + 0.5f) / float2(ScreenSize);
    float4 clip = float4(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f, deviceDepth, 1.0f);
    float4 viewH = mul(clip, InverseProjection);
    return viewH.xyz / max(viewH.w, kEpsilon);
}

FTileFrustum BuildTileFrustum(uint2 tilePixelMin, uint2 tilePixelMax)
{
    FTileFrustum frustum;
    // Four corner rays in View Space
    float3 pTL = SafeNormalize(ReconstructViewPosition(uint2(tilePixelMin.x, tilePixelMin.y), 1.0f));
    float3 pTR = SafeNormalize(ReconstructViewPosition(uint2(tilePixelMax.x, tilePixelMin.y), 1.0f));
    float3 pBL = SafeNormalize(ReconstructViewPosition(uint2(tilePixelMin.x, tilePixelMax.y), 1.0f));
    float3 pBR = SafeNormalize(ReconstructViewPosition(uint2(tilePixelMax.x, tilePixelMax.y), 1.0f));

    // Plane normals pointing INSIDE
    frustum.LeftNormal   = SafeNormalize(cross(pTL, pBL));
    frustum.RightNormal  = SafeNormalize(cross(pBR, pTR));
    frustum.TopNormal    = SafeNormalize(cross(pTR, pTL));
    frustum.BottomNormal = SafeNormalize(cross(pBL, pBR));
    return frustum;
}

uint BuildDepthSliceMask(float minZ, float maxZ, float tileMinZ, float tileMaxZ)
{
    float clampedMin = max(minZ, tileMinZ);
    float clampedMax = min(maxZ, tileMaxZ);
    if (clampedMax < clampedMin) return 0u;
    float extent = tileMaxZ - tileMinZ;
    if (extent <= kEpsilon) return 1u;

    float nMin = saturate((clampedMin - tileMinZ) / extent);
    float nMax = saturate((clampedMax - tileMinZ) / extent);
    uint sMin = min((uint)floor(nMin * 31.0f), 31u);
    uint sMax = min((uint)ceil(nMax * 31.0f), 31u);
    uint mask = 0u;
    [loop] for (uint s = sMin; s <= sMax; ++s) { mask |= (1u << s); if (s == 31u) break; }
    return mask;
}

bool SphereIntersectsTileFrustum(float3 centerVS, float radius, FTileFrustum frustum, float tileMinDepth, float tileMaxDepth)
{
    float centerDepth = GetViewDepth(centerVS);
    if (centerDepth + radius < tileMinDepth) return false;
    if (centerDepth - radius > tileMaxDepth) return false;
    if (dot(frustum.LeftNormal, centerVS) < -radius) return false;
    if (dot(frustum.RightNormal, centerVS) < -radius) return false;
    if (dot(frustum.TopNormal, centerVS) < -radius) return false;
    if (dot(frustum.BottomNormal, centerVS) < -radius) return false;
    return true;
}

bool SpherePasses25DMask(float3 centerVS, float radius, float tileMinDepth, float tileMaxDepth, uint tileDepthMask)
{
    float centerDepth = GetViewDepth(centerVS);
    uint lightMask = BuildDepthSliceMask(centerDepth - radius, centerDepth + radius, tileMinDepth, tileMaxDepth);
    if (lightMask == 0u) return false;
    if (tileDepthMask == 0u) return true;
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
    if (ComputeConePlaneMaxDistance(bounds, frustum.LeftNormal, 0.0f) < 0.0f) return false;
    if (ComputeConePlaneMaxDistance(bounds, frustum.RightNormal, 0.0f) < 0.0f) return false;
    if (ComputeConePlaneMaxDistance(bounds, frustum.TopNormal, 0.0f) < 0.0f) return false;
    if (ComputeConePlaneMaxDistance(bounds, frustum.BottomNormal, 0.0f) < 0.0f) return false;
    if (ComputeConePlaneMaxDistance(bounds, float3(1.0f, 0.0f, 0.0f), -tileMinDepth) < 0.0f) return false;
    if (ComputeConePlaneMaxDistance(bounds, float3(-1.0f, 0.0f, 0.0f), tileMaxDepth) < 0.0f) return false;
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
    if (lightMask == 0u) return false;
    if (tileDepthMask == 0u) return true;
    return (lightMask & tileDepthMask) != 0u;
}

[numthreads(FORWARD_PLUS_TILE_SIZE_X, FORWARD_PLUS_TILE_SIZE_Y, 1)]
void TileLightCulling25DCS(uint3 GroupID : SV_GroupID, uint3 GroupThreadID : SV_GroupThreadID, uint GroupIndex : SV_GroupIndex)
{
    uint tileIndex = GroupID.y * TileCount.x + GroupID.x;
    uint2 tilePixelMin = GroupID.xy * uint2(FORWARD_PLUS_TILE_SIZE_X, FORWARD_PLUS_TILE_SIZE_Y);
    uint2 tilePixelMax = min(tilePixelMin + uint2(FORWARD_PLUS_TILE_SIZE_X, FORWARD_PLUS_TILE_SIZE_Y), ScreenSize);

    if (GroupIndex == 0u) {
        gMinDepthBits = asuint(kFloatMax); gMaxDepthBits = asuint(0.0f);
        gTileDepthMask = 0u; gHasValidDepth = 0u;
        gPointLightCount = 0u; gSpotLightCount = 0u;
    }
    GroupMemoryBarrierWithGroupSync();

    uint2 pixelCoord = tilePixelMin + GroupThreadID.xy;
    float viewDepth = 0.0f;
    bool bValidPixel = false;
    if (pixelCoord.x < ScreenSize.x && pixelCoord.y < ScreenSize.y) {
        float deviceDepth = SceneDepth.Load(int3(pixelCoord, 0));
        if (deviceDepth < 1.0f) {
            float3 vPos = ReconstructViewPosition(pixelCoord, deviceDepth);
            viewDepth = GetViewDepth(vPos);
            if (viewDepth > 0.0f) {
                bValidPixel = true;
                InterlockedMin(gMinDepthBits, asuint(viewDepth));
                InterlockedMax(gMaxDepthBits, asuint(viewDepth));
            }
        }
    }
    GroupMemoryBarrierWithGroupSync();

    if (GroupIndex == 0u) {
        if (gMinDepthBits != asuint(kFloatMax)) {
            gHasValidDepth = 1u;
            gTileMinDepth = asfloat(gMinDepthBits);
            gTileMaxDepth = asfloat(gMaxDepthBits);
            FTileFrustum f = BuildTileFrustum(tilePixelMin, tilePixelMax);
            gPlanes[0] = f.LeftNormal; gPlanes[1] = f.RightNormal;
            gPlanes[2] = f.TopNormal; gPlanes[3] = f.BottomNormal;
        } else {
            TilePointLightGrid[tileIndex] = uint2(tileIndex * FORWARD_PLUS_MAX_POINT_LIGHTS_PER_TILE, 0u);
            TileSpotLightGrid[tileIndex] = uint2(tileIndex * FORWARD_PLUS_MAX_SPOT_LIGHTS_PER_TILE, 0u);
        }
    }
    GroupMemoryBarrierWithGroupSync();

    if (gHasValidDepth == 0u) return;

    if (bValidPixel) {
        uint pMask = BuildDepthSliceMask(viewDepth, viewDepth, gTileMinDepth, gTileMaxDepth);
        InterlockedOr(gTileDepthMask, pMask);
    }
    GroupMemoryBarrierWithGroupSync();

    FTileFrustum sharedFrustum;
    sharedFrustum.LeftNormal = gPlanes[0];
    sharedFrustum.RightNormal = gPlanes[1];
    sharedFrustum.TopNormal = gPlanes[2];
    sharedFrustum.BottomNormal = gPlanes[3];

    // Culling
    for (uint i = GroupIndex; i < PointLightCount; i += FORWARD_PLUS_THREAD_COUNT) {
        FPointLightInfo l = PointLights[i];
        float3 cVS = mul(float4(l.Position, 1.0f), View).xyz;
        float r = l.Radius;
        if (!SphereIntersectsTileFrustum(cVS, r, sharedFrustum, gTileMinDepth, gTileMaxDepth)) continue;
        if (bEnable25DMask && !SpherePasses25DMask(cVS, r, gTileMinDepth, gTileMaxDepth, gTileDepthMask)) continue;
        uint wIdx; InterlockedAdd(gPointLightCount, 1u, wIdx);
        if (wIdx < FORWARD_PLUS_MAX_POINT_LIGHTS_PER_TILE) gPointLightIndices[wIdx] = i;
    }
    for (uint j = GroupIndex; j < SpotLightCount; j += FORWARD_PLUS_THREAD_COUNT) {
        FSpotLightInfo sl = SpotLights[j];
        FSpotConeBounds bounds = BuildSpotConeBounds(sl);
        if (!SphereIntersectsTileFrustum(bounds.BroadPhaseCenterVS, bounds.BroadPhaseRadius, sharedFrustum, gTileMinDepth, gTileMaxDepth)) continue;
        if (!ConeIntersectsTileFrustum(bounds, sharedFrustum, gTileMinDepth, gTileMaxDepth)) continue;
        if (bEnable25DMask && !ConePasses25DMask(bounds, gTileMinDepth, gTileMaxDepth, gTileDepthMask)) continue;
        uint wIdx; InterlockedAdd(gSpotLightCount, 1u, wIdx);
        if (wIdx < FORWARD_PLUS_MAX_SPOT_LIGHTS_PER_TILE) gSpotLightIndices[wIdx] = j;
    }
    GroupMemoryBarrierWithGroupSync();

    uint pFinal = min(gPointLightCount, FORWARD_PLUS_MAX_POINT_LIGHTS_PER_TILE);
    uint sFinal = min(gSpotLightCount, FORWARD_PLUS_MAX_SPOT_LIGHTS_PER_TILE);
    uint pOffset = tileIndex * FORWARD_PLUS_MAX_POINT_LIGHTS_PER_TILE;
    uint sOffset = tileIndex * FORWARD_PLUS_MAX_SPOT_LIGHTS_PER_TILE;

    if (GroupIndex == 0u) {
        TilePointLightGrid[tileIndex] = uint2(pOffset, pFinal);
        TileSpotLightGrid[tileIndex] = uint2(sOffset, sFinal);
    }
    for (uint k = GroupIndex; k < pFinal; k += FORWARD_PLUS_THREAD_COUNT) TilePointLightIndices[pOffset + k] = gPointLightIndices[k];
    for (uint m = GroupIndex; m < sFinal; m += FORWARD_PLUS_THREAD_COUNT) TileSpotLightIndices[sOffset + m] = gSpotLightIndices[m];
}
