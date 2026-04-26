#include "ShadowLightSelector.h"

std::vector<FShadowRequest> FShadowLightSelector::SelectShadowLights(const TArray<FRenderLight>& SceneLights)
{
	std::vector<FShadowRequest> SelectedLights;

	if (SceneLights.empty())
		return SelectedLights;

	/**
	 * Test 용으로 맨 처음 Light 만 Req 에 포함 
	 */
	FShadowRequest Req;
	Req.LightId = 0;
	Req.Type = (ELightType)SceneLights[0].Type;
	Req.Resolution = 1024;
	Req.ProjectionMode = EShadowProjectionMode::Default;
	Req.CascadeCount = 1;
	Req.bUseVSM = false;

	SelectedLights.push_back(Req);
	
	return SelectedLights;
}
