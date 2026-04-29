#pragma once
#include "RenderPass.h"
#include "Render/Common/ComPtr.h"
#include "Render/Common/ViewTypes.h"
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
        float ShadowSlopeBias = 0.f;
		float ShadowFilterScale = 1.0f;
        float Pad0 = 0.0f;

		uint32 ShadowMapType = 0; // 0: none, 1: Depth2D, 2: DepthCube, 3: VSM2D, 4: VSMCube
		uint32 SliceCount = 0;
        uint32 ShadowTextureIndex = 0;
		uint32 ShadowFilterMode = static_cast<uint32>(EShadowFilterMode::SSM_PCF); // 0: SSM, 1: SSM+PCF, 2: VSM

        float CascadeSplits[3] = { 0.0f, 0.0f, 0.0f };
		float PointShadowTexelSize = 0.0f;

		float VSMDepthBias = 5.0e-4f;
		float VSMMinVariance = 2.0e-5f;
		float VSMLightBleedingReduction = 0.2f;
		float Pad2 = 0.0f;
	};

	struct FShadowArrayCB
	{
		FShadowCB ShadowDataArray[MAX_SHADOW_LIGHTS];
	};

	static_assert((sizeof(FShadowCB) % 16) == 0, "FShadowCB must remain 16-byte aligned for HLSL constant buffer packing.");
	static_assert(sizeof(FShadowCB) == 480, "FShadowCB layout must stay in sync with UberLit.hlsl / BufferVisualizationPass.hlsl.");

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
