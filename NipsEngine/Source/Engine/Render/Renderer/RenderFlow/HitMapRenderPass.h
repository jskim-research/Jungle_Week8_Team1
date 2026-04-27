#pragma once
#include "RenderPass.h"

class FHitMapRenderPass : public FBaseRenderPass
{
public:
    bool Initialize() override;
    bool Release() override;

private:
    bool Begin(const FRenderPassContext* Context) override;
    bool DrawCommand(const FRenderPassContext* Context) override;
    bool End(const FRenderPassContext* Context) override;

    bool EnsureShader(ID3D11Device* Device);

private:
    UShader* HitMapShader = nullptr;
    bool bSkipHitMapDraw = false;
};
