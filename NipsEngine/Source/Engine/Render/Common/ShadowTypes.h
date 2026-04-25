#pragma once

enum class EShadowProjectionMode
{
    Default,      // Light 기반 자동 결정 (Directional->Ortho, Point/Spot->Perspective)
    Orthographic, // 강제 Ortho
    Perspective   // 강제 Perspective
};

// 각 Shadow DepthMap Texture 형태
enum class EShadowSliceType
{
    Atlas,
    CubeFace,
    CSM
};

// 그림자를 “어떤 방식(알고리즘/샘플링 구조)”으로 계산할지 정의
enum class EShadowMapType
{
    Depth2D, // 일반 2D depth shadow map.
             // Spot light, Directional light (CSM 각 slice), planar shadow 등에서 사용.
             // 가장 기본적인 shadow sampling (depth 비교 기반).

    DepthCube, // Point light용 cube shadow map.
               // 6개 face (±X, ±Y, ±Z)로 구성되며 방향 벡터 기반으로 샘플링.
               // omnidirectional shadow에 사용.

    VSM2D // Variance Shadow Map (2nd moment 포함).
          // depth + depth^2를 저장하여 soft shadow/PCF 근사 가능.
          // light bleeding 발생 가능성이 있음.
};

// 그림자를 “GPU 리소스에 어떻게 배치/공유할지” 정의
enum class EShadowAllocationMode
{
    PerLight, // Light 1개 = Texture 1개.
              // 가장 단순한 구조. 디버깅 쉽고 aliasing 없음.
              // 대신 memory cost 가장 큼.

    AtlasPacked, // 여러 shadow를 하나의 큰 texture atlas에 packing.
                 // UV offset/scale로 접근.
                 // memory 효율 좋지만 packing/fragmentation 관리 필요.

    ArrayBased, // Texture2DArray 또는 TextureCubeArray 사용.
                // index 기반 접근 (cascade index, face index).
                // GPU 접근 단순하고 안정적.

    // Dedicated 는 추후 확장용
    Dedicated // 특정 shadow (예: main directional CSM 등)
              // 전용 리소스로 고정 생성 (reallocation 없음).
              // engine-critical shadow에 사용.
};

struct FShadowSlice
{
    EShadowSliceType Type;
    // FShadowMap 의 Cascade Index => Cascades[Index] 가 LightViewProj 등 보관
    uint32 Index; // Cascade / Face / Atlas slot index (Type 에 따라 분기)

    FVector2 UVOffset;
    FVector2 UVScale;
};

struct FCascadeData
{
    FMatrix LightViewProj;
    float SplitDepth;
};

struct FShadowMap
{
    // 기본 1개
    std::vector<FCascadeData> Cascades;

    // 소유권은 Renderer Resource Pool 이 가짐
    FShadowResource* Resource;

    std::vector<FShadowSlice> Slices;

    EShadowMapType MapType; // Shader Sampling 방식
};