#include "OpaqueRenderPass.h"
#include "LightCullingPass.h"
#include "Render/Device/D3DDevice.h"
#include "Render/Scene/RenderBus.h"
#include "Render/Resource/RenderResources.h"
#include "Render/Resource/Material.h"
#include "Core/ResourceManager.h"
#include "SceneLightBinding.h"
#include "ShadowPass.h"

namespace
{
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
    const FRenderTargetSet* RenderTargets = Context->RenderTargets;
    ID3D11RenderTargetView* RTVs[3] = {
		RenderTargets->SceneColorRTV,
		RenderTargets->SceneNormalRTV,
		RenderTargets->SceneWorldPosRTV
	};
    ID3D11DepthStencilView* DSV = RenderTargets->DepthStencilView;

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

    SceneLightBinding::BindResources(Context, VisibleLightConstantBuffer);

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
        }

		SceneLightBinding::BindResources(Context, VisibleLightConstantBuffer);

		// 테스트용으로 하나만 mapping
		if (!FShadowPass::GetShadowMaps().empty())
		{
            Context->DeviceContext->PSSetShaderResources(11, 1, &FShadowPass::GetShadowMaps()[0].Resource->SRV);

			FShadowCB ShadowCB;
            ShadowCB.ShadowLightView = FShadowPass::GetShadowMaps()[0].Views[0].LightView;
            ShadowCB.ShadowLightProjection = FShadowPass::GetShadowMaps()[0].Views[0].LightProjection;

			D3D11_MAPPED_SUBRESOURCE Mapped = {};
            if (SUCCEEDED(Context->DeviceContext->Map(ShadowConstantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &Mapped)))
            {
                std::memcpy(Mapped.pData, &ShadowCB, sizeof(ShadowCB));
                Context->DeviceContext->Unmap(ShadowConstantBuffer.Get(), 0);
            }

			ID3D11Buffer* RawShadowConstantBuffer = ShadowConstantBuffer.Get();
			Context->DeviceContext->PSSetConstantBuffers(6, 1, &RawShadowConstantBuffer);

			ID3D11SamplerState* ShadowSampler = FResourceManager::Get().GetOrCreateSamplerState(ESamplerType::EST_Point, Context->Device);
            Context->DeviceContext->PSSetSamplers(1, 1, &ShadowSampler);
		}

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

	ID3D11ShaderResourceView* NullSRV[] = { nullptr };
    Context->DeviceContext->PSSetShaderResources(11, 1, NullSRV);
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
