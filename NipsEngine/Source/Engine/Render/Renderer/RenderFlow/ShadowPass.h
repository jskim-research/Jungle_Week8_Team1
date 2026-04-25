#pragma once

#include "RenderPass.h"

class FShadowPass : public FBaseRenderPass
{
    bool Initialize();
    bool Release();

    bool Begin(const FRenderPassContext* Context);
    bool DrawCommand(const FRenderPassContext* Context);
    bool End(const FRenderPassContext* Context);
};
