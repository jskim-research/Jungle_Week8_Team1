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
		FMatrix ShadowCubeViewProjection[6];
		FVector ShadowLightPosition;
		float ShadowFar = 0.0f;
		float ShadowBias = 0.005f;
		uint32 ShadowMapType = 0; // 0: none, 1: Depth2D, 2: DepthCube
		uint32 ShadowedVisibleLightIndex = 0xffffffffu;
		uint32 bShadowEnabled = 0;
		uint32 Padding0 = 0;
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
