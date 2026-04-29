#pragma once
#include "Math/Matrix.h"
#include "Math/Vector2.h"
#include "Render/Common/RenderTypes.h"
#include "Render/Resource/ShadowResource.h"

enum class EShadowProjectionMode : uint8
{
	Standard = 0,
	PSM = 1
};

// 각 Shadow DepthMap Texture 형태
enum class EShadowSliceType
{
	Atlas,
	CubeFace,
	CSM
};

// 그림자를 “어떤 방식(알고리즘/샘플링 구조)”으로 계산할지 정의
// 주의: 값은 HLSL shadow map type 상수(UberLit.hlsl)와 반드시 일치해야 함.
enum class EShadowMapType
{
	None = 0,

	Depth2D = 1, // 일반 2D depth shadow map.
			 // Spot light, Directional light (CSM 각 slice), planar shadow 등에서 사용.
			 // 가장 기본적인 shadow sampling (depth 비교 기반).

	DepthCube = 2, // Point light용 cube shadow map.
			   // 6개 face (±X, ±Y, ±Z)로 구성되며 방향 벡터 기반으로 샘플링.
			   // omnidirectional shadow에 사용.

	VSM2D = 3, // Variance Shadow Map for 2D/CSM shadows.
		   // depth + depth^2를 저장하여 soft shadow/PCF 근사 가능.
		   // light bleeding 발생 가능성이 있음.

	VSMCube = 4 // Point light용 VSM cubemap shadow.
};

static_assert(
	static_cast<uint32>(EShadowMapType::None) == 0 &&
	static_cast<uint32>(EShadowMapType::Depth2D) == 1 &&
	static_cast<uint32>(EShadowMapType::DepthCube) == 2 &&
	static_cast<uint32>(EShadowMapType::VSM2D) == 3 &&
	static_cast<uint32>(EShadowMapType::VSMCube) == 4,
	"EShadowMapType values must match the shadow map constants in UberLit.hlsl.");

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

// GPU에 그리는 최소 단위 (draw 1회)
struct FShadowSlice
{
	EShadowSliceType Type = EShadowSliceType::Atlas;
	// FShadowMap.Views[Index] (e.g., Cubemap 일 경우 6개)
	uint32 Index = 0;

	FVector2 UVOffset = FVector2(0.0f, 0.0f);
	FVector2 UVScale = FVector2(1.0f, 1.0f);
	uint32 LightId = 0xFFFFFFFF;
	uint32 SourceLightSlotIndex = 0xFFFFFFFF;
};

/*추후에 unio으로 묶어도 됨
union
{
    // 1번 그룹: 일반 섀도우 (Uniform / CSM 등) 용도
    struct
    {
        FMatrix LightView;
        FMatrix LightProjection;
    };

    // 2번 그룹: PSM 전용 용도
    struct
    {
        FMatrix PostPerspectiveViewProjection;
        FMatrix VirtualCameraViewProjection;
    };
};
*/
struct FShadowViewInfo
{
	FMatrix LightView;
	FMatrix LightProjection;

	//PSM용 데이터
	FMatrix PostPerspectiveViewProjection;
    FMatrix VirtualCameraViewProjection;

	// viewspace z (linearlized)
	/*
	if (depth < split[0]) use cascade 0;
	else if (depth < split[1]) use cascade 1;
	else if (depth < split[2]) use cascade 2;
	else use cascade 3;

	Cascade 랑 Cube View Info 들을 합치면서 View Info 에 병합됨
	CSM 을 따로 안 쓰면 SplitDepth = Far;
	*/
	float SplitDepth = 0.f;
};

struct FShadowMap
{
	// 기본 1개
	TArray<FShadowViewInfo> Views;

	// 소유권은 Renderer Resource Pool 이 가짐
	FShadowResource* Resource = nullptr;

	TArray<FShadowSlice> Slices;

	EShadowMapType MapType = EShadowMapType::None; // Shader Sampling 방식
	uint32 LightId = 0;
	uint32 SourceLightSlotIndex = 0xFFFFFFFF;
	uint32 ResourceSliceOffset = 0;
	bool bOwnsResource = true;
	ELightType LightType = ELightType::Max;
};
