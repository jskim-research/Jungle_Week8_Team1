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

	/***************/
    /*  Selection  */
    /***************/
    std::vector<FShadowRequest> ShadowRequests = ShadowLightSelector.SelectShadowLights(Context->RenderBus->GetLights());

	if (ShadowRequests.empty())
    {
        bSkip = true;
        return true;
	}

	/****************/
    /*  Allocation  */
    /****************/
    for (const FShadowRequest& ShadowRequest : ShadowRequests)
	{
        FShadowMap ShadowMap;
		if (MakeShadowMap(Context, ShadowRequest, ShadowMap))
	        ShadowMaps.push_back(ShadowMap);
	}

	if (ShadowMaps.empty())
	{
        bSkip = true;
        return true;
	}

	OutSRV = ShadowMaps[0].Resource->SRV;
    OutRTV = nullptr;

    ShaderBinding->ApplyFrameParameters(*Context->RenderBus);
    ShaderBinding->SetMatrix4("View", ShadowMaps[0].Cascades[0].LightView);
    ShaderBinding->SetMatrix4("Projection", ShadowMaps[0].Cascades[0].LightProjection);

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

bool FShadowPass::MakeShadowMap(const FRenderPassContext* Context, const FShadowRequest& Req, FShadowMap& OutShadowMap)
{
    FShadowRequestDesc Desc;
    Desc.AllocationMode = EShadowAllocationMode::ArrayBased; // CSM
    Desc.MapType = EShadowMapType::Depth2D;
    Desc.Resolution = Req.Resolution;
    Desc.CascadeCount = 1; // 일단은 한 장씩만 사용

    if (!AcquireResource(Context, Desc, &OutShadowMap.Resource))
        return false;
    if (!BuildCascades(Context, Req, OutShadowMap.Cascades))
        return false;
    if (!BuildSlices(Context, Req, OutShadowMap.Slices))
        return false;
    OutShadowMap.MapType = Desc.MapType;

    return true;
}

bool FShadowPass::BuildCascades(const FRenderPassContext* Context, const FShadowRequest& Req, TArray<FCascadeData>& OutCascadeDataArray)
{
	switch (Req.Type)
	{
    case ELightType::LightType_Directional:
        for (uint32 i = 0; i < Req.CascadeCount; i++)
        {
            FRenderLight Light = Context->RenderBus->GetLights()[Req.LightId];
            FCascadeData CascadeData;

			FVector LightDir = Light.Direction; // normalize 되어 있어야 함

            FVector Center = Context->RenderBus->GetCameraPosition();
            /**
             * 테스트용으로 넣어뒀고, 실제로는 Scene AABB 크기에 맞춰서 설정해야함 
             */
            FVector Eye = Center - LightDir * 100.0f; // 뒤에서 바라보게
            FVector Target = Context->RenderBus->GetCameraPosition();


			FVector Up = FVector(0, 0, 1);
            if (abs(FVector::DotProduct(LightDir, Up)) > 0.99f)
            {
                Up = FVector(1, 0, 0); // X-Forward니까 X로 대체
            }

            CascadeData.LightView = FMatrix::MakeViewLookAtLH(Eye, Target, Up);
            float OrthoSize = 1024.0f;
            CascadeData.LightProjection = FMatrix::MakeOrthographicLH(
                OrthoSize, // Width
                OrthoSize, // Height
                1.0f,      // Near
                2000.0f    // Far (Eye에서 500 뒤에 있으니 충분히 크게)
            );

            CascadeData.SplitDepth = 1000;

			OutCascadeDataArray.push_back(CascadeData);
        }
		break;

	case ELightType::LightType_Spot:
        for (uint32 i = 0; i < Req.CascadeCount; i++)
        {
            FRenderLight Light = Context->RenderBus->GetLights()[Req.LightId];
            FCascadeData CascadeData;

            FVector LightDir = Light.Direction; // normalize 되어 있어야 함

            FVector Eye = Light.Position;
            FVector Target = Eye + Light.Direction;

            FVector Up = FVector(0, 0, 1);
            if (abs(FVector::DotProduct(LightDir, Up)) > 0.99f)
            {
                Up = FVector(1, 0, 0); // X-Forward니까 X로 대체
            }

            CascadeData.LightView = FMatrix::MakeViewLookAtLH(Eye, Target, Up);
            CascadeData.SplitDepth = 1000;

			float OuterAngleRad = acos(Light.SpotOuterCos); // 반각(half angle)
            float FovRad = OuterAngleRad * 2.0f;            // 전체 FOV

            CascadeData.LightProjection = FMatrix::MakePerspectiveFovLH(
                FovRad,
                1.0f,        // 정사각형 섀도우 맵
                1.0f,        // Near
                Light.Radius // Far = 라이트 반경
            );

            OutCascadeDataArray.push_back(CascadeData);
        }
        break;
    default:
        return false;
	}
	
	return true;
}

bool FShadowPass::BuildSlices(const FRenderPassContext* Context, const FShadowRequest& Req, TArray<FShadowSlice>& OutShadowSlices)
{
	switch (Req.Type)
	{
    case ELightType::LightType_Directional:
		for (uint32 i = 0; i < Req.CascadeCount; i++)
        {
            FShadowSlice ShadowSlice;
			// CSM 일 경우 Index 은 Cascade Index
            ShadowSlice.Index = i;
            ShadowSlice.Type = EShadowSliceType::CSM;
            ShadowSlice.UVOffset = FVector2(0, 0);
            ShadowSlice.UVScale = FVector2(1, 1);
            OutShadowSlices.push_back(ShadowSlice);
		}
		break;

	case ELightType::LightType_Spot:
		for (uint32 i = 0; i < Req.CascadeCount; i++)
		{
            FShadowSlice ShadowSlice;
            // CSM 일 경우 Index 은 Cascade Index
            ShadowSlice.Index = i;
            ShadowSlice.Type = EShadowSliceType::CSM;
            ShadowSlice.UVOffset = FVector2(0, 0);
            ShadowSlice.UVScale = FVector2(1, 1);
            OutShadowSlices.push_back(ShadowSlice);
		}
        break;
	default:
        return false;
	}

	return true;
}

bool FShadowPass::AcquireResource(const FRenderPassContext* Context, const FShadowRequestDesc& Desc, FShadowResource** OutShadowResource)
{
    *OutShadowResource = Context->ShadowResourcePool->Acquire(Context->Device, Desc);
    return true;
}
