#include "DepthPrepassRenderPass.h"

#include "Core/ResourceManager.h"
#include "Render/Resource/Material.h"
#include "Render/Resource/RenderResources.h"
#include "Render/Scene/RenderBus.h"
#include "Render/Scene/RenderCommand.h"

namespace
{
    bool IsDepthPrepassEligible(const FRenderCommand& Cmd)
    {
        if (Cmd.Type != ERenderCommandType::StaticMesh)
        {
            return false;
        }

        if (Cmd.Material == nullptr)
        {
            return true;
        }

        if (Cmd.Material->GetEffectiveMaterialDomain() != EMaterialDomain::Surface)
        {
            return false;
        }

        FMaterialParamValue OpacityValue;
        if (Cmd.Material->GetParam("Opacity", OpacityValue) &&
            OpacityValue.Type == EMaterialParamType::Float &&
            std::get<float>(OpacityValue.Value) < 0.999f)
        {
            return false;
        }

        return true;
    }
}

bool FDepthPrepassRenderPass::Initialize()
{
    return true;
}

bool FDepthPrepassRenderPass::Release()
{
    return true;
}

bool FDepthPrepassRenderPass::Begin(const FRenderPassContext* Context)
{
    if (!Context || !Context->DeviceContext || !Context->RenderTargets)
    {
        return false;
    }

    ID3D11DepthStencilView* DSV = Context->RenderTargets->DepthStencilView;
    Context->DeviceContext->OMSetRenderTargets(0, nullptr, DSV);

    ID3D11DepthStencilState* DepthStencilState =
        FResourceManager::Get().GetOrCreateDepthStencilState(EDepthStencilType::Default);
    ID3D11BlendState* BlendState =
        FResourceManager::Get().GetOrCreateBlendState(EBlendType::NoColor);
    ID3D11RasterizerState* RasterizerState =
        FResourceManager::Get().GetOrCreateRasterizerState(ERasterizerType::SolidBackCull);

    Context->DeviceContext->OMSetDepthStencilState(DepthStencilState, 0);
    Context->DeviceContext->OMSetBlendState(BlendState, nullptr, 0xFFFFFFFF);
    Context->DeviceContext->RSSetState(RasterizerState);
    Context->DeviceContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    OutSRV = PrevPassSRV;
    OutRTV = PrevPassRTV;
    return true;
}

bool FDepthPrepassRenderPass::DrawCommand(const FRenderPassContext* Context)
{
    if (!Context || !Context->DeviceContext || !Context->RenderBus)
    {
        return false;
    }

    const TArray<FRenderCommand>& Commands = Context->RenderBus->GetCommands(ERenderPass::Opaque);
    if (Commands.empty())
    {
        return true;
    }

    UShader* DepthShader = FResourceManager::Get().GetShader("Shaders/DepthPrepass.hlsl");
    if (DepthShader == nullptr)
    {
        return false;
    }

    ID3D11DepthStencilState* DepthStencilState =
        FResourceManager::Get().GetOrCreateDepthStencilState(EDepthStencilType::Default);
    ID3D11BlendState* NoColorBlendState =
        FResourceManager::Get().GetOrCreateBlendState(EBlendType::NoColor);
    ID3D11RasterizerState* DefaultRasterizerState =
        FResourceManager::Get().GetOrCreateRasterizerState(ERasterizerType::SolidBackCull);

    ID3D11DeviceContext* DeviceContext = Context->DeviceContext;

    // Set initial states once to reduce redundant calls
    DeviceContext->OMSetDepthStencilState(DepthStencilState, 0);
    DeviceContext->OMSetBlendState(NoColorBlendState, nullptr, 0xFFFFFFFF);
    DeviceContext->RSSetState(DefaultRasterizerState);
    DeviceContext->PSSetShader(nullptr, nullptr, 0);

    for (const FRenderCommand& Cmd : Commands)
    {
        if (!IsDepthPrepassEligible(Cmd))
        {
            continue;
        }

        if (Cmd.MeshBuffer == nullptr || !Cmd.MeshBuffer->IsValid())
        {
            continue;
        }

        uint32 Offset = 0;
        ID3D11Buffer* VertexBuffer = Cmd.MeshBuffer->GetVertexBuffer().GetBuffer();
        if (VertexBuffer == nullptr)
        {
            continue;
        }

        const uint32 VertexCount = Cmd.MeshBuffer->GetVertexBuffer().GetVertexCount();
        const uint32 Stride = Cmd.MeshBuffer->GetVertexBuffer().GetStride();
        if (VertexCount == 0 || Stride == 0)
        {
            continue;
        }

        if (Cmd.Material != nullptr)
        {
            Cmd.Material->Bind(DeviceContext, Context->RenderBus, &Cmd.PerObjectConstants, DepthShader, Context);

            // Re-apply states that Material::Bind might have changed, ensuring consistency (#2)
            DeviceContext->OMSetDepthStencilState(DepthStencilState, 0);
            DeviceContext->OMSetBlendState(NoColorBlendState, nullptr, 0xFFFFFFFF);
            // Force the default RS to ensure consistency across all objects (#2)
            DeviceContext->RSSetState(DefaultRasterizerState);
            DeviceContext->PSSetShader(nullptr, nullptr, 0);
        }

        DeviceContext->IASetVertexBuffers(0, 1, &VertexBuffer, &Stride, &Offset);

        ID3D11Buffer* IndexBuffer = Cmd.MeshBuffer->GetIndexBuffer().GetBuffer();
        if (IndexBuffer != nullptr)
        {
            DeviceContext->IASetIndexBuffer(IndexBuffer, DXGI_FORMAT_R32_UINT, 0);
            DeviceContext->DrawIndexed(Cmd.SectionIndexCount, Cmd.SectionIndexStart, 0);
        }
        else
        {
            DeviceContext->Draw(VertexCount, 0);
        }
    }

    return true;
}

bool FDepthPrepassRenderPass::End(const FRenderPassContext* Context)
{
    if (!Context || !Context->DeviceContext)
    {
        return false;
    }

    Context->DeviceContext->PSSetShader(nullptr, nullptr, 0);
    return true;
}
