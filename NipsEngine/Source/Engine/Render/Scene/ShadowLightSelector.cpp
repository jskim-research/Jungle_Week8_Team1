#include "ShadowLightSelector.h"

TArray<FShadowRequest> FShadowLightSelector::SelectShadowLights(const TArray<FRenderLight>& SceneLights)
{
	TArray<FShadowRequest> SelectedLights;

	if (SceneLights.empty())
		return SelectedLights;

	constexpr uint32 MaxAtlasShadowCount = 16;
	constexpr uint32 MaxPointShadowCount = 1;

	uint32 AtlasShadowCount = 0;
	uint32 PointShadowCount = 0;

	for (uint32 LightIndex = 0; LightIndex < static_cast<uint32>(SceneLights.size()); ++LightIndex)
	{
		const ELightType Type = static_cast<ELightType>(SceneLights[LightIndex].Type);
		if (Type == ELightType::LightType_AmbientLight || Type == ELightType::Max)
		{
			continue;
		}

		if (Type == ELightType::LightType_Point)
		{
			if (PointShadowCount >= MaxPointShadowCount)
			{
				continue;
			}

			++PointShadowCount;
		}
		else
		{
			if (AtlasShadowCount >= MaxAtlasShadowCount)
			{
				continue;
			}

			++AtlasShadowCount;
		}

		FShadowRequest Req;
		Req.LightId = LightIndex;
		Req.Type = Type;
		Req.Resolution = 1024;
		Req.ProjectionMode = EShadowProjectionMode::Default;
		Req.CascadeCount = 1;
		Req.bUseVSM = false;
		SelectedLights.push_back(Req);
	}
	
	return SelectedLights;
}
