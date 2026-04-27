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
    std::vector<FLightShadowMappingInfo> ShadowLookupTable(Context->RenderBus->GetLights().size());

    AtlasAllocator.Reset();

    for (const FShadowRequest& ShadowRequest : ShadowRequests)
    {
        if (ShadowRequest.Type == ELightType::LightType_Spot)
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

            BuildViews(Context, ShadowRequest, CurrentAtlasMap.Views);

            // 아틀라스 전용 UV 적용
            FShadowSlice Slice;
            Slice.Index = 0;
            Slice.Type = EShadowSliceType::Atlas;
            Slice.UVOffset = AllocResult.UVOffset; // Allocator가 준 값
            Slice.UVScale = AllocResult.UVScale;   // Allocator가 준 값
            CurrentAtlasMap.Slices.push_back(Slice);

            // 매핑 테이블 기록
            // 라이팅 패스에서 "이 Spot Light 그림자 어딨어?" 하면 바로 대답할 수 있게 기록합니다.
            FLightShadowMappingInfo& MappingInfo = ShadowLookupTable[ShadowRequest.LightId];
            MappingInfo.bHasShadow = true;
            MappingInfo.ShadowMapIndex = AtlasIndex;
            MappingInfo.SliceIndex = CurrentAtlasMap.Slices.size() - 1;
        }
        else
        {
            FShadowMap ShadowMap;
            if (MakeShadowMap(Context, ShadowRequest, ShadowMap))
                GShadowMaps.push_back(ShadowMap);
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

    D3D11_VIEWPORT ShadowViewport = {};
    ShadowViewport.TopLeftX = 0.0f;
    ShadowViewport.TopLeftY = 0.0f;
    ShadowViewport.Width = (float)GShadowMaps[0].Resource->Resolution;
    ShadowViewport.Height = (float)GShadowMaps[0].Resource->Resolution;
    ShadowViewport.MinDepth = 0.0f;
    ShadowViewport.MaxDepth = 1.0f;
    Context->DeviceContext->RSSetViewports(1, &ShadowViewport);
    for (uint32 i = 0; i < GShadowMaps.size(); i++)

        if (GShadowMaps[i].MapType == EShadowMapType::Depth2D) // 아틀라스 진행
        {
            for (uint32 j = 0; j < GShadowMaps[i].Slices.size(); j++)
            {
                D3D11_VIEWPORT ShadowViewport = {};
                ShadowViewport.TopLeftX = GShadowMaps[i].Slices[j].UVOffset.X;
                ShadowViewport.TopLeftY = GShadowMaps[i].Slices[j].UVOffset.Y;
                ShadowViewport.Width = (float)GShadowMaps[i].Slices[j].UVScale.X * ATLAS_SIZE;
                ShadowViewport.Height = (float)GShadowMaps[i].Slices[j].UVScale.Y * ATLAS_SIZE;
                ShadowViewport.MinDepth = 0.0f;
                ShadowViewport.MaxDepth = 1.0f;
                Context->DeviceContext->RSSetViewports(1, &ShadowViewport);

                Context->DeviceContext->ClearDepthStencilView(GShadowMaps[i].Resource->DSVs[0], D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
                Context->DeviceContext->OMSetRenderTargets(0, nullptr, GShadowMaps[i].Resource->DSVs[0]);
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
        for (uint32 i = 0; i < Req.CascadeCount; i++)
        {
            FRenderLight Light = Context->RenderBus->GetLights()[Req.LightId];
            FShadowViewInfo ViewInfo;

            const FCameraState& Cam = Context->RenderBus->GetCameraState();

            float Near = Cam.NearZ;
            float Far = Cam.FarZ;
            float FovY = Cam.FOV;
            float Aspect = Cam.AspectRatio;

            FVector CamPos = Context->RenderBus->GetCameraPosition();
            FVector CamForward = Context->RenderBus->GetCameraForward();
            FVector CamRight = Context->RenderBus->GetCameraRight();
            FVector CamUp = Context->RenderBus->GetCameraUp();

            // =========================
            // 1. Frustum 크기 계산
            // =========================
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

            // =========================
            // 2. Frustum Center
            // =========================
            FVector Center(0, 0, 0);
            for (int j = 0; j < 8; j++)
            {
                Center += FrustumCorners[j];
            }
            Center /= 8.0f;

            // =========================
            // 3. Light View 생성
            // =========================
            FVector LightDir = Light.Direction; // normalize 되어 있어야 함

            FVector Eye = Center - LightDir * 500.0f;
            FVector Target = Center;

            FVector Up = FVector(0, 0, 1);
            if (abs(FVector::DotProduct(LightDir, Up)) > 0.99f)
            {
                Up = FVector(1, 0, 0);
            }

            FMatrix LightView = FMatrix::MakeViewLookAtLH(Eye, Target, Up);

            // =========================
            // 4. Frustum → Light Space AABB
            // =========================
            FVector Min(FLT_MAX, FLT_MAX, FLT_MAX);
            FVector Max(-FLT_MAX, -FLT_MAX, -FLT_MAX);

            for (int j = 0; j < 8; j++)
            {
                FVector P = LightView.TransformPosition(FrustumCorners[j]);

                Min.X = std::min<float>(Min.X, P.X);
                Min.Y = std::min<float>(Min.Y, P.Y);
                Min.Z = std::min<float>(Min.Z, P.Z);

                Max.X = std::max<float>(Max.X, P.X);
                Max.Y = std::max<float>(Max.Y, P.Y);
                Max.Z = std::max<float>(Max.Z, P.Z);
            }

            // =========================
            // 5. Centered Ortho 맞추기 (핵심)
            // =========================
            FVector CenterLS = (Min + Max) * 0.5f;

            // LightView를 Center 기준으로 이동
            FMatrix CenterOffset = FMatrix::MakeTranslation(-CenterLS);
            ViewInfo.LightView = CenterOffset * LightView;

            float ViewWidth = (Max.X - Min.X);
            float ViewHeight = (Max.Y - Min.Y);

            float NearZ = 0.0f;
            float FarZ = (Max.Z - Min.Z) + 200.0f; // 여유

            // =========================
            // 6. Projection
            // =========================
            ViewInfo.LightProjection = FMatrix::MakeOrthographicLH(
                ViewWidth,
                ViewHeight,
                NearZ,
                FarZ);

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

            float FovRad = (90 * (3.141592 / 180)); // 전체 FOV

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
            ShadowSlice.Type = EShadowSliceType::Atlas;
            ShadowSlice.UVOffset = FVector2(0, 0);
            ShadowSlice.UVScale = FVector2(1, 1); // 텍스쳐 전부를 쓰겠다.
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
