#include "OpaqueRenderPass.h"
#include "LightCullingPass.h"
#include "Render/Device/D3DDevice.h"
#include "Render/Scene/RenderBus.h"
#include "Render/Resource/RenderResources.h"
#include "Render/Resource/Material.h"
#include "Core/ResourceManager.h"
#include <cstring>

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

    return true;
}

bool FOpaqueRenderPass::DrawCommand(const FRenderPassContext* Context)
{
    const FRenderBus* RenderBus = Context->RenderBus;

    const TArray<FRenderCommand>& Commands = RenderBus->GetCommands(ERenderPass::Opaque);

    if (Commands.empty())
        return true;

    UShader* ShaderOverride = ResolveOpaqueShaderOverride(Context);

    BindLightCullingResources(Context);

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

		BindLightCullingResources(Context);

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
    ID3D11ShaderResourceView* NullSRVs[3] = { nullptr, nullptr, nullptr };
    Context->DeviceContext->PSSetShaderResources(8, 3, NullSRVs);

    ID3D11Buffer* NullCB = nullptr;
    Context->DeviceContext->PSSetConstantBuffers(4, 1, &NullCB);

    return true;
}

bool FOpaqueRenderPass::Release()
{
    VisibleLightConstantBuffer.Reset();
    return true;
}

bool FOpaqueRenderPass::EnsureLightCullingConstantBuffer(ID3D11Device* Device)
{
    if (VisibleLightConstantBuffer)
    {
        return true;
    }

    D3D11_BUFFER_DESC Desc = {};
    Desc.ByteWidth = sizeof(FVisibleLightConstants);
    Desc.Usage = D3D11_USAGE_DYNAMIC;
    Desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    Desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    return SUCCEEDED(Device->CreateBuffer(&Desc, nullptr, VisibleLightConstantBuffer.GetAddressOf()));
}

void FOpaqueRenderPass::BindLightCullingResources(const FRenderPassContext* Context)
{
    const FLightCullingOutputs& Outputs = FLightCullingPass::GetOutputs();

    if (!EnsureLightCullingConstantBuffer(Context->Device))
    {
        return;
    }

    FVisibleLightConstants Constants = {};
    Constants.TileCountX = Outputs.TileCountX;
    Constants.TileCountY = Outputs.TileCountY;
    Constants.TileSize = Outputs.TileSize;
    Constants.MaxLightsPerTile = Outputs.MaxLightsPerTile;
    Constants.LightCount = Outputs.LightCount;

    D3D11_MAPPED_SUBRESOURCE Mapped = {};
    if (SUCCEEDED(Context->DeviceContext->Map(VisibleLightConstantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &Mapped)))
    {
        std::memcpy(Mapped.pData, &Constants, sizeof(Constants));
        Context->DeviceContext->Unmap(VisibleLightConstantBuffer.Get(), 0);
    }

    ID3D11ShaderResourceView* SRVs[3] =
    {
        Outputs.LightBufferSRV,
        Outputs.TileLightCountSRV,
        Outputs.TileLightIndexSRV
    };
    Context->DeviceContext->PSSetShaderResources(8, 3, SRVs);

    ID3D11Buffer* CBuffer = VisibleLightConstantBuffer.Get();
    Context->DeviceContext->PSSetConstantBuffers(4, 1, &CBuffer);
}
