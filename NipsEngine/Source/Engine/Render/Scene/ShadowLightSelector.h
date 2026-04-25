#pragma once
#include "Core/Common.h"
#include "Component/Light/LightComponent.h"
#include "Render/Common/ShadowTypes.h"


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
    SelectShadowLights(const Scene& scene, const Camera& camera) = 0;
};