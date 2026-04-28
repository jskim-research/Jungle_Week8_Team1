#pragma once
#include "RenderPass.h"
#include "Render/Common/ComPtr.h"
#include "Math/Vector2.h"
#define MAX_SHADOW_LIGHTS 32
class FOpaqueRenderPass : public FBaseRenderPass
{
public:
	struct FShadowCB
	{
		FMatrix ShadowLightView = FMatrix::Identity;
		FMatrix ShadowLightProjection = FMatrix::Identity;
		FVector2 UVScale = FVector2(1.0f, 1.0f);
		FVector2 UVOffset = FVector2(0.0f, 0.0f);
		FVector ShadowLightPosition = FVector::ZeroVector;
		float ShadowFar = 0.0f;
		float ShadowBias = 0.005f;
		uint32 ShadowMapType = 0; // 0: none, 1: Depth2D, 2: DepthCube
		uint32 SliceIndex = 0;
		uint32 Padding0 = 0;
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
