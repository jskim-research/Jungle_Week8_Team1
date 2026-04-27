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

struct FPointLightInfo { float3 Position; float Radius; float3 Color; float Intensity; };
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
    frustum.LeftNormal   = SafeNormalize(cross(pBL, pTL));
    frustum.RightNormal  = SafeNormalize(cross(pTR, pBR));
    frustum.TopNormal    = SafeNormalize(cross(pTL, pTR));
    frustum.BottomNormal = SafeNormalize(cross(pBR, pBL));
    return frustum;
}

uint BuildDepthSliceMask(float minZ, float maxZ, float tileMinZ, float tileMaxZ)
{
    float clampedMin = max(minZ, tileMinZ);
    float clampedMax = min(maxZ, tileMaxZ);
    if (clampedMax < clampedMin) return 0u;
    float extent = tileMaxZ - tileMinZ;
    if (extent <= kEpsilon) return 0xFFFFFFFF;

    float nMin = saturate((clampedMin - tileMinZ) / extent);
    float nMax = saturate((clampedMax - tileMinZ) / extent);
    uint sMin = min((uint)floor(nMin * 31.0f), 31u);
    uint sMax = min((uint)ceil(nMax * 31.0f), 31u);
    uint mask = 0u;
    [loop] for (uint s = sMin; s <= sMax; ++s) { mask |= (1u << s); if (s == 31u) break; }
    return mask;
}

[numthreads(FORWARD_PLUS_TILE_SIZE_X, FORWARD_PLUS_TILE_SIZE_Y, 1)]
void TileLightCulling25DCS(uint3 GroupID : SV_GroupID, uint3 GroupThreadID : SV_GroupThreadID, uint GroupIndex : SV_GroupIndex)
{
    uint tileIndex = GroupID.y * TileCount.x + GroupID.x;
    uint2 tilePixelMin = GroupID.xy * 16;
    uint2 tilePixelMax = min(tilePixelMin + 16, ScreenSize);

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

    // Culling
    for (uint i = GroupIndex; i < PointLightCount; i += FORWARD_PLUS_THREAD_COUNT) {
        FPointLightInfo l = PointLights[i];
        float3 cVS = mul(float4(l.Position, 1.0f), View).xyz;
        float r = l.Radius;
        float dZ = GetViewDepth(cVS);
        if (dZ + r < gTileMinDepth || dZ - r > gTileMaxDepth) continue;
        if (dot(gPlanes[0], cVS) < -r || dot(gPlanes[1], cVS) < -r || dot(gPlanes[2], cVS) < -r || dot(gPlanes[3], cVS) < -r) continue;
        if (bEnable25DMask) {
            uint lMask = BuildDepthSliceMask(dZ - r, dZ + r, gTileMinDepth, gTileMaxDepth);
            if ((lMask & gTileDepthMask) == 0u) continue;
        }
        uint wIdx; InterlockedAdd(gPointLightCount, 1u, wIdx);
        if (wIdx < FORWARD_PLUS_MAX_POINT_LIGHTS_PER_TILE) gPointLightIndices[wIdx] = i;
    }
    // Spot light culling simplified for brevity, similar to point
    for (uint j = GroupIndex; i < SpotLightCount; j += FORWARD_PLUS_THREAD_COUNT) {
        FSpotLightInfo sl = SpotLights[j];
        float3 sVS = mul(float4(sl.Position, 1.0f), View).xyz;
        if (GetViewDepth(sVS) + sl.Radius < gTileMinDepth || GetViewDepth(sVS) - sl.Radius > gTileMaxDepth) continue;
        if (dot(gPlanes[0], sVS) < -sl.Radius || dot(gPlanes[1], sVS) < -sl.Radius || dot(gPlanes[2], sVS) < -sl.Radius || dot(gPlanes[3], sVS) < -sl.Radius) continue;
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
