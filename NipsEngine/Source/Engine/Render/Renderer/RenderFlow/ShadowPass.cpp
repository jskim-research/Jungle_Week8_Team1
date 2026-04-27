#include "ShadowPass.h"
#include "Render/Scene/ShadowLightSelector.h"
#include "Core/ResourceManager.h"
#include "Editor/UI/EditorConsoleWidget.h"
#include <algorithm>

#define ATLAS_SIZE 4096
namespace
{
// 현재 Pass 간 Input, Output 연결 구조가 아니어서 전역으로 놓았는데, 나중에 바꿔야 함
TArray<FShadowMap> GShadowMaps;

// 1. LightId -> ShadowDataArray Index (0~31) 매핑 테이블
TArray<int32> GLightToShadowIndices;
// 2. OpaquePass에 넘겨줄 상수 버퍼 데이터
FOpaqueRenderPass::FShadowArrayCB GShadowCBData;

} // namespace

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

const TArray<int32>& FShadowPass::GetLightToShadowIndices()
{
    return GLightToShadowIndices;
}
const FOpaqueRenderPass::FShadowArrayCB& FShadowPass::GetShadowCBData()
{
    return GShadowCBData;
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

    // LightType별로 모아두기 (atlas용)
    std::array<std::vector<FShadowRequest>, static_cast<size_t>(ELightType::Max)> buckets;

    for (auto& req : ShadowRequests)
    {
        buckets[static_cast<int>(req.Type)].push_back(req);
    }
    // 버킷별로 내림차순 정렬
    for (int t = 0; t < static_cast<size_t>(ELightType::Max); ++t)
    {
        std::sort(buckets[t].begin(), buckets[t].end(),
                  [](const FShadowRequest& a, const FShadowRequest& b)
                  {
                      return a.Resolution > b.Resolution; // 무조건 큰 놈부터!
                  });
    }
    // 정렬이 완료된 버킷들을 다시 하나의 순차 배열로 합치기
    ShadowRequests.clear();
    for (int t = 0; t < static_cast<size_t>(ELightType::Max); ++t)
    {
        for (const auto& req : buckets[t])
        {
            ShadowRequests.push_back(req);
        }
    }

    // 원본 인덱스 저장한 룩업테이블
    struct FLightShadowMappingInfo
    {
        bool bHasShadow = false;
        uint32 ShadowMapIndex = 0; // GShadowMaps 배열에서의 인덱스
        uint32 SliceIndex = 0;     // 해당 ShadowMap 내부 Slices 배열에서의 인덱스
    };

    // 원본 라이트 개수만큼 매핑 테이블 할당
    TArray<FLightShadowMappingInfo> ShadowLookupTable(Context->RenderBus->GetLights().size());

	GLightToShadowIndices.assign(Context->RenderBus->GetLights().size(), -1);
    std::memset(&GShadowCBData, 0, sizeof(GShadowCBData));

    uint32 ShadowIndexCounter = 0; // GPU 버퍼 배열에 들어갈 인덱스 (0 ~ 31)

    AtlasAllocator.Reset();

    // 기존의 범위 기반 for 문을 인덱스 기반으로 교체
    for (int i = 0; i < ShadowRequests.size(); ++i)
    {
        const FShadowRequest& ShadowRequest = ShadowRequests[i];

        if (ShadowIndexCounter >= MAX_SHADOW_LIGHTS)
            break;

        if (ShadowRequest.Type == ELightType::LightType_Spot || ShadowRequest.Type == ELightType::LightType_Directional)
        {
            // 1. 공간 할당 가능?
            FAtlasAllocationResult AllocResult;
            if (!AtlasAllocator.Allocate(ShadowRequest.Resolution, AllocResult))
            {
                // 공간 부족 시 새 아틀라스 생성 후 재할당
                FShadowRequestDesc Desc;
                Desc.AllocationMode = EShadowAllocationMode::AtlasPacked;
                Desc.MapType = EShadowMapType::Depth2D;
                Desc.Resolution = ATLAS_SIZE;
                Desc.CascadeCount = 1;

                FShadowResource* NewAtlasRes = nullptr;
                if (AcquireResource(Context, Desc, &NewAtlasRes))
                {
                    AtlasAllocator.AddNewAtlasResource(NewAtlasRes);

                    FShadowMap NewAtlasMap;
                    NewAtlasMap.Resource = NewAtlasRes;
                    NewAtlasMap.MapType = EShadowMapType::Depth2D;
                    GShadowMaps.push_back(NewAtlasMap);

                    uint32 NewAtlasIndex = GShadowMaps.size() - 1;
                    AtlasAllocator.SetCurrentAtlasIndex(NewAtlasIndex);
                    AtlasAllocator.Allocate(ShadowRequest.Resolution, AllocResult);
                }
            }
            uint32 AtlasIndex = AtlasAllocator.GetCurrentAtlasIndex();
            FShadowMap& CurrentAtlasMap = GShadowMaps[AtlasIndex];

            // 뷰 추가 (배열 맨 뒤에 push_back 됨)
            BuildViews(Context, ShadowRequest, CurrentAtlasMap.Views);

            // 아틀라스 전용 UV 슬라이스 추가
            FShadowSlice Slice;
            Slice.Index = 0;
            Slice.Type = EShadowSliceType::Atlas;
            Slice.UVOffset = AllocResult.UVOffset;
            Slice.UVScale = AllocResult.UVScale;
            CurrentAtlasMap.Slices.push_back(Slice);

            // ★ 방금 추가된 View와 Slice의 실제 인덱스 추출 (맨 마지막 위치)
            uint32 CurrentViewIndex = CurrentAtlasMap.Views.size() - 1;
            uint32 CurrentSliceIndex = CurrentAtlasMap.Slices.size() - 1;

            // 매핑 테이블 기록
            FLightShadowMappingInfo& MappingInfo = ShadowLookupTable[ShadowRequest.LightId];
            MappingInfo.bHasShadow = true;
            MappingInfo.ShadowMapIndex = AtlasIndex;
            MappingInfo.SliceIndex = CurrentSliceIndex; // 계산된 슬라이스 인덱스 사용

            // 현재 LightId가 몇 번째 ShadowIndex를 쓰는지 기록
            GLightToShadowIndices[ShadowRequest.LightId] = ShadowIndexCounter;

            // ★ 2. GPU에 넘길 상수 버퍼 데이터를 여기서 싹 다 채워버림!
            // [수정됨] Views[0] 대신 방금 추가된 Views[CurrentViewIndex]를 참조합니다!
            GShadowCBData.ShadowDataArray[ShadowIndexCounter].ShadowLightView = CurrentAtlasMap.Views[CurrentViewIndex].LightView;
            GShadowCBData.ShadowDataArray[ShadowIndexCounter].ShadowLightProjection = CurrentAtlasMap.Views[CurrentViewIndex].LightProjection;
            GShadowCBData.ShadowDataArray[ShadowIndexCounter].UVOffset = AllocResult.UVOffset;
            GShadowCBData.ShadowDataArray[ShadowIndexCounter].UVScale = AllocResult.UVScale;
            GShadowCBData.ShadowDataArray[ShadowIndexCounter].SliceIndex = 0;

            ShadowIndexCounter++;
        }
        else
        {
            FShadowMap ShadowMap;
            if (MakeShadowMap(Context, ShadowRequest, ShadowMap))
            {
                GShadowMaps.push_back(ShadowMap);

                GLightToShadowIndices[ShadowRequest.LightId] = ShadowIndexCounter;

                GShadowCBData.ShadowDataArray[ShadowIndexCounter].ShadowLightView = ShadowMap.Views[0].LightView;
                GShadowCBData.ShadowDataArray[ShadowIndexCounter].ShadowLightProjection = ShadowMap.Views[0].LightProjection;
                GShadowCBData.ShadowDataArray[ShadowIndexCounter].UVOffset = FVector2(0, 0);
                GShadowCBData.ShadowDataArray[ShadowIndexCounter].UVScale = FVector2(1, 1);
                GShadowCBData.ShadowDataArray[ShadowIndexCounter].SliceIndex = 0;

                ShadowIndexCounter++;
            }
        }
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

    //D3D11_VIEWPORT ShadowViewport = {};
    //ShadowViewport.TopLeftX = (float)GShadowMaps[0];
    //ShadowViewport.TopLeftY = (float)GShadowMaps[0].Resource->Resolution;
    //ShadowViewport.Width = (float)GShadowMaps[0].Resource->Resolution;
    //ShadowViewport.Height = (float)GShadowMaps[0].Resource->Resolution;
    //ShadowViewport.MinDepth = 0.0f;
    //ShadowViewport.MaxDepth = 1.0f;
    //Context->DeviceContext->RSSetViewports(1, &ShadowViewport);
    for (uint32 i = 0; i < GShadowMaps.size(); i++)

        if (GShadowMaps[i].MapType == EShadowMapType::Depth2D) // 아틀라스 진행
        {
            Context->DeviceContext->ClearDepthStencilView(GShadowMaps[i].Resource->DSVs[0], D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
            Context->DeviceContext->OMSetRenderTargets(0, nullptr, GShadowMaps[i].Resource->DSVs[0]);

            for (uint32 j = 0; j < GShadowMaps[i].Slices.size(); j++)
            {

				ShaderBinding->SetMatrix4("View", GShadowMaps[i].Views[j].LightView);
                ShaderBinding->SetMatrix4("Projection", GShadowMaps[i].Views[j].LightProjection);

                D3D11_VIEWPORT ShadowViewport = {};
                ShadowViewport.TopLeftX = GShadowMaps[i].Slices[j].UVOffset.X * ATLAS_SIZE;
                ShadowViewport.TopLeftY = GShadowMaps[i].Slices[j].UVOffset.Y * ATLAS_SIZE;
                ShadowViewport.Width = (float)GShadowMaps[i].Slices[j].UVScale.X * ATLAS_SIZE;
                ShadowViewport.Height = (float)GShadowMaps[i].Slices[j].UVScale.Y * ATLAS_SIZE;
                ShadowViewport.MinDepth = 0.0f;
                ShadowViewport.MaxDepth = 1.0f;
                Context->DeviceContext->RSSetViewports(1, &ShadowViewport);

                for (const FRenderCommand& Cmd : Commands)
                {
                    if (Cmd.Type == ERenderCommandType::PostProcessOutline)
                        continue;

                    if (Cmd.MeshBuffer == nullptr || !Cmd.MeshBuffer->IsValid())
                        return false;

                    uint32 offset = 0;
                    ID3D11Buffer* vertexBuffer = Cmd.MeshBuffer->GetVertexBuffer().GetBuffer();
                    if (vertexBuffer == nullptr)
                        return false;

                    uint32 vertexCount = Cmd.MeshBuffer->GetVertexBuffer().GetVertexCount();
                    uint32 stride = Cmd.MeshBuffer->GetVertexBuffer().GetStride();

                    if (vertexCount == 0 || stride == 0)
                        return false;

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
        }

        else
        {
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
        for (uint32 i = 0; i < Req.CascadeCount; ++i)
        {
            // gather camera frustum corners in world space
            const FCameraState& Cam = Context->RenderBus->GetCameraState();
            const FVector CamPos = Context->RenderBus->GetCameraPosition();
            const FVector CamForward = Context->RenderBus->GetCameraForward();
            const FVector CamRight = Context->RenderBus->GetCameraRight();
            const FVector CamUp = Context->RenderBus->GetCameraUp();

            const float Near = Cam.NearZ;
            const float Far = Cam.FarZ;
            const float HalfTan = tanf(Cam.FOV * 0.5f);

            const float NearH = 2.0f * Near * HalfTan;
            const float NearW = NearH * Cam.AspectRatio;
            const float FarH = 2.0f * Far * HalfTan;
            const float FarW = FarH * Cam.AspectRatio;

            const FVector NearCenter = CamPos + CamForward * Near;
            const FVector FarCenter = CamPos + CamForward * Far;

            FVector FrustumCorners[8] = {
                NearCenter + CamUp * (NearH * 0.5f) - CamRight * (NearW * 0.5f),
                NearCenter + CamUp * (NearH * 0.5f) + CamRight * (NearW * 0.5f),
                NearCenter - CamUp * (NearH * 0.5f) - CamRight * (NearW * 0.5f),
                NearCenter - CamUp * (NearH * 0.5f) + CamRight * (NearW * 0.5f),
                FarCenter + CamUp * (FarH * 0.5f) - CamRight * (FarW * 0.5f),
                FarCenter + CamUp * (FarH * 0.5f) + CamRight * (FarW * 0.5f),
                FarCenter - CamUp * (FarH * 0.5f) - CamRight * (FarW * 0.5f),
                FarCenter - CamUp * (FarH * 0.5f) + CamRight * (FarW * 0.5f)
            };

            // compute frustum center and radius
            FVector FrustumCenter = FVector::ZeroVector;
            for (int j = 0; j < 8; ++j) FrustumCenter += FrustumCorners[j];
            FrustumCenter *= (1.0f / 8.0f);

            float AABBRadius = 0.0f;
            for (int j = 0; j < 8; ++j) AABBRadius = std::max(AABBRadius, (FrustumCorners[j] - FrustumCenter).Size());

            // light basis
            FRenderLight Light = Context->RenderBus->GetLights()[Req.LightId];
            const FVector LightDir = Light.Direction.GetSafeNormal();
            FVector Up = FVector::UpVector;
            if (std::abs(FVector::DotProduct(LightDir, Up)) > 0.99f) Up = FVector::RightVector;

            // place light-eye and build view
            const FVector Eye = FrustumCenter + LightDir * AABBRadius;
            FMatrix LightView = FMatrix::MakeViewLookAtLH(Eye, FrustumCenter, Up);

            // transform frustum into light space and compute AABB
            FVector FrustumLS[8];
            for (int j = 0; j < 8; ++j) FrustumLS[j] = LightView.TransformPosition(FrustumCorners[j]);

            FVector Min = FrustumLS[0];
            FVector Max = FrustumLS[0];
            for (int j = 1; j < 8; ++j)
            {
                Min.X = std::min(Min.X, FrustumLS[j].X);
                Min.Y = std::min(Min.Y, FrustumLS[j].Y);
                Min.Z = std::min(Min.Z, FrustumLS[j].Z);
                Max.X = std::max(Max.X, FrustumLS[j].X);
                Max.Y = std::max(Max.Y, FrustumLS[j].Y);
                Max.Z = std::max(Max.Z, FrustumLS[j].Z);
            }

            // Near/Far from light-space Z extents with padding and safety fallback
            float NearZ = Min.Z;
            float FarZ = Max.Z;
            const float Padding = std::max(50.0f, (FarZ - NearZ) * 0.1f);
            NearZ -= Padding; FarZ += Padding;
            if (FarZ - NearZ < 1.0f)
            {
                float center = (NearZ + FarZ) * 0.5f;
                NearZ = center - 0.5f; FarZ = center + 0.5f;
            }
            const float FallbackFar = AABBRadius + 1000.0f;
            if (FarZ < FallbackFar) FarZ = FallbackFar;

            // build projection
            FShadowViewInfo ViewInfo;
            ViewInfo.LightView = LightView;
            ViewInfo.LightProjection = FMatrix::MakeOrthographicLH(Max.X - Min.X, Max.Y - Min.Y, NearZ, FarZ);
            ViewInfo.SplitDepth = Far;
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

			float NearZ = 0.1f;
            float FarZ = std::max(Light.Radius, NearZ + 0.1f);

			ViewInfo.LightProjection = FMatrix::MakePerspectiveFovLH(
				FovRad,
				1.0f,        // 정사각형 섀도우 맵
				NearZ,        // Near
				FarZ // Far = 라이트 반경
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

			float FovRad = (90.0f * (3.141592f / 180.0f));          // 전체 FOV

            float NearZ = 0.1f;
            float FarZ = std::max(Light.Radius, NearZ + 0.1f);

			ViewInfo.LightProjection = FMatrix::MakePerspectiveFovLH(
				FovRad,
				1.0f,        // 정사각형 섀도우 맵
				NearZ,        // Near
				FarZ // Far = 라이트 반경
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
			ShadowSlice.Type = EShadowSliceType::Atlas;
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
