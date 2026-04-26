#define LIGHT_CULLING_TILE_SIZE 16
#define LIGHT_CULLING_MAX_LIGHTS_PER_TILE 512
#define THREADS_PER_TILE (LIGHT_CULLING_TILE_SIZE * LIGHT_CULLING_TILE_SIZE) // 256
#define NUM_SLICES                      32

struct FLightDataCS
{
    float3 WorldPos;
    float Radius;
    float3 Color;
    float Intensity;
    float RadiusFalloff;
    uint Type;
    float SpotInnerCos;
    float SpotOuterCos;
    float3 Direction;
    float Padding;
};

cbuffer LightCullingConstants : register(b0)
{
    row_major float4x4 View;
    row_major float4x4 Projection;
    uint LightCount;
    uint TileCountX;
    uint TileCountY;
    uint TileSize;
    float ViewportWidth;
    float ViewportHeight;
    uint IsOrthographic;
    uint Enable25DCulling;
    float NearZ;
    float FarZ;
    float2 Padding;
};

StructuredBuffer<FLightDataCS> SceneLights : register(t0);
Texture2D<float> SceneDepth : register(t1); //predepth

RWStructuredBuffer<uint> TileVisibleLightCount : register(u0);
RWStructuredBuffer<uint> TileVisibleLightIndices : register(u1);
RWStructuredBuffer<uint> CulledPointLightIndexMaskOUT : register(u3); // 전체 OR 누적 (Shadow Map용)
RWStructuredBuffer<uint> PerTilePointLightIndexMaskOut : register(u4); // 타일별 조명 비트마스크

RWTexture2D<float4> DebugHitMap : register(u5); // 히트맵 디버그 텍스처

groupshared uint groupMinZ; // 타일 내 최솟값 (uint reinterpret)
groupshared uint groupMaxZ; // 타일 내 최댓값 (uint reinterpret)
groupshared uint hitCount; // 타일 내 조명 교차 수 (히트맵용)
groupshared uint tileDepthMask; // 타일 내 오브젝트 Depth 비트마스크

// GroupShared: 타일에서 살아남은 라이트 인덱스
groupshared uint gs_VisibleLightCount;
groupshared uint gs_VisibleLightIndices[LIGHT_CULLING_MAX_LIGHTS_PER_TILE];

// 타일당 최대 512개 빛 / 32 = 16개의 버킷 필요
groupshared uint gs_TileLightMask[LIGHT_CULLING_MAX_LIGHTS_PER_TILE / 32];

// GroupShared: 배치 로드용 라이트 캐시
groupshared float4 gs_LightPosRadius[THREADS_PER_TILE]; // xyz=WorldPos, w=Radius

[numthreads(LIGHT_CULLING_TILE_SIZE, LIGHT_CULLING_TILE_SIZE, 1)]
void mainCS(
    uint3 GroupID : SV_GroupID,
    uint3 GroupThreadID : SV_GroupThreadID,
    uint GroupIndex : SV_GroupIndex)       // 0..255, flat thread index
{
    // -------------------------------------------------------
    // 0. 타일 메타데이터
    // -------------------------------------------------------
    const uint TileIndex = GroupID.y * TileCountX + GroupID.x;
    const uint TileStartOffset = TileIndex * LIGHT_CULLING_MAX_LIGHTS_PER_TILE;

    const float2 TileMin = float2(GroupID.xy) * float(TileSize);
    const float2 TileMax = TileMin + float(TileSize);

    uint2 tileCoord = GroupID.xy;
    uint2 pixel = tileCoord * TileSize + GroupThreadID.xy;

    // -------------------------------------------------------
    // 1. GroupShared 초기화 (thread 0만)
    // -------------------------------------------------------
    if (GroupIndex == 0)
    {
        groupMinZ = 0x7f7fffff; // float 0
        groupMaxZ = 0x00000000; // float max
        
        gs_VisibleLightCount = 0;
        hitCount = 0;
        tileDepthMask = 0;

    }
    if (GroupIndex < (LIGHT_CULLING_MAX_LIGHTS_PER_TILE / 32))
    {
        gs_TileLightMask[GroupIndex] = 0;
    }
    GroupMemoryBarrierWithGroupSync();
    
    // --------------------------------------------------------
    // 2. Depth Sampling & MinZ/MaxZ 수집
    // --------------------------------------------------------
    float depthSample = 1.0;
    float linearZ = FarZ;
    
    
    if (Enable25DCulling != 0 && all(pixel < float2(ViewportWidth, ViewportHeight)))
    {
        depthSample = SceneDepth[pixel];
        if (depthSample < 1.0)
        {
            // 비선형 depth → View Space 선형 거리
// Reverse-Z 환경에서의 올바른 View Space Linear Z 변환
            linearZ = (NearZ * FarZ) / (NearZ + depthSample * (FarZ - NearZ));
            InterlockedMin(groupMinZ, asuint(linearZ));
            InterlockedMax(groupMaxZ, asuint(linearZ));
        }
    }
    GroupMemoryBarrierWithGroupSync();
    
    float minZ = NearZ;
    float maxZ = FarZ;
    
    if (Enable25DCulling != 0 && groupMaxZ > groupMinZ)
    {
        minZ = asfloat(groupMinZ);
        maxZ = asfloat(groupMaxZ);
    }
    if (Enable25DCulling != 0 && depthSample < 1.0)
    {
        float rangeZ = maxZ - minZ;
        if (rangeZ < 1e-3)
        {
            // 깊이 분포가 너무 좁으면 전체 범위로 확장 (Dithering 방지)
            minZ = NearZ;
            maxZ = FarZ;
            rangeZ = maxZ - minZ;
        }
        float normZ = saturate((linearZ - minZ) / rangeZ);
        int slice = clamp((int) floor(normZ * NUM_SLICES), 0, NUM_SLICES - 1);
        InterlockedOr(tileDepthMask, 1u << slice);
    }
    GroupMemoryBarrierWithGroupSync();
    
    // -------------------------------------------------------
    // 3. 라이트를 THREADS_PER_TILE 배치로 나눠 협력 컬링
    //    각 스레드가 라이트 1개씩 로드 → 256개씩 처리
    // -------------------------------------------------------

    float2 tileMin = float2(tileCoord * TileSize);
    float2 tileMax = tileMin + float2(TileSize);
    
    // HLSL의 float4x4 생성자는 행(Row) 단위로 값을 채워 넣습니다.
    float4x4 InverseProjection = float4x4(
    1.0f / Projection._11, 0.0f, 0.0f, 0.0f,
    0.0f, 1.0f / Projection._22, 0.0f, 0.0f,
    0.0f, 0.0f, 0.0f, 1.0f / Projection._43,
    0.0f, 0.0f, 1.0f, -Projection._33 / Projection._43
    );
    
    const uint BatchSize = THREADS_PER_TILE; // 256

    for (uint BatchStart = 0; BatchStart < LightCount; BatchStart += BatchSize)
    {
        // --- 2a. 협력 로드: 스레드i → 라이트(BatchStart+i) --- //256개의 쓰레드가 각각 하나의 조명을 담당해서 계산
        const uint LoadIndex = BatchStart + GroupIndex;
        if (LoadIndex < LightCount)
        {
            const FLightDataCS L = SceneLights[LoadIndex];
            gs_LightPosRadius[GroupIndex] = float4(L.WorldPos, L.Radius);
        }
        else
        {
            gs_LightPosRadius[GroupIndex] = float4(0, 0, 0, -1); // 더미 (Radius<0 → 컬링)
        }
        GroupMemoryBarrierWithGroupSync();

        // --- 3b. 배치 내 라이트를 모든 스레드가 나눠서 컬링 ---
        //  thread i → 라이트 i (배치 내)
        //  256스레드 * 1라이트 = 배치 전체 병렬 처리
        const uint LightInBatch = GroupIndex;
        if (LightInBatch < min(BatchSize, LightCount - BatchStart))
        {
            const float4 PosR = gs_LightPosRadius[LightInBatch];
            const float3 WorldPos = PosR.xyz;
            const float Radius = PosR.w;
            const uint GlobalIdx = BatchStart + LightInBatch;

            const float4 ViewPosition = mul(float4(WorldPos, 1.0f), View);
            const float EyeDepth = ViewPosition.x;  //??????????????
            
            if (EyeDepth + Radius <= 1e-4f)
            {
                // 카메라 뒤
                // → 컬링
            }
            // 카메라 구 내부
            else if (EyeDepth < Radius)
            {
                uint Slot;
                InterlockedAdd(gs_VisibleLightCount, 1, Slot);
                if (Slot < LIGHT_CULLING_MAX_LIGHTS_PER_TILE)
                    gs_VisibleLightIndices[Slot] = GlobalIdx;
            }
            else
            {
                const float4 ClipPosition = mul(ViewPosition, Projection);
                if (ClipPosition.w > 0.0f)
                {
                    const float2 Ndc = ClipPosition.xy / ClipPosition.w;
                    const float2 ScreenPos = float2(
                        (Ndc.x * 0.5f + 0.5f) * ViewportWidth,
                        (-Ndc.y * 0.5f + 0.5f) * ViewportHeight);

                    // Orthographic 일 때 Radius 크기 조정 필요 X
                    const float EffectiveDepth = IsOrthographic ? 1 : max(EyeDepth - Radius, 1e-4f);
                    const float YScale = Projection[2][1];
                    const float ProjectedRadius = (Radius / EffectiveDepth) * YScale * (ViewportHeight * 0.5f);
                    
                    if (ProjectedRadius > 0.0f)
                    {
                        const float2 Closest = clamp(ScreenPos, TileMin, TileMax);
                        const float2 Delta = ScreenPos - Closest;
                        const bool bIntersects = dot(Delta, Delta) <= (ProjectedRadius * ProjectedRadius);
                        
                        if (bIntersects)
                        {
                            bool bLightAffectTile;
                        // ============================================================
                        // 2.5D Depth Masking
                        // ============================================================
                        
                        // 조명(구)의 깊이 구간이 타일 오브젝트 깊이 마스크와 겹치는지 판정        
                            if (Enable25DCulling != 0)
                            {
                                if (tileDepthMask == 0)
                                {
                                    bLightAffectTile = false;
                                }
                                else
                                {
                                    float s_minDepth = EyeDepth - Radius;
                                    float s_maxDepth = EyeDepth + Radius;

                        // 타일 깊이 범위 밖이면 컬링
                                    if (s_maxDepth < minZ || s_minDepth > maxZ)
                                    {
                                        bLightAffectTile = false;
                                    }
                                    else
                                    {
                                
                                        float rangeZ = max(1e-5, maxZ - minZ);
                                        float normMin = saturate((s_minDepth - minZ) / rangeZ);
                                        float normMax = saturate((s_maxDepth - minZ) / rangeZ);

                        // 구가 여러 슬라이스에 걸칠 수 있으므로 floor/ceil로 바운드
                                        int sliceMin = clamp((int) floor(normMin * NUM_SLICES), 0, NUM_SLICES - 1);
                                        int sliceMax = clamp((int) ceil(normMax * NUM_SLICES), 0, NUM_SLICES - 1);

                                        uint sphereMask = 0;

                                        for (int i = sliceMin; i <= sliceMax; ++i)
                                            sphereMask |= (1u << i);

                                        bLightAffectTile = (sphereMask & tileDepthMask) != 0;
                            
                                    
                                    }
                                }
                            }
                            
                            if (bLightAffectTile)
                            {
                                uint Slot;
                                InterlockedAdd(gs_VisibleLightCount, 1, Slot);
                                if (Slot < LIGHT_CULLING_MAX_LIGHTS_PER_TILE)
                                    gs_VisibleLightIndices[Slot] = GlobalIdx;
                            
                                uint Bucket = GlobalIdx / 32;
                                uint Bit = GlobalIdx % 32;
                                InterlockedOr(gs_TileLightMask[Bucket], 1u << Bit);
                            }
                        }
                    }
                }
            }
        }
        GroupMemoryBarrierWithGroupSync(); // 배치 완료 후 동기화
    }

    // -------------------------------------------------------
    // 4. 결과를 Global 버퍼에 기록
    //    256스레드가 협력해서 씀 (thread i → slot i)
    // -------------------------------------------------------
    GroupMemoryBarrierWithGroupSync();

    if (GroupIndex == 0)
    {
        TileVisibleLightCount[TileIndex] = min(gs_VisibleLightCount, LIGHT_CULLING_MAX_LIGHTS_PER_TILE);
    }

    const uint FinalCount = min(gs_VisibleLightCount, LIGHT_CULLING_MAX_LIGHTS_PER_TILE);
    for (uint WriteIdx = GroupIndex; WriteIdx < FinalCount; WriteIdx += BatchSize)
    {
        TileVisibleLightIndices[TileStartOffset + WriteIdx] = gs_VisibleLightIndices[WriteIdx];
    }

    const uint BucketsPerTile = LIGHT_CULLING_MAX_LIGHTS_PER_TILE / 32;
    if (GroupIndex < BucketsPerTile)
    {
        uint MaskIndex = TileIndex * BucketsPerTile + GroupIndex;
        PerTilePointLightIndexMaskOut[MaskIndex] = gs_TileLightMask[GroupIndex];
    }
}