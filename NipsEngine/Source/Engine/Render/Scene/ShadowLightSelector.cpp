#include "ShadowLightSelector.h"

std::vector<FShadowRequest> FShadowLightSelector::SelectShadowLights(const TArray<FRenderLight>& SceneLights)
{
    std::vector<FShadowRequest> SelectedLights;

	if (SceneLights.empty())
        return SelectedLights;

	/**
	 * Test 용으로 맨 처음 Light 만 Req 에 포함 
	 */

	for (int i=0;i < SceneLights.size();i++)
    {
        FShadowRequest Req;
        Req.LightId = i;
        Req.Type = (ELightType)SceneLights[i].Type;
        Req.Resolution = i==0? 1024 : 512;
        Req.ProjectionMode = EShadowProjectionMode::Default;
        Req.CascadeCount = 1;
        Req.bUseVSM = false;

        SelectedLights.push_back(Req);
    }
	
    return SelectedLights;
}
