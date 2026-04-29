#pragma once
#include "Render/Common/ShadowTypes.h"
#include "Render/Scene/RenderCommand.h"
#include "Component/CameraComponent.h"

class UWorld;
class UCameraComponent;

struct FCascadeInfo
{
    float Near;
    float Far;
};

struct FShadowRequest
{
	uint32 LightId;      // 어떤 라이트인지
	ELightType Type; // Directional / Point / Spot

	int32 Resolution; // Shadow Map 해상도
    TArray<FCascadeInfo> Cascades;

	bool bUseVSM = false; // VSM 사용할지 여부
    bool bPSM = false;
	EShadowProjectionMode ProjectionMode = EShadowProjectionMode::Standard;
};

class IShadowLightSelector
{
public:
	virtual TArray<FShadowRequest> 
	SelectShadowLights(const TArray<FRenderLight>& SceneLights, const FVector& CameraPosition, const FCameraState& CameraState) = 0;
};

class FShadowLightSelector : public IShadowLightSelector
{
public:
	TArray<FShadowRequest>
    SelectShadowLights(const TArray<FRenderLight>& SceneLights, const FVector& CameraPosition, const FCameraState& CameraState) override;
};
