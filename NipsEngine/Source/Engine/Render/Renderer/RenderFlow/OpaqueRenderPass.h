#pragma once
#include "RenderPass.h"
#include "Render/Common/ComPtr.h"

class FOpaqueRenderPass : public FBaseRenderPass
{
public:
	struct FShadowCB
	{
		FMatrix ShadowLightView;
		FMatrix ShadowLightProjection;
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
