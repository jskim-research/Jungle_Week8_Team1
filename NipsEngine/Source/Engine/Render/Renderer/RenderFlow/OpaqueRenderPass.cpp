#include "OpaqueRenderPass.h"
#include "LightCullingPass.h"
#include "Render/Device/D3DDevice.h"
#include "Render/Scene/RenderBus.h"
#include "Render/Resource/RenderResources.h"
#include "Render/Resource/Material.h"
#include "Core/ResourceManager.h"
#include "SceneLightBinding.h"
#include "ShadowPass.h"
#include <algorithm>

namespace
{
	constexpr uint32 ShadowMapType_None = 0u;
	constexpr uint32 ShadowMapType_Depth2D = 1u;
	constexpr uint32 ShadowMapType_DepthCube = 2u;
	constexpr uint32 InvalidVisibleLightIndex = 0xffffffffu;

	bool IsLocalLightType(uint32 Type)
	{
		return Type == static_cast<uint32>(ELightType::LightType_Point) ||
			   Type == static_cast<uint32>(ELightType::LightType_Spot);
	}

	uint32 FindVisibleLocalLightIndex(const FRenderBus& RenderBus, uint32 ShadowLightId)
	{
		const TArray<FRenderLight>& Lights = RenderBus.GetLights();
		if (ShadowLightId >= static_cast<uint32>(Lights.size()))
		{
			return InvalidVisibleLightIndex;
		}

		uint32 LocalIndex = 0;
		for (uint32 LightIndex = 0; LightIndex < static_cast<uint32>(Lights.size()); ++LightIndex)
		{
			if (!IsLocalLightType(Lights[LightIndex].Type))
			{
				continue;
			}

			if (LightIndex == ShadowLightId)
			{
				return LocalIndex;
			}

			++LocalIndex;
		}

		return InvalidVisibleLightIndex;
	}

	UShader* ResolveOpaqueShaderOverride(const FRenderPassContext* Context)
	{
		if (!Context || !Context->RenderBus)
		{
			return nullptr;
		}

		if (Context->RenderBus->GetViewMode() != EViewMode::Unlit)
		{
			return nullptr;
		}

		return FResourceManager::Get().GetShader("Shaders/UberUnlit.hlsl");
	}
}

bool FOpaqueRenderPass::Initialize()
{
	return true;
}

bool FOpaqueRenderPass::Begin(const FRenderPassContext* Context)
{
	if (!Context || !Context->RenderTargets || !Context->DeviceContext)
	{
		return false;
	}

	const FRenderTargetSet* RenderTargets = Context->RenderTargets;
	ID3D11RenderTargetView* RTVs[3] = {
		RenderTargets->SceneColorRTV,
		RenderTargets->SceneNormalRTV,
		RenderTargets->SceneWorldPosRTV
	};
	ID3D11DepthStencilView* DSV = RenderTargets->DepthStencilView;

	// Re-bind targets here to ensure we are not affected by previous pass's unbinding
	Context->DeviceContext->OMSetRenderTargets(ARRAYSIZE(RTVs), RTVs, DSV);
	
	OutSRV = RenderTargets->SceneColorSRV;
	OutRTV = RenderTargets->SceneColorRTV;

	Context->DeviceContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

	if (!EnsureShadowConstantBuffer(Context->Device))
	{
		return false;
	}

	return true;
}

bool FOpaqueRenderPass::DrawCommand(const FRenderPassContext* Context)
{
	const FRenderBus* RenderBus = Context->RenderBus;
	const TArray<FRenderCommand>& Commands = RenderBus->GetCommands(ERenderPass::Opaque);

	if (Commands.empty())
		return true;

    UShader* ShaderOverride = ResolveOpaqueShaderOverride(Context);
    ID3D11DepthStencilState* ReadOnlyDepthStencilState =
        FResourceManager::Get().GetOrCreateDepthStencilState(EDepthStencilType::DepthReadOnly);

	SceneLightBinding::BindResources(Context, VisibleLightConstantBuffer);
	
	// Initial state setup before loop
	Context->DeviceContext->OMSetDepthStencilState(ReadOnlyDepthStencilState, 0);

	FShadowCB ShadowCB = {};
	ShadowCB.ShadowLightView = FMatrix::Identity;
	ShadowCB.ShadowLightProjection = FMatrix::Identity;
	for (uint32 FaceIndex = 0; FaceIndex < 6; ++FaceIndex)
	{
		ShadowCB.ShadowCubeViewProjection[FaceIndex] = FMatrix::Identity;
	}
	ShadowCB.ShadowMapType = ShadowMapType_None;
	ShadowCB.ShadowedVisibleLightIndex = InvalidVisibleLightIndex;

	ID3D11ShaderResourceView* ShadowMap2DSRV = nullptr;
	ID3D11ShaderResourceView* ShadowMapCubeSRV = nullptr;

	if (!FShadowPass::GetShadowMaps().empty())
	{
		const FShadowMap& ShadowMap = FShadowPass::GetShadowMaps()[0];
		if (ShadowMap.Resource != nullptr)
		{
			ShadowCB.bShadowEnabled = 1;
			ShadowCB.ShadowBias = 0.005f;

			if (ShadowMap.MapType == EShadowMapType::DepthCube)
			{
				ShadowMapCubeSRV = ShadowMap.Resource->SRV;
				ShadowCB.ShadowMapType = ShadowMapType_DepthCube;

				if (Context->RenderBus && ShadowMap.LightId < static_cast<uint32>(Context->RenderBus->GetLights().size()))
				{
					const FRenderLight& ShadowLight = Context->RenderBus->GetLights()[ShadowMap.LightId];
					ShadowCB.ShadowLightPosition = ShadowLight.Position;
					ShadowCB.ShadowFar = ShadowLight.Radius;
					ShadowCB.ShadowedVisibleLightIndex = FindVisibleLocalLightIndex(*Context->RenderBus, ShadowMap.LightId);
				}

				const uint32 CubeViewCount = std::min<uint32>(6, static_cast<uint32>(ShadowMap.Views.size()));
				for (uint32 FaceIndex = 0; FaceIndex < CubeViewCount; ++FaceIndex)
				{
					ShadowCB.ShadowCubeViewProjection[FaceIndex] =
						ShadowMap.Views[FaceIndex].LightView * ShadowMap.Views[FaceIndex].LightProjection;
				}
			}
			else
			{
				ShadowMap2DSRV = ShadowMap.Resource->SRV;
				ShadowCB.ShadowMapType = ShadowMapType_Depth2D;

				if (!ShadowMap.Views.empty())
				{
					ShadowCB.ShadowLightView = ShadowMap.Views[0].LightView;
					ShadowCB.ShadowLightProjection = ShadowMap.Views[0].LightProjection;
				}
			}
		}
	}

	D3D11_MAPPED_SUBRESOURCE Mapped = {};
	if (SUCCEEDED(Context->DeviceContext->Map(ShadowConstantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &Mapped)))
	{
		std::memcpy(Mapped.pData, &ShadowCB, sizeof(ShadowCB));
		Context->DeviceContext->Unmap(ShadowConstantBuffer.Get(), 0);
	}

	ID3D11SamplerState* ShadowSampler = FResourceManager::Get().GetOrCreateSamplerState(ESamplerType::EST_Point, Context->Device);

	for (const FRenderCommand& Cmd : Commands)
	{
		if (Cmd.Type == ERenderCommandType::PostProcessOutline)
		{
			continue;
		}

		if (Cmd.MeshBuffer == nullptr || !Cmd.MeshBuffer->IsValid())
		{
			return false;
		}

		uint32 offset = 0;
		ID3D11Buffer* vertexBuffer = Cmd.MeshBuffer->GetVertexBuffer().GetBuffer();
		if (vertexBuffer == nullptr)
		{
			return false;
		}

		uint32 vertexCount = Cmd.MeshBuffer->GetVertexBuffer().GetVertexCount();
		uint32 stride = Cmd.MeshBuffer->GetVertexBuffer().GetStride();
		if (vertexCount == 0 || stride == 0)
		{
			return false;
		}

		if (Cmd.Material)
		{
			Cmd.Material->Bind(Context->DeviceContext, Context->RenderBus, &Cmd.PerObjectConstants, ShaderOverride, Context);
			
			// VERY IMPORTANT: Material::Bind might have its own DS state (e.g. for translucent or special materials).
			// We MUST force ReadOnly (LESS_EQUAL) for Opaque pass to work with Depth Prepass.
			Context->DeviceContext->OMSetDepthStencilState(ReadOnlyDepthStencilState, 0);
		}

		SceneLightBinding::BindResources(Context, VisibleLightConstantBuffer);

		// Material bind가 texture 슬롯을 다시 덮어쓸 수 있으므로 shadow 리소스는 draw 직전 재바인딩한다.
		ID3D11ShaderResourceView* ShadowSRVs[2] = { ShadowMap2DSRV, ShadowMapCubeSRV };
		Context->DeviceContext->PSSetShaderResources(14, 2, ShadowSRVs);

		ID3D11Buffer* RawShadowConstantBuffer = ShadowConstantBuffer.Get();
		Context->DeviceContext->PSSetConstantBuffers(6, 1, &RawShadowConstantBuffer);
		Context->DeviceContext->PSSetSamplers(1, 1, &ShadowSampler);

		CheckOverrideViewMode(Context);

		Context->DeviceContext->IASetVertexBuffers(0, 1, &vertexBuffer, &stride, &offset);

		ID3D11Buffer* indexBuffer = Cmd.MeshBuffer->GetIndexBuffer().GetBuffer();
		if (indexBuffer != nullptr)
		{
			uint32 indexStart = Cmd.SectionIndexStart;
			uint32 indexCount = Cmd.SectionIndexCount;
			Context->DeviceContext->IASetIndexBuffer(indexBuffer, DXGI_FORMAT_R32_UINT, 0);
			Context->DeviceContext->DrawIndexed(indexCount, indexStart, 0);
		}
		else
		{
			Context->DeviceContext->Draw(vertexCount, 0);
		}
	}

	return true;
}

bool FOpaqueRenderPass::End(const FRenderPassContext* Context)
{
	SceneLightBinding::UnbindResources(Context ? Context->DeviceContext : nullptr);

	ID3D11ShaderResourceView* NullSRVs[2] = { nullptr, nullptr };
	Context->DeviceContext->PSSetShaderResources(14, 2, NullSRVs);
	ID3D11Buffer* NullShadowCB = nullptr;
	Context->DeviceContext->PSSetConstantBuffers(6, 1, &NullShadowCB);
	return true;
}

bool FOpaqueRenderPass::EnsureShadowConstantBuffer(ID3D11Device* Device)
{
	if (ShadowConstantBuffer)
		return true;

	HRESULT Result;
	D3D11_BUFFER_DESC Desc = {};
	Desc.ByteWidth = sizeof(FShadowCB);
	Desc.Usage = D3D11_USAGE_DYNAMIC;
	Desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	Desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

	Result = Device->CreateBuffer(&Desc, nullptr, ShadowConstantBuffer.GetAddressOf());

	return SUCCEEDED(Result);
}

bool FOpaqueRenderPass::Release()
{
	VisibleLightConstantBuffer.Reset();
	ShadowConstantBuffer.Reset();
	return true;
}
