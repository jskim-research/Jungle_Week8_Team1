#pragma once

#include "Core/CoreTypes.h"
#include "Render/Common/ShadowTypes.h"
// 에디터 UI와 렌더러가 공유하는 view mode 정의다.
// 새 view mode를 추가할 때는 enum만 늘리지 말고 아래 helper 규칙도 함께 확장해야 한다.

enum class EViewMode : int32
{
	Lit = 0,
	Unlit,
	Wireframe,
	SceneDepth,
	WorldNormal,
	Count
};

// 버퍼 기반 시각화 모드 분기는 여기로 모아둔다.
// SceneDepth / WorldNormal 외 다른 buffer visualization을 추가할 때 함께 확장하는 지점이다.
inline bool IsBufferVisualizationViewMode(EViewMode ViewMode)
{
	return ViewMode == EViewMode::SceneDepth || ViewMode == EViewMode::WorldNormal;
}

// composite pass 우회 규칙도 공용 helper에서 관리한다.
// 새 view mode가 decal/fog/fxaa를 건너뛰어야 하면 각 패스를 따로 늘리지 말고 여기서 정의한다.
inline bool ShouldBypassSceneCompositePasses(EViewMode ViewMode)
{
	return ViewMode == EViewMode::Wireframe || IsBufferVisualizationViewMode(ViewMode);
}

enum class EShadowFilterMode : uint8
{
	SSM = 0,
	SSM_PCF = 1,
	VSM = 2
};

constexpr EShadowProjectionMode GetDefaultShadowProjectionMode()
{
	return EShadowProjectionMode::Standard;
}

constexpr EShadowProjectionMode SanitizeShadowProjectionMode(
	int32 ModeValue,
	EShadowProjectionMode Fallback = GetDefaultShadowProjectionMode())
{
	switch (static_cast<EShadowProjectionMode>(ModeValue))
	{
	case EShadowProjectionMode::Standard:
	case EShadowProjectionMode::PSM:
		return static_cast<EShadowProjectionMode>(ModeValue);
	default:
		return Fallback;
	}
}

inline const char* GetShadowProjectionModeDisplayName(EShadowProjectionMode Mode)
{
	switch (Mode)
	{
	case EShadowProjectionMode::Standard:
		return "Standard";
	case EShadowProjectionMode::PSM:
		return "PSM";
	default:
		return "Unknown";
	}
}

constexpr EShadowFilterMode GetDefaultShadowFilterMode()
{
	return EShadowFilterMode::SSM_PCF;
}

constexpr EShadowFilterMode SanitizeShadowFilterMode(int32 ModeValue, EShadowFilterMode Fallback = GetDefaultShadowFilterMode())
{
	switch (static_cast<EShadowFilterMode>(ModeValue))
	{
	case EShadowFilterMode::SSM:
	case EShadowFilterMode::SSM_PCF:
	case EShadowFilterMode::VSM:
		return static_cast<EShadowFilterMode>(ModeValue);
	default:
		return Fallback;
	}
}

inline const char* GetShadowFilterModeDisplayName(EShadowFilterMode Mode)
{
	switch (Mode)
	{
	case EShadowFilterMode::SSM:
		return "SSM";
	case EShadowFilterMode::SSM_PCF:
		return "SSM + PCF";
	case EShadowFilterMode::VSM:
		return "VSM";
	default:
		return "Unknown";
	}
}

struct FShowFlags
{
	bool bPrimitives = true;
	bool bGrid = true;
	bool bAxis = true;
	bool bGizmo = true;
	bool bDirectionalLightDebug = false;
	bool bPointLightDebug = false;
	bool bSpotLightDebug = false;
	bool bBillboardText = false;
	bool bBoundingVolume = false;
	bool bBVHBoundingVolume = false;
	bool bEnableLOD = true;
	bool bDecals = true;
	bool bFog = true;
	bool bShowLightHitmapOverlay = false;
	EShadowProjectionMode ShadowProjection = GetDefaultShadowProjectionMode();
	EShadowFilterMode ShadowFilter = GetDefaultShadowFilterMode();

	bool UsesPSMShadowProjection() const
	{
		return ShadowProjection == EShadowProjectionMode::PSM;
	}

	bool UsesVSMShadowFilter() const
	{
		return ShadowFilter == EShadowFilterMode::VSM;
	}
};

struct FGridRenderSettings
{
	float LineThickness;
	float MajorLineThickness;
	int32 MajorLineInterval;
	float MinorIntensity;
	float MajorIntensity;
	float AxisThickness;
	float AxisIntensity;
	float AxisLengthScale;
};

constexpr FGridRenderSettings MakeDefaultGridRenderSettings()
{
	return {
		1.0f,  // LineThickness
		1.25f, // MajorLineThickness
		10,    // MajorLineInterval
		0.45f, // MinorIntensity
		0.9f,  // MajorIntensity
		1.5f,  // AxisThickness
		1.0f,  // AxisIntensity
		1.0f,  // AxisLengthScale
	};
}

