#include "ShadowLightSelector.h"

TArray<FShadowRequest> FShadowLightSelector::SelectShadowLights(const TArray<FRenderLight>& SceneLights)
{
	TArray<FShadowRequest> SelectedLights;

	if (SceneLights.empty())
		return SelectedLights;

	int32 SelectedLightId = -1;
	ELightType SelectedType = ELightType::Max;

	// Point light shadow를 우선 선택하고 없으면 Spot, Directional 순으로 fallback
	for (uint32 i = 0; i < static_cast<uint32>(SceneLights.size()); ++i)
	{
		const ELightType Type = static_cast<ELightType>(SceneLights[i].Type);
		if (Type == ELightType::LightType_Point)
		{
			SelectedLightId = static_cast<int32>(i);
			SelectedType = Type;
			break;
		}

		if (Type == ELightType::LightType_Spot && SelectedLightId < 0)
		{
			SelectedLightId = static_cast<int32>(i);
			SelectedType = Type;
			continue;
		}

		if (Type == ELightType::LightType_Directional && SelectedLightId < 0)
		{
			SelectedLightId = static_cast<int32>(i);
			SelectedType = Type;
		}
	}

	if (SelectedLightId < 0 || SelectedType == ELightType::Max)
	{
		return SelectedLights;
	}

	FShadowRequest Req;
    Req.LightId = static_cast<uint32>(SelectedLightId);
    Req.Type = SelectedType;
    Req.Resolution = 2048;
    Req.ProjectionMode = EShadowProjectionMode::Default;
    Req.CascadeCount = 1;
    Req.bUseVSM = false;

	SelectedLights.push_back(Req);
	
	return SelectedLights;
}
