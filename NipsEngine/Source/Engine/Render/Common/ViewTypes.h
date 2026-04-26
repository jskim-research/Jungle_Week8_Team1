#pragma once

#include "Core/CoreTypes.h"

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

struct FShowFlags
{
    bool bPrimitives = true;
    bool bGrid = true;
    bool bAxis = true;
    bool bGizmo = true;
    bool bBillboardText = false;
    bool bBoundingVolume = false;
    bool bBVHBoundingVolume = false;
    bool bEnableLOD = true;
    bool bDecals = true;
    bool bFog = true;
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
