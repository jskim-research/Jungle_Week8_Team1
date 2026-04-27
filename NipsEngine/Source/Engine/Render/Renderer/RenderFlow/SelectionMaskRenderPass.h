#pragma once
#include "RenderPass.h"
#include <memory>

class FShaderBindingInstance;

class FSelectionMaskRenderPass : public FBaseRenderPass
{
public:
    bool Initialize() override;
    bool Release() override;

private:
    bool Begin(const FRenderPassContext* Context) override;
    bool DrawCommand(const FRenderPassContext* Context) override;
    bool End(const FRenderPassContext* Context) override;

private:
    std::shared_ptr<FShaderBindingInstance> ShaderBinding;
    std::shared_ptr<FShaderBindingInstance> BillboardShaderBinding;

    ID3D11DepthStencilState* PrevDepthStencilState = nullptr;
    UINT PrevStencilRef = 0;

    ID3D11BlendState* PrevBlendState = nullptr;
    FLOAT PrevBlendFactor[4];
    UINT PrevSampleMask = 0;

    ID3D11RasterizerState* PrevRasterizerState = nullptr;
};
