#pragma once

#include "RenderPass.h"
#include "Render/Scene/ShadowLightSelector.h"

class FShadowPass : public FBaseRenderPass
{
public:
    bool Initialize();
    bool Release();

protected:
    bool Begin(const FRenderPassContext* Context);
    bool DrawCommand(const FRenderPassContext* Context);
    bool End(const FRenderPassContext* Context);

private:
    FShadowLightSelector ShadowLightSelector;
    TArray<FShadowMap> ShadowMaps;
    bool bSkip = false;
};
