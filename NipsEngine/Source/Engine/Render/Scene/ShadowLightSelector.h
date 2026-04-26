#pragma once
#include "Core/Common.h"
#include "Component/Light/LightComponent.h"
#include "Render/Common/ShadowTypes.h"
#include "Render/Scene/RenderCommand.h"

class UWorld;
class UCameraComponent;

struct FShadowRequest
{
	uint32 LightId;      // 어떤 라이트인지
	ELightType Type; // Directional / Point / Spot

	int Resolution; // Shadow Map 해상도
	uint32 CascadeCount;

	bool bUseVSM; // VSM 사용할지 여부

	EShadowProjectionMode ProjectionMode = EShadowProjectionMode::Default;
};

class IShadowLightSelector
{
public:
	virtual std::vector<FShadowRequest> 
	SelectShadowLights(const TArray<FRenderLight>& SceneLights) = 0;
};

class FShadowLightSelector : public IShadowLightSelector
{
public:
	/*
		아직은 세부 로직이 없어서 SceneLights 만 파라미터로 넣어둠
	*/
	std::vector<FShadowRequest>
	SelectShadowLights(const TArray<FRenderLight>& SceneLights) override;
};
