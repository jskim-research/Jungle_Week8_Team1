#include "DepthPrepass.h"
#include "Core/ResourceManager.h"
#include "Render/Scene/RenderBus.h"
#include "Render/Scene/RenderCommand.h"

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif


bool FDepthPrepass::Initialize()
{
    return true;
}

bool FDepthPrepass::Release()
{
    ShaderBinding.reset();
    return true;
}

bool FDepthPrepass::Begin(const FRenderPassContext* Context)
{
    bSkipDepthDraw = false;

    // Output pass-through for the pipeline
    OutSRV = Context->RenderTargets->SceneColorSRV;
    OutRTV = Context->RenderTargets->SceneColorRTV;

    const TArray<FRenderCommand>& Commands = Context->RenderBus->GetCommands(ERenderPass::Opaque);
    if (Commands.empty())
    {
        bSkipDepthDraw = true;
        return true;
    }

    UShader* DepthPassShader = FResourceManager::Get().GetShader("Shaders/DepthPrepass.hlsl");
    if (!DepthPassShader)
    {
        bSkipDepthDraw = true;
        return true;
    }

    if (!ShaderBinding || ShaderBinding->GetShader() != DepthPassShader)
    {
        ShaderBinding = DepthPassShader->CreateBindingInstance(Context->Device);
    }

    if (!ShaderBinding)
    {
        bSkipDepthDraw = true;
        return true;
    }

    // Apply Frame Constants (View, Projection, etc.)
    ShaderBinding->ApplyFrameParameters(*Context->RenderBus);

    // Clear depth/stencil
    ID3D11DepthStencilView* DSV = Context->RenderTargets->DepthStencilView;
    Context->DeviceContext->ClearDepthStencilView(DSV, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
    
    // Bind DSV and unbind RTVs for depth-only pass
    Context->DeviceContext->OMSetRenderTargets(0, nullptr, DSV);

    // Set States explicitly
    ID3D11DepthStencilState* DSState = FResourceManager::Get().GetOrCreateDepthStencilState(EDepthStencilType::Default);
    Context->DeviceContext->OMSetDepthStencilState(DSState, 0);

    ID3D11RasterizerState* RSState = FResourceManager::Get().GetOrCreateRasterizerState(ERasterizerType::SolidBackCull);
    Context->DeviceContext->RSSetState(RSState);

    // Set default blend state (Opaque)
    ID3D11BlendState* BlendState = FResourceManager::Get().GetOrCreateBlendState(EBlendType::Opaque);
    Context->DeviceContext->OMSetBlendState(BlendState, nullptr, 0xFFFFFFFF);

    // Unbind SRVs to avoid hazards if they were bound in previous passes
    ID3D11ShaderResourceView* NullSRVs[8] = {};
    Context->DeviceContext->PSSetShaderResources(0, 8, NullSRVs);
    Context->DeviceContext->VSSetShaderResources(0, 8, NullSRVs);

    DepthPassShader->Bind(Context->DeviceContext);
    Context->DeviceContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    return true;
}

bool FDepthPrepass::DrawCommand(const FRenderPassContext* Context)
{
    if (bSkipDepthDraw || !ShaderBinding)
    {
        return true;
    }

    const TArray<FRenderCommand>& Commands = Context->RenderBus->GetCommands(ERenderPass::Opaque);
    if (Commands.empty())
    {
        return true;
    }

    for (const FRenderCommand& Cmd : Commands)
    {
        // Draw both generic primitives and static meshes that are in the opaque pass
        if (Cmd.Type != ERenderCommandType::Primitive && Cmd.Type != ERenderCommandType::StaticMesh)
        {
            continue;
        }

        if (Cmd.MeshBuffer == nullptr || !Cmd.MeshBuffer->IsValid())
        {
            continue;
        }

        // Only surface materials should participate in scene depth prepass
        if (Cmd.Material && Cmd.Material->GetEffectiveMaterialDomain() != EMaterialDomain::Surface)
        {
            continue;
        }

        // Update and bind per-object constants (Model matrix etc.)
        ShaderBinding->ApplyPerObjectParameters(Cmd.PerObjectConstants);
        ShaderBinding->Bind(Context->DeviceContext);

        uint32 offset = 0;
        ID3D11Buffer* vertexBuffer = Cmd.MeshBuffer->GetVertexBuffer().GetBuffer();
        uint32 stride = Cmd.MeshBuffer->GetVertexBuffer().GetStride();
        
        Context->DeviceContext->IASetVertexBuffers(0, 1, &vertexBuffer, &stride, &offset);

        CheckOverrideViewMode(Context);

        ID3D11Buffer* indexBuffer = Cmd.MeshBuffer->GetIndexBuffer().GetBuffer();
        if (indexBuffer != nullptr)
        {
            Context->DeviceContext->IASetIndexBuffer(indexBuffer, DXGI_FORMAT_R32_UINT, 0);
            Context->DeviceContext->DrawIndexed(Cmd.SectionIndexCount, Cmd.SectionIndexStart, 0);
        }
        else
        {
            Context->DeviceContext->Draw(Cmd.MeshBuffer->GetVertexBuffer().GetVertexCount(), 0);
        }
    }

    return true;
}

bool FDepthPrepass::End(const FRenderPassContext* Context)
{
    return true;
}
