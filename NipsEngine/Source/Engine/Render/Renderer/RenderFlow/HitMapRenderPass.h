#pragma once
#include "RenderPass.h"
#include "Render/Common/ComPtr.h"

class FHitMapRenderPass : public FBaseRenderPass
{
public:
    bool Initialize() override;
    bool Release() override;

private:
    bool Begin(const FRenderPassContext* Context) override;
    bool DrawCommand(const FRenderPassContext* Context) override;
    bool End(const FRenderPassContext* Context) override;

    bool EnsureResources(ID3D11Device* Device);

    TComPtr<ID3D11VertexShader> VertexShader;
    TComPtr<ID3D11PixelShader> PixelShader;
};