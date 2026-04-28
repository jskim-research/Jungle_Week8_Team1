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
        FMatrix ShadowLightView[3] = { FMatrix::Identity, FMatrix::Identity, FMatrix::Identity };
        FMatrix ShadowLightProjection[3] = { FMatrix::Identity, FMatrix::Identity, FMatrix::Identity };
		
		FVector2 UVScale = FVector2(1.0f, 1.0f);
		FVector2 UVOffset = FVector2(0.0f, 0.0f);
		
		FVector ShadowLightPosition = FVector::ZeroVector;
		float ShadowFar = 0.0f;

		float ShadowBias = 0.005f;
		uint32 ShadowMapType = 0; // 0: none, 1: Depth2D, 2: DepthCube
		uint32 SliceCount = 0;
        uint32 ShadowTextureIndex = 0;

        float CascadeSplits[3];
		float Padding0;
	};

	struct FShadowArrayCB
	{
		FShadowCB ShadowDataArray[MAX_SHADOW_LIGHTS];
	};

	static_assert((sizeof(FShadowCB) % 16) == 0, "FShadowCB must remain 16-byte aligned for HLSL constant buffer packing.");

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
