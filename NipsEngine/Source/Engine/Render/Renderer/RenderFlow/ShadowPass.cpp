#include "ShadowPass.h"
#include "Render/Scene/ShadowLightSelector.h"
#include "Core/ResourceManager.h"
#include "Editor/UI/EditorConsoleWidget.h"

namespace
{
	// 현재 Pass 간 Input, Output 연결 구조가 아니어서 전역으로 놓았는데, 나중에 바꿔야 함
	TArray<FShadowMap> GShadowMaps;
}

bool FShadowPass::Initialize()
{
	return true;
}

bool FShadowPass::Release()
{
    ShaderBinding.reset();
	return true;
}

TArray<FShadowMap>& FShadowPass::GetShadowMaps()
{
    return GShadowMaps;
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

	if (!GShadowMaps.empty())
	{
		for (FShadowMap& ShadowMap : GShadowMaps)
		{
            Context->ShadowResourcePool->Release(ShadowMap.Resource);
		}
        GShadowMaps.clear();
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
	        GShadowMaps.push_back(ShadowMap);
	}

	if (GShadowMaps.empty())
	{
        bSkip = true;
        return true;
	}

	OutSRV = GShadowMaps[0].Resource->SRV;
    OutRTV = nullptr;

    ShaderBinding->ApplyFrameParameters(*Context->RenderBus);
	// 만약 Shadow Pass 만 도는 경우 Light 첫 번째를 기준으로 시각화 용도
    ShaderBinding->SetMatrix4("View", GShadowMaps[0].Views[0].LightView);
    ShaderBinding->SetMatrix4("Projection", GShadowMaps[0].Views[0].LightProjection);

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

	// 이전 뷰포트 설정 저장
	D3D11_VIEWPORT oldVP[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
    UINT oldVPCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    Context->DeviceContext->RSGetViewports(&oldVPCount, oldVP);
	
    D3D11_VIEWPORT ShadowViewport = {};
    ShadowViewport.TopLeftX = 0.0f;
    ShadowViewport.TopLeftY = 0.0f;
    ShadowViewport.Width = (float)GShadowMaps[0].Resource->Resolution;
    ShadowViewport.Height = (float)GShadowMaps[0].Resource->Resolution;
    ShadowViewport.MinDepth = 0.0f;
    ShadowViewport.MaxDepth = 1.0f;
    Context->DeviceContext->RSSetViewports(1, &ShadowViewport);

	for (uint32 i = 0; i < GShadowMaps[0].Resource->DSVs.size(); i++)
	{

        Context->DeviceContext->ClearDepthStencilView(GShadowMaps[0].Resource->DSVs[i], D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
        Context->DeviceContext->OMSetRenderTargets(0, nullptr, GShadowMaps[0].Resource->DSVs[i]);

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
	}

	// 상태 복구
	Context->DeviceContext->RSSetViewports(oldVPCount, oldVP);

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
    if (!BuildViews(Context, Req, OutShadowMap.Views))
        return false;
    if (!BuildSlices(Context, Req, OutShadowMap.Slices))
        return false;
    OutShadowMap.MapType = Desc.MapType;

    return true;
}

bool FShadowPass::BuildViews(const FRenderPassContext* Context, const FShadowRequest& Req, TArray<FShadowViewInfo>& OutViewInfoArray)
{
	switch (Req.Type)
	{
    case ELightType::LightType_Directional:
        for (uint32 i = 0; i < Req.CascadeCount; i++)
        {
            FRenderLight Light = Context->RenderBus->GetLights()[Req.LightId];
            const FCameraState& Cam = Context->RenderBus->GetCameraState();

            float Near = Cam.NearZ;
            float Far = Cam.FarZ;
            float FovY = Cam.FOV;
            float Aspect = Cam.AspectRatio;

            FVector CamPos = Context->RenderBus->GetCameraPosition();
            FVector CamForward = Context->RenderBus->GetCameraForward();
            FVector CamRight = Context->RenderBus->GetCameraRight();
            FVector CamUp = Context->RenderBus->GetCameraUp();

            // Directional Light 의 경우 Light.Direction = Directional Light 의 반대 방향 벡터이다.
            // 반대로 Spot/Point Light 는 그대로 forward 벡터임
            FVector LightDir = Light.Direction; // normalize 되어 있어야 함

            FVector Up = FVector(0, 0, 1);
            if (abs(FVector::DotProduct(LightDir, Up)) > 0.99f)
            {
                Up = FVector(1, 0, 0);
            }

            // -----------------------------------
            // 3. Frustum Corner 계산
            // -----------------------------------
            float NearH = 2.0f * Near * tanf(FovY * 0.5f);
            float NearW = NearH * Aspect;

            float FarH = 2.0f * Far * tanf(FovY * 0.5f);
            float FarW = FarH * Aspect;

            FVector NearCenter = CamPos + CamForward * Near;
            FVector FarCenter = CamPos + CamForward * Far;

            FVector FrustumCorners[8];

            // Near
            FrustumCorners[0] = NearCenter + CamUp * (NearH * 0.5f) - CamRight * (NearW * 0.5f);
            FrustumCorners[1] = NearCenter + CamUp * (NearH * 0.5f) + CamRight * (NearW * 0.5f);
            FrustumCorners[2] = NearCenter - CamUp * (NearH * 0.5f) - CamRight * (NearW * 0.5f);
            FrustumCorners[3] = NearCenter - CamUp * (NearH * 0.5f) + CamRight * (NearW * 0.5f);

            // Far
            FrustumCorners[4] = FarCenter + CamUp * (FarH * 0.5f) - CamRight * (FarW * 0.5f);
            FrustumCorners[5] = FarCenter + CamUp * (FarH * 0.5f) + CamRight * (FarW * 0.5f);
            FrustumCorners[6] = FarCenter - CamUp * (FarH * 0.5f) - CamRight * (FarW * 0.5f);
            FrustumCorners[7] = FarCenter - CamUp * (FarH * 0.5f) + CamRight * (FarW * 0.5f);

            FVector FrustumWorldCenter = FVector(0, 0, 0);
            for (int j = 0; j < 8; j++)
            {
                FrustumWorldCenter += FrustumCorners[j];
            }
            FrustumWorldCenter *= (1.0f / 8.0f);

            // Radius
            float AABBRadius = 0.0f;
            for (int j = 0; j < 8; j++)
            {
                float Dist = (FrustumCorners[j] - FrustumWorldCenter).Size();
                AABBRadius = std::max(AABBRadius, Dist);
            }

            FVector Eye = FrustumWorldCenter + LightDir * AABBRadius;
            FVector Target = FrustumWorldCenter;

            //FVector Eye = LightDir * AABBRadius;
            //FVector Target = FVector(0, 0, 0);

            FMatrix LightView = FMatrix::MakeViewLookAtLH(Eye, Target, Up);
            FShadowViewInfo ViewInfo;
            ViewInfo.LightView = LightView;

            // -----------------------------------
            // 4. Light Space 변환
            // -----------------------------------
            FVector FrustumCornersLS[8];
            for (int j = 0; j < 8; j++)
            {
                FrustumCornersLS[j] = LightView.TransformPosition(FrustumCorners[j]);
            }

            // -----------------------------------
            // 5. AABB 계산
            // -----------------------------------
            FVector Min = FrustumCornersLS[0];
            FVector Max = FrustumCornersLS[0];

            for (int j = 1; j < 8; j++)
            {
                Min.X = std::min(Min.X, FrustumCornersLS[j].X);
                Min.Y = std::min(Min.Y, FrustumCornersLS[j].Y);
                Min.Z = std::min(Min.Z, FrustumCornersLS[j].Z);
                Max.X = std::max(Max.X, FrustumCornersLS[j].X);
                Max.Y = std::max(Max.Y, FrustumCornersLS[j].Y);
                Max.Z = std::max(Max.Z, FrustumCornersLS[j].Z);
            }

            // =========================
            // 6. Projection
            // =========================
            float NewFar = AABBRadius + (Max.Z - Min.Z) / 2;
            ViewInfo.LightProjection = FMatrix::MakeOrthographicLH(
                Max.X - Min.X,
                Max.Y - Min.Y,
                0,
                Max.Z + abs(Min.Z));


            ViewInfo.SplitDepth = Far; // 일단 전체 (CSM 전 단계)

            OutViewInfoArray.push_back(ViewInfo);
        }
		break;

	case ELightType::LightType_Spot:
        for (uint32 i = 0; i < Req.CascadeCount; i++)
        {
            FRenderLight Light = Context->RenderBus->GetLights()[Req.LightId];
            FShadowViewInfo ViewInfo;

            FVector LightDir = Light.Direction; // normalize 되어 있어야 함

            FVector Eye = Light.Position;
            FVector Target = Eye + Light.Direction;
            FVector Up = FVector(0, 0, 1);

            if (abs(FVector::DotProduct(LightDir, Up)) > 0.99f)
            {
                Up = FVector(1, 0, 0); // X-Forward니까 X로 대체
            }

            ViewInfo.LightView = FMatrix::MakeViewLookAtLH(Eye, Target, Up);
            ViewInfo.SplitDepth = Context->RenderBus->GetCameraState().FarZ;

			float OuterAngleRad = acos(Light.SpotOuterCos); // 반각(half angle)
            float FovRad = OuterAngleRad * 2.0f;            // 전체 FOV

            ViewInfo.LightProjection = FMatrix::MakePerspectiveFovLH(
                FovRad,
                1.0f,        // 정사각형 섀도우 맵
                1.0f,        // Near
                Light.Radius // Far = 라이트 반경
            );

            OutViewInfoArray.push_back(ViewInfo);
        }
        break;

	case ELightType::LightType_Point:
		for (uint32 i = 0; i < 6; i++)
		{
            static FVector CubeDirs[6] = {
                FVector::UpVector,
                -FVector::UpVector,
                FVector::ForwardVector,
                -FVector::ForwardVector,
                FVector::RightVector,
                -FVector::RightVector
            };

            FRenderLight Light = Context->RenderBus->GetLights()[Req.LightId];
            FShadowViewInfo ViewInfo;

            FVector LightDir = CubeDirs[i];

            FVector Eye = Light.Position;
            FVector Target = Eye + LightDir;

            FVector Up = FVector(0, 0, 1);
            if (abs(FVector::DotProduct(LightDir, Up)) > 0.99f)
            {
                Up = FVector(1, 0, 0); // X-Forward니까 X로 대체
            }

            ViewInfo.LightView = FMatrix::MakeViewLookAtLH(Eye, Target, Up);
            ViewInfo.SplitDepth = Context->RenderBus->GetCameraState().FarZ;

            float FovRad = (90 * (3.141592 / 180));          // 전체 FOV

            ViewInfo.LightProjection = FMatrix::MakePerspectiveFovLH(
                FovRad,
                1.0f,        // 정사각형 섀도우 맵
                1.0f,        // Near
                Light.Radius // Far = 라이트 반경
            );

            OutViewInfoArray.push_back(ViewInfo);
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
            ShadowSlice.Index = i;
            ShadowSlice.Type = EShadowSliceType::CSM;
            ShadowSlice.UVOffset = FVector2(0, 0);
            ShadowSlice.UVScale = FVector2(1, 1);
            OutShadowSlices.push_back(ShadowSlice);
		}
        break;
    case ELightType::LightType_Point:
		// Point Light 는 CSM 고려 X
		for (uint32 i = 0; i < 6; i++)
		{
            FShadowSlice ShadowSlice;
            ShadowSlice.Index = i;
            ShadowSlice.Type = EShadowSliceType::CubeFace;
            ShadowSlice.UVOffset = FVector2(0, 0);
            ShadowSlice.UVScale = FVector2(1, 1);
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
