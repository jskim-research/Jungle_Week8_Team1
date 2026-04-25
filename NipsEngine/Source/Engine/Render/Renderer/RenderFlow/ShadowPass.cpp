#include "ShadowPass.h"
#include "Render/Scene/ShadowLightSelector.h"
#include "Core/ResourceManager.h"
#include "Editor/UI/EditorConsoleWidget.h"

bool FShadowPass::Initialize()
{
	return true;
}

bool FShadowPass::Release()
{
    ShaderBinding.reset();
	return true;
}

bool FShadowPass::Begin(const FRenderPassContext* Context)
{
    UShader* Shader = FResourceManager::Get().GetShader("Shaders/Primitive.hlsl");
    if (!ShaderBinding || ShaderBinding->GetShader() != Shader)
    {
        ShaderBinding = Shader->CreateBindingInstance(Context->Device);
    }

    if (!ShaderBinding)
    {
        bSkip = true;
        return true;
    }

	if (!ShadowMaps.empty())
	{
		for (FShadowMap& ShadowMap : ShadowMaps)
		{
            Context->ShadowResourcePool->Release(ShadowMap.Resource);
		}
        ShadowMaps.clear();
	}
    bSkip = false;
	std::vector<FShadowRequest> ShadowRequests = ShadowLightSelector.SelectShadowLights(Context->RenderBus->GetLights());

	if (ShadowRequests.empty())
    {
        bSkip = true;
        return true;
	}

	// 테스트 용도로 첫 번째만 사용
    FCascadeData CascadeData;
    FShadowSlice ShadowSlice;

	FShadowRequestDesc Desc;

	// 테스트 용도로 Directional Light 만 제대로 해놨음
	switch (ShadowRequests[0].Type)
	{
    case ELightType::LightType_Directional:
    {
        Desc.AllocationMode = EShadowAllocationMode::ArrayBased; // CSM
        Desc.MapType = EShadowMapType::Depth2D;
        Desc.Resolution = ShadowRequests[0].Resolution;
        Desc.CascadeCount = 1; // 일단은 한 장씩만 사용

        FVector LightDir = Context->RenderBus->GetLights()[ShadowRequests[0].LightId].Direction;
        LightDir.Normalize();

        // 카메라 기준
        FVector CameraPos = Context->RenderBus->GetCameraPosition();
        FVector Target = CameraPos;

        // 카메라 기준으로 뒤에서 본다
        float Distance = 500.0f; // 임시 (나중에 Frustum 기반으로 바꿔야 함)
        FVector LightPos = Target - LightDir * Distance;

        CascadeData.LightView = FMatrix::MakeViewLookAtLH(LightPos, Target);

        // 테스트 용도로 Orthographic 가정
        CascadeData.LightProjection = FMatrix::Identity;
        CascadeData.SplitDepth = 1000;

        ShadowSlice.Index = 0;
        ShadowSlice.Type = EShadowSliceType::CSM;
        ShadowSlice.UVOffset = FVector2(0, 0);
        ShadowSlice.UVScale = FVector2(1, 1);
        break;
    }
	default:
		// Ambient 등은 따로 처리 안함
        bSkip = true;
        return true;
	}

	FShadowMap ShadowMap;

	ShadowMap.Resource = Context->ShadowResourcePool->Acquire(Context->Device, Desc);
    ShadowMap.MapType = Desc.MapType;
	// 테스트 용도 -> Cascade Count = 1
	ShadowMap.Cascades.push_back(CascadeData);
    ShadowMap.Slices.push_back(ShadowSlice);

	ShadowMaps.push_back(ShadowMap);

	OutSRV = ShadowMap.Resource->SRV;
    OutRTV = nullptr;

    ShaderBinding->ApplyFrameParameters(*Context->RenderBus);
    // ShaderBinding->SetMatrix4("View", CascadeData.LightView);
    // ShaderBinding->SetMatrix4("Projection", CascadeData.LightProjection);

	return true;
}

bool FShadowPass::DrawCommand(const FRenderPassContext* Context)
{
    if (bSkip || !ShaderBinding)
        return true;

    const FRenderBus* RenderBus = Context->RenderBus;

    const TArray<FRenderCommand>& Commands = RenderBus->GetCommands(ERenderPass::Opaque);

    if (Commands.empty())
        return true;

    Context->DeviceContext->ClearDepthStencilView(ShadowMaps[0].Resource->DSVs[0], D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
    Context->DeviceContext->OMSetRenderTargets(0, nullptr, ShadowMaps[0].Resource->DSVs[0]);

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
            UShader* Shader = FResourceManager::Get().GetShader("Shaders/Primitive.hlsl");
            // Cmd.Material->Bind(Context->DeviceContext, Context->RenderBus, &Cmd.PerObjectConstants, Shader, Context);
            ShaderBinding->ApplyPerObjectParameters(Cmd.PerObjectConstants);
            ShaderBinding->Bind(Context->DeviceContext);
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

bool FShadowPass::End(const FRenderPassContext* Context)
{
    if (bSkip)
        return true;
	return true;
}