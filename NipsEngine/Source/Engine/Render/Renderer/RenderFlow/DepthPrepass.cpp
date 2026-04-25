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

	//clear
    ID3D11ShaderResourceView* NullSRVs[3] = {};
    Context->DeviceContext->PSSetShaderResources(0, ARRAY_SIZE(NullSRVs), NullSRVs);

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

	ID3D11DepthStencilView* DSV = Context->RenderTargets->DepthStencilView;
    Context->DeviceContext->ClearDepthStencilView(DSV, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
    Context->DeviceContext->OMSetRenderTargets(0, nullptr, DSV);
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
        if (Cmd.Type != ERenderCommandType::Primitive)
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

bool FDepthPrepass::End(const FRenderPassContext* Context)
{
    return false;
}
