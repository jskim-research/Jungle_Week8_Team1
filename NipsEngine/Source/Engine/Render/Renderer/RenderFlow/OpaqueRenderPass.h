#pragma once
#include "RenderPass.h"
#include "Render/Common/ComPtr.h"
#define MAX_SHADOW_LIGHTS 32
class FOpaqueRenderPass : public FBaseRenderPass
{
public:
    struct FShadowCB
    {
        FMatrix ShadowLightView;
        FMatrix ShadowLightProjection;
        FVector2 UVScale;
        FVector2 UVOffset;
        uint32 SliceIndex;
        FVector Pad; 
    };

    struct FShadowArrayCB
    {
        FShadowCB ShadowDataArray[MAX_SHADOW_LIGHTS];
    };



	bool Initialize() override;
	bool Release() override;

protected:
	bool Begin(const FRenderPassContext* Context) override;
	bool DrawCommand(const FRenderPassContext* Context) override;
	bool End(const FRenderPassContext* Context) override;
	bool EnsureShadowConstantBuffer(ID3D11Device* Device);

private:
	TComPtr<ID3D11Buffer> VisibleLightConstantBuffer;
	TComPtr<ID3D11Buffer> ShadowConstantBuffer;
};
