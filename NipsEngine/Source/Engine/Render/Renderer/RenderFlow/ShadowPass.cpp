#include "ShadowPass.h"
#include "Render/Scene/ShadowLightSelector.h"
#include "Core/ResourceManager.h"
#include "Editor/UI/EditorConsoleWidget.h"
#include "Render/Common/PSMCalculator.h"
#include <algorithm>

// Shadow map atlas 크기 (픽셀)
static constexpr uint32 kAtlasSize = 4096;
// PSM VirtualCamera를 뒤로 밀 거리
static constexpr float kPsmSliderBack = 3.0f;

// ---------------------------------------------------------------------------
// 파일 내부 전역 (Pass 간 Input/Output 연결 구조 정비 전 임시)
// ---------------------------------------------------------------------------
namespace
{
TArray<FShadowMap> GShadowMaps;
TArray<int32> GLightToShadowIndices; // LightId -> ShadowDataArray 인덱스
FOpaqueRenderPass::FShadowArrayCB GShadowCBData;

// ShadowBias 계산: UserBias(0~1) 를 실제 depth compare bias 로 변환
float ComputeShadowCompareBias(const FRenderLight& Light)
{
    const float UserBias = std::clamp(Light.ShadowBias, 0.0f, 1.0f);
    return 0.005f * std::max(UserBias * 2.0f, 0.1f);
}

// PSM Matrix 계산: VirtualCamera * PostPerspective 합성
FMatrix ComputePSMMatrix(const FRenderPassContext* Context, const FRenderLight& Light)
{
    FCamera Camera = {};
    Camera.Forward = Context->RenderBus->GetCameraForward();
    Camera.Up = Context->RenderBus->GetCameraUp();
    Camera.Right = Context->RenderBus->GetCameraRight();
    Camera.Position = Context->RenderBus->GetCameraPosition();
    Camera.CameraState = Context->RenderBus->GetCameraState();

    PSM::GetCameraFitNearZ(Context->RenderBus->GetCommands(ERenderPass::Opaque), Camera);

    FMatrix VCView, VCProj;
    PSM::GenerateVirtualCameraViewProjection(kPsmSliderBack, Camera, VCProj, VCView);

    // Directional Light는 광원 방향이 반전되어 PP 공간으로 이동
    const bool bDirectional = (Light.Type == static_cast<uint32>(ELightType::LightType_Directional));
    const FVector LightDir = bDirectional
                                 ? -Light.Direction.GetSafeNormal()
                                 : Light.Direction.GetSafeNormal();

    FMatrix PPView, PPProj;
    PSM::GeneratePostPerspectiveViewProjection(LightDir, PPProj, PPView, VCView, VCProj);

    // Row-major (DirectX): VCView * VCProj * PPView * PPProj
    return VCView * VCProj * PPView * PPProj;
}

} // namespace

// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// Begin: ShadowMap 리소스 할당 및 CB 데이터 구성
// ---------------------------------------------------------------------------
bool FShadowPass::Begin(const FRenderPassContext* Context)
{
    // ── Shader 바인딩 ──────────────────────────────────────────────────────
    UShader* Shader = FResourceManager::Get().GetShader("Shaders/ShadowMap.hlsl");
    if (!ShaderBinding || ShaderBinding->GetShader() != Shader)
        ShaderBinding = Shader->CreateBindingInstance(Context->Device);

    if (!ShaderBinding)
    {
        bSkip = true;
        return true;
    }

    // ── 이전 프레임 리소스 해제 ────────────────────────────────────────────
    for (FShadowMap& ShadowMap : GShadowMaps)
    {
        if (ShadowMap.bOwnsResource)
            Context->ShadowResourcePool->Release(ShadowMap.Resource);
    }
    GShadowMaps.clear();
    bSkip = false;

    // ── 라이트 선택 ────────────────────────────────────────────────────────
    std::vector<FShadowRequest> ShadowRequests =
        ShadowLightSelector.SelectShadowLights(
            Context->RenderBus->GetLights(),
            Context->RenderBus->GetCameraPosition(),
            Context->RenderBus->GetCameraState());

    if (ShadowRequests.empty())
    {
        bSkip = true;
        return true;
    }

    // ── LightType 별 버킷 정렬 (해상도 내림차순) ───────────────────────────
    std::array<std::vector<FShadowRequest>, static_cast<size_t>(ELightType::Max)> Buckets;
    for (auto& Req : ShadowRequests)
        Buckets[static_cast<int>(Req.Type)].push_back(Req);

    for (auto& Bucket : Buckets)
        std::sort(Bucket.begin(), Bucket.end(),
                  [](const FShadowRequest& A, const FShadowRequest& B)
                  { return A.Resolution > B.Resolution; });

    ShadowRequests.clear();
    for (auto& Bucket : Buckets)
        for (const auto& Req : Bucket)
            ShadowRequests.push_back(Req);

    // ── 인덱스 테이블 초기화 ──────────────────────────────────────────────
    const uint32 LightCount = static_cast<uint32>(Context->RenderBus->GetLights().size());
    GLightToShadowIndices.assign(LightCount, -1);
    std::memset(&GShadowCBData, 0, sizeof(GShadowCBData));

    uint32 ShadowIndexCounter = 0;
    uint32 PointShadowTextureIndex = 0;

    AtlasAllocator.Reset();

    // ── Point Light 공용 CubeArray 리소스 사전 할당 ───────────────────────
    FShadowResource* SharedPointResource = nullptr;
    uint32 SharedPointFaceOffset = 0;

    {
        uint32 PointCount = 0;
        uint32 PointResolution = 0;
        for (const auto& Req : ShadowRequests)
        {
            if (Req.Type != ELightType::LightType_Point)
                continue;
            ++PointCount;
            PointResolution = std::max((int)PointResolution, (int)Req.Resolution);
        }

        if (PointCount > 0)
        {
            FShadowRequestDesc Desc = {};
            Desc.AllocationMode = EShadowAllocationMode::ArrayBased;
            Desc.MapType = EShadowMapType::DepthCube;
            Desc.Resolution = PointResolution;
            Desc.CubeCount = std::min(PointCount, static_cast<uint32>(MAX_SHADOW_LIGHTS));

            if (!AcquireResource(Context, Desc, &SharedPointResource))
                SharedPointResource = nullptr;
        }
    }

    // ── 라이트 별 ShadowMap 생성 ──────────────────────────────────────────
    for (const FShadowRequest& Req : ShadowRequests)
    {
        if (ShadowIndexCounter >= MAX_SHADOW_LIGHTS)
            break;

        const FRenderLight& Light = Context->RenderBus->GetLights()[Req.LightId];

        // ── Point Light ───────────────────────────────────────────────────
        if (Req.Type == ELightType::LightType_Point)
        {
            if (SharedPointResource == nullptr)
                continue;

            FShadowMap ShadowMap;
            ShadowMap.Resource = SharedPointResource;
            ShadowMap.MapType = EShadowMapType::DepthCube;
            ShadowMap.LightId = Req.LightId;
            ShadowMap.LightType = Req.Type;
            ShadowMap.SourceLightSlotIndex = Light.SourceLightSlotIndex;
            ShadowMap.ResourceSliceOffset = SharedPointFaceOffset;
            ShadowMap.bOwnsResource = (PointShadowTextureIndex == 0);

            if (!BuildViews(Context, Req, ShadowMap.Views) ||
                !BuildSlices(Context, Req, ShadowMap.Slices))
            {
                if (ShadowMap.bOwnsResource)
                {
                    Context->ShadowResourcePool->Release(SharedPointResource);
                    SharedPointResource = nullptr;
                }
                continue;
            }

            GShadowMaps.push_back(ShadowMap);
            GLightToShadowIndices[Req.LightId] = ShadowIndexCounter;

            auto& CB = GShadowCBData.ShadowDataArray[ShadowIndexCounter];
            CB.UVOffset = FVector2(0.0f, 0.0f);
            CB.UVScale = FVector2(1.0f, 1.0f);
            CB.ShadowLightPosition = Light.Position;
            CB.ShadowFar = std::max(Light.Radius, 0.1f);
            CB.ShadowBias = ComputeShadowCompareBias(Light);
            CB.ShadowMapType = static_cast<uint32>(ShadowMap.MapType);
            CB.SliceCount = 1;
            CB.ShadowTextureIndex = PointShadowTextureIndex;
            CB.PointShadowTexelSize = 2.0f / std::max(static_cast<float>(SharedPointResource->Resolution), 1.0f);
            CB.isPSM = false;

            SharedPointFaceOffset += 6;
            ++PointShadowTextureIndex;
            ++ShadowIndexCounter;
            continue;
        }

        // ── Spot Light (Atlas) ────────────────────────────────────────────
        if (Req.Type == ELightType::LightType_Spot)
        {
            FAtlasAllocationResult AllocResult;
            if (!AtlasAllocator.Allocate(Req.Resolution, AllocResult))
            {
                // Atlas 공간 부족 → 새 Atlas 추가
                FShadowRequestDesc Desc = {};
                Desc.AllocationMode = EShadowAllocationMode::AtlasPacked;
                Desc.MapType = EShadowMapType::Depth2D;
                Desc.Resolution = kAtlasSize;
                Desc.CascadeCount = Req.Cascades.size();

                FShadowResource* NewAtlasRes = nullptr;
                if (!AcquireResource(Context, Desc, &NewAtlasRes))
                    continue;

                AtlasAllocator.AddNewAtlasResource(NewAtlasRes);

                FShadowMap NewAtlasMap;
                NewAtlasMap.Resource = NewAtlasRes;
                NewAtlasMap.MapType = EShadowMapType::Depth2D;
                NewAtlasMap.LightType = ELightType::LightType_Spot;
                NewAtlasMap.bOwnsResource = true;
                GShadowMaps.push_back(NewAtlasMap);

                AtlasAllocator.SetCurrentAtlasIndex(static_cast<uint32>(GShadowMaps.size() - 1));
                AtlasAllocator.Allocate(Req.Resolution, AllocResult);
            }

            const uint32 AtlasIndex = AtlasAllocator.GetCurrentAtlasIndex();
            FShadowMap& CurrentAtlas = GShadowMaps[AtlasIndex];

            BuildViews(Context, Req, CurrentAtlas.Views);

            FShadowSlice Slice;
            Slice.Index = static_cast<uint32>(CurrentAtlas.Slices.size());
            Slice.Type = EShadowSliceType::Atlas;
            Slice.UVOffset = AllocResult.UVOffset;
            Slice.UVScale = AllocResult.UVScale;
            Slice.LightId = Req.LightId;
            Slice.SourceLightSlotIndex = Light.SourceLightSlotIndex;
            CurrentAtlas.Slices.push_back(Slice);

            const uint32 ViewIndex = static_cast<uint32>(CurrentAtlas.Views.size()) - 1;

            GLightToShadowIndices[Req.LightId] = ShadowIndexCounter;

            auto& CB = GShadowCBData.ShadowDataArray[ShadowIndexCounter];
            CB.ShadowLightView[0] = CurrentAtlas.Views[ViewIndex].LightView;
            CB.ShadowLightProjection[0] = CurrentAtlas.Views[ViewIndex].LightProjection;
            CB.UVOffset = AllocResult.UVOffset;
            CB.UVScale = AllocResult.UVScale;
            CB.ShadowBias = ComputeShadowCompareBias(Light);
            CB.ShadowSlopeBias = Light.ShadowSlopeBias;
            CB.ShadowMapType = static_cast<uint32>(CurrentAtlas.MapType);
            CB.SliceCount = 1;
            CB.PointShadowTexelSize = 0.0f;
            CB.PSM = ComputePSMMatrix(Context, Light);
            CB.isPSM = true;
            CB.ShadowTextureIndex = 1u;
            ++ShadowIndexCounter;
            continue;
        }

        // ── Directional Light (CSM) ───────────────────────────────────────
        {
            FShadowMap ShadowMap;
            if (!MakeShadowMap(Context, Req, ShadowMap))
                continue;

            GShadowMaps.push_back(ShadowMap);
            GLightToShadowIndices[Req.LightId] = ShadowIndexCounter;

            auto& CB = GShadowCBData.ShadowDataArray[ShadowIndexCounter];
            for (size_t i = 0; i < Req.Cascades.size(); ++i)
            {
                CB.ShadowLightView[i] = ShadowMap.Views[i].LightView;
                CB.ShadowLightProjection[i] = ShadowMap.Views[i].LightProjection;
                CB.CascadeSplits[i] = Req.Cascades[i].Far;
            }
            CB.UVOffset = FVector2(0.0f, 0.0f);
            CB.UVScale = FVector2(1.0f, 1.0f);
            CB.ShadowLightPosition = Light.Position;
            CB.ShadowFar = std::max(Light.Radius, 0.1f);
            CB.ShadowBias = ComputeShadowCompareBias(Light);
            CB.ShadowSlopeBias = Light.ShadowSlopeBias;
            CB.ShadowMapType = static_cast<uint32>(ShadowMap.MapType);
            CB.SliceCount = static_cast<uint32>(Req.Cascades.size());
            CB.ShadowTextureIndex = 0u;
            CB.PointShadowTexelSize = 0.0f;
            CB.PSM = ComputePSMMatrix(Context, Light);
            CB.isPSM = true;

            ++ShadowIndexCounter;
        }
    }

    // Point 리소스를 아무도 못 쓴 경우 즉시 해제
    if (SharedPointResource != nullptr && PointShadowTextureIndex == 0)
        Context->ShadowResourcePool->Release(SharedPointResource);

    if (GShadowMaps.empty())
    {
        bSkip = true;
        return true;
    }

    OutSRV = GShadowMaps[0].Resource->SRV;
    OutRTV = nullptr;

    ShaderBinding->ApplyFrameParameters(*Context->RenderBus);
    return true;
}

// ---------------------------------------------------------------------------
// DrawCommand: ShadowMap 렌더링
// ---------------------------------------------------------------------------
bool FShadowPass::DrawCommand(const FRenderPassContext* Context)
{
    if (bSkip || !ShaderBinding)
        return true;

    const TArray<FRenderCommand>& Commands = Context->RenderBus->GetCommands(ERenderPass::Opaque);
    if (Commands.empty())
        return true;

    // Viewport 복원용 백업
    D3D11_VIEWPORT OldVP[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
    UINT OldVPCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    Context->DeviceContext->RSGetViewports(&OldVPCount, OldVP);
    Context->DeviceContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    // ── 슬라이스 1개 드로우 ────────────────────────────────────────────────
    auto DrawSlice = [&](const FShadowViewInfo& ViewInfo, const FRenderLight* Light) -> bool
    {
        if (Light == nullptr)
            return true; // 라이트 정보 없으면 스킵 (에러 아님)

        ShaderBinding->SetMatrix4("View", ViewInfo.LightView);
        ShaderBinding->SetMatrix4("Projection", ViewInfo.LightProjection);
        if (Light->Type != static_cast<uint32>(ELightType::LightType_Point))
        {
            ShaderBinding->SetMatrix4("PSM", ComputePSMMatrix(Context, *Light));
            ShaderBinding->SetBool("isPSM", 1);
        }
        else
        {
            // Point Light와 Spot Light는 일반 렌더링 방식을 따름
            ShaderBinding->SetMatrix4("PSM", FMatrix::Identity); // 쓰레기값 방지
            ShaderBinding->SetBool("isPSM", 0);
        }

        for (const FRenderCommand& Cmd : Commands)
        {
            if (Cmd.Type == ERenderCommandType::PostProcessOutline)
                continue;

            // 라이트 반경 밖의 메시 컬링 (Directional은 Position/Radius 무의미하므로 스킵)
            if (Cmd.WorldBounds.IsValid() &&
                Light->Type != static_cast<uint32>(ELightType::LightType_Directional))
            {
                const float Dist = FVector::Dist(Cmd.WorldBounds.GetCenter(), Light->Position) - Cmd.WorldBounds.GetExtent().Size();
                if (Dist > Light->Radius)
                    continue;
            }

            if (Cmd.MeshBuffer == nullptr || !Cmd.MeshBuffer->IsValid())
                return false;

            ID3D11Buffer* VB = Cmd.MeshBuffer->GetVertexBuffer().GetBuffer();
            const uint32 Stride = Cmd.MeshBuffer->GetVertexBuffer().GetStride();
            const uint32 VCount = Cmd.MeshBuffer->GetVertexBuffer().GetVertexCount();
            if (VB == nullptr || VCount == 0 || Stride == 0)
                return false;

            if (Cmd.Material)
            {
                ShaderBinding->ApplyPerObjectParameters(Cmd.PerObjectConstants);
                ShaderBinding->Bind(Context->DeviceContext);
                Context->DeviceContext->PSSetShader(nullptr, nullptr, 0);
            }

            CheckOverrideViewMode(Context);

            uint32 Offset = 0;
            Context->DeviceContext->IASetVertexBuffers(0, 1, &VB, &Stride, &Offset);

            ID3D11Buffer* IB = Cmd.MeshBuffer->GetIndexBuffer().GetBuffer();
            if (IB != nullptr)
            {
                Context->DeviceContext->IASetIndexBuffer(IB, DXGI_FORMAT_R32_UINT, 0);
                Context->DeviceContext->DrawIndexed(Cmd.SectionIndexCount, Cmd.SectionIndexStart, 0);
            }
            else
            {
                Context->DeviceContext->Draw(VCount, 0);
            }
        }
        return true;
    };

    // ── ShadowMap 순회 ────────────────────────────────────────────────────
    const auto& Lights = Context->RenderBus->GetLights();

    auto GetLight = [&](uint32 LightId) -> const FRenderLight*
    {
        return (LightId < static_cast<uint32>(Lights.size())) ? &Lights[LightId] : nullptr;
    };

    for (FShadowMap& ShadowMap : GShadowMaps)
    {
        if (ShadowMap.Resource == nullptr)
            continue;

        const bool bAtlas = (ShadowMap.MapType == EShadowMapType::Depth2D) &&
                            (!ShadowMap.Slices.empty()) &&
                            (ShadowMap.Slices[0].Type == EShadowSliceType::Atlas);

        if (bAtlas)
        {
            if (ShadowMap.Resource->DSVs.empty())
                continue;

            Context->DeviceContext->ClearDepthStencilView(
                ShadowMap.Resource->DSVs[0], D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
            Context->DeviceContext->OMSetRenderTargets(0, nullptr, ShadowMap.Resource->DSVs[0]);

            const uint32 DrawCount = std::min(
                static_cast<uint32>(ShadowMap.Views.size()),
                static_cast<uint32>(ShadowMap.Slices.size()));

            for (uint32 Si = 0; Si < DrawCount; ++Si)
            {
                const FShadowSlice& Slice = ShadowMap.Slices[Si];
                const float Res = static_cast<float>(ShadowMap.Resource->Resolution);

                D3D11_VIEWPORT VP = {};
                VP.TopLeftX = Slice.UVOffset.X * Res;
                VP.TopLeftY = Slice.UVOffset.Y * Res;
                VP.Width = std::max(1.0f, Slice.UVScale.X * Res);
                VP.Height = std::max(1.0f, Slice.UVScale.Y * Res);
                VP.MinDepth = 0.0f;
                VP.MaxDepth = 1.0f;
                Context->DeviceContext->RSSetViewports(1, &VP);

                // Atlas는 Slice별 LightId로 라이트를 찾아야 함
                if (!DrawSlice(ShadowMap.Views[Si], GetLight(Slice.LightId)))
                {
                    Context->DeviceContext->RSSetViewports(OldVPCount, OldVP);
                    return false;
                }
            }
            continue;
        }

        // ── Non-Atlas (CSM, CubeMap) ──────────────────────────────────────
        const uint32 AvailableDSVs = (ShadowMap.Resource->DSVs.size() > ShadowMap.ResourceSliceOffset)
                                         ? static_cast<uint32>(ShadowMap.Resource->DSVs.size() - ShadowMap.ResourceSliceOffset)
                                         : 0u;
        const uint32 DrawCount = std::min(AvailableDSVs, static_cast<uint32>(ShadowMap.Views.size()));
        if (DrawCount == 0)
            continue;

        D3D11_VIEWPORT VP = {};
        VP.Width = static_cast<float>(ShadowMap.Resource->Resolution);
        VP.Height = static_cast<float>(ShadowMap.Resource->Resolution);
        VP.MinDepth = 0.0f;
        VP.MaxDepth = 1.0f;
        Context->DeviceContext->RSSetViewports(1, &VP);

        const FRenderLight* Light = GetLight(ShadowMap.LightId);

        for (uint32 Vi = 0; Vi < DrawCount; ++Vi)
        {
            const uint32 DsvIndex = ShadowMap.ResourceSliceOffset + Vi;
            Context->DeviceContext->ClearDepthStencilView(
                ShadowMap.Resource->DSVs[DsvIndex], D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
            Context->DeviceContext->OMSetRenderTargets(0, nullptr, ShadowMap.Resource->DSVs[DsvIndex]);

            if (!DrawSlice(ShadowMap.Views[Vi], Light))
            {
                Context->DeviceContext->RSSetViewports(OldVPCount, OldVP);
                return false;
            }
        }
    }

    Context->DeviceContext->RSSetViewports(OldVPCount, OldVP);
    return true;
}

// ---------------------------------------------------------------------------
// End
// ---------------------------------------------------------------------------
bool FShadowPass::End(const FRenderPassContext* Context)
{
    (void)Context;
    return true;
}

// ---------------------------------------------------------------------------
// MakeShadowMap: CSM / CubeMap용 단독 리소스 생성
// ---------------------------------------------------------------------------
bool FShadowPass::MakeShadowMap(const FRenderPassContext* Context,
                                const FShadowRequest& Req,
                                FShadowMap& OutShadowMap)
{
    FShadowRequestDesc Desc = {};
    Desc.AllocationMode = EShadowAllocationMode::ArrayBased;
    Desc.MapType = (Req.Type != ELightType::LightType_Point)
                       ? EShadowMapType::Depth2D
                       : EShadowMapType::DepthCube;
    Desc.Resolution = Req.Resolution;
    Desc.CascadeCount = Req.Cascades.size();
    Desc.CubeCount = (Req.Type == ELightType::LightType_Point) ? 1u : 0u;

    if (!AcquireResource(Context, Desc, &OutShadowMap.Resource))
        return false;
    if (!BuildViews(Context, Req, OutShadowMap.Views))
        return false;
    if (!BuildSlices(Context, Req, OutShadowMap.Slices))
        return false;

    OutShadowMap.bOwnsResource = true;
    OutShadowMap.MapType = Desc.MapType;
    OutShadowMap.LightId = Req.LightId;
    OutShadowMap.SourceLightSlotIndex = Context->RenderBus->GetLights()[Req.LightId].SourceLightSlotIndex;
    OutShadowMap.LightType = Req.Type;
    return true;
}

// ---------------------------------------------------------------------------
// BuildViews: 라이트 타입별 View / Projection 행렬 생성
// ---------------------------------------------------------------------------
bool FShadowPass::BuildViews(const FRenderPassContext* Context,
                             const FShadowRequest& Req,
                             TArray<FShadowViewInfo>& OutViewInfoArray)
{
    const auto& Lights = Context->RenderBus->GetLights();

    switch (Req.Type)
    {
    // ── Directional (CSM) ─────────────────────────────────────────────────
    case ELightType::LightType_Directional:
    {
        const FCameraState& Cam = Context->RenderBus->GetCameraState();
        const FVector CamPos = Context->RenderBus->GetCameraPosition();
        const FVector CamFwd = Context->RenderBus->GetCameraForward();
        const FVector CamRight = Context->RenderBus->GetCameraRight();
        const FVector CamUp = Context->RenderBus->GetCameraUp();
        const FVector LightDir = Lights[Req.LightId].Direction.GetSafeNormal();

        FVector LightUp = FVector::UpVector;
        if (std::abs(FVector::DotProduct(LightDir, LightUp)) > 0.99f)
            LightUp = FVector::RightVector;

        const float HalfTan = tanf(Cam.FOV * 0.5f);

        for (uint32 i = 0; i < static_cast<uint32>(Req.Cascades.size()); ++i)
        {
            const float Near = Req.Cascades[i].Near;
            const float Far = Req.Cascades[i].Far;

            const float NearH = 2.0f * Near * HalfTan;
            const float NearW = NearH * Cam.AspectRatio;
            const float FarH = 2.0f * Far * HalfTan;
            const float FarW = FarH * Cam.AspectRatio;

            const FVector NC = CamPos + CamFwd * Near;
            const FVector FC = CamPos + CamFwd * Far;

            FVector Corners[8] = {
                NC + CamUp * (NearH * 0.5f) - CamRight * (NearW * 0.5f),
                NC + CamUp * (NearH * 0.5f) + CamRight * (NearW * 0.5f),
                NC - CamUp * (NearH * 0.5f) - CamRight * (NearW * 0.5f),
                NC - CamUp * (NearH * 0.5f) + CamRight * (NearW * 0.5f),
                FC + CamUp * (FarH * 0.5f) - CamRight * (FarW * 0.5f),
                FC + CamUp * (FarH * 0.5f) + CamRight * (FarW * 0.5f),
                FC - CamUp * (FarH * 0.5f) - CamRight * (FarW * 0.5f),
                FC - CamUp * (FarH * 0.5f) + CamRight * (FarW * 0.5f),
            };

            FVector Center = FVector::ZeroVector;
            for (const auto& C : Corners)
                Center += C;
            Center *= (1.0f / 8.0f);

            float Radius = 0.0f;
            for (const auto& C : Corners)
                Radius = std::max(Radius, (C - Center).Size());

            const FVector Eye = Center + LightDir * Radius;
            const FMatrix LightView = FMatrix::MakeViewLookAtLH(Eye, Center, LightUp);

            // 라이트 공간 AABB
            FVector LS[8];
            for (int j = 0; j < 8; ++j)
                LS[j] = LightView.TransformPosition(Corners[j]);

            FVector LSMin = LS[0], LSMax = LS[0];
            for (int j = 1; j < 8; ++j)
            {
                LSMin.X = std::min(LSMin.X, LS[j].X);
                LSMax.X = std::max(LSMax.X, LS[j].X);
                LSMin.Y = std::min(LSMin.Y, LS[j].Y);
                LSMax.Y = std::max(LSMax.Y, LS[j].Y);
                LSMin.Z = std::min(LSMin.Z, LS[j].Z);
                LSMax.Z = std::max(LSMax.Z, LS[j].Z);
            }

            float NearZ = LSMin.Z;
            float FarZ = LSMax.Z;
            const float Padding = std::max(50.0f, (FarZ - NearZ) * 0.1f);
            NearZ -= Padding;
            FarZ += Padding;
            FarZ = std::max(FarZ, Radius + 1000.0f);
            if (FarZ - NearZ < 1.0f)
            {
                NearZ -= 0.5f;
                FarZ += 0.5f;
            }

            FShadowViewInfo View;
            View.LightView = LightView;
            View.LightProjection = FMatrix::MakeOrthographicLH(LSMax.X - LSMin.X, LSMax.Y - LSMin.Y, NearZ, FarZ);
            View.SplitDepth = Far;
            OutViewInfoArray.push_back(View);
        }
        break;
    }

    // ── Spot Light ────────────────────────────────────────────────────────
    case ELightType::LightType_Spot:
    {
        const FRenderLight& Light = Lights[Req.LightId];
        const FVector LightDir = Light.Direction.GetSafeNormal();

        FVector Up = FVector(0.0f, 0.0f, 1.0f);
        if (std::abs(FVector::DotProduct(LightDir, Up)) > 0.99f)
            Up = FVector(1.0f, 0.0f, 0.0f);

        const float FovRad = std::acos(Light.SpotOuterCos) * 2.0f;
        const float NearZ = 0.1f;
        const float FarZ = std::max(Light.Radius, NearZ + 0.1f);

        for (uint32 i = 0; i < static_cast<uint32>(Req.Cascades.size()); ++i)
        {
            FShadowViewInfo View;
            View.LightView = FMatrix::MakeViewLookAtLH(Light.Position, Light.Position + LightDir, Up);
            View.LightProjection = FMatrix::MakePerspectiveFovLH(FovRad, 1.0f, NearZ, FarZ);
            View.SplitDepth = Context->RenderBus->GetCameraState().FarZ;
            OutViewInfoArray.push_back(View);
        }
        break;
    }

    // ── Point Light (CubeMap 6 faces) ─────────────────────────────────────
    case ELightType::LightType_Point:
    {
        static const FVector CubeDirs[6] = {
            FVector::ForwardVector,
            -FVector::ForwardVector,
            FVector::RightVector,
            -FVector::RightVector,
            FVector::UpVector,
            -FVector::UpVector,
        };
        static const FVector CubeUps[6] = {
            FVector::RightVector,
            FVector::RightVector,
            -FVector::UpVector,
            FVector::UpVector,
            FVector::RightVector,
            FVector::RightVector,
        };

        const FRenderLight& Light = Lights[Req.LightId];
        const float NearZ = 0.1f;
        const float FarZ = std::max(Light.Radius, NearZ + 0.1f);
        const float FovRad = 90.0f * (3.141592f / 180.0f);

        for (uint32 i = 0; i < 6; ++i)
        {
            FShadowViewInfo View;
            View.LightView = FMatrix::MakeViewLookAtLH(Light.Position, Light.Position + CubeDirs[i], CubeUps[i]);
            View.LightProjection = FMatrix::MakePerspectiveFovLH(FovRad, 1.0f, NearZ, FarZ);
            View.SplitDepth = Context->RenderBus->GetCameraState().FarZ;
            OutViewInfoArray.push_back(View);
        }
        break;
    }

    default:
        return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
// BuildSlices: Slice 메타 데이터 생성
// ---------------------------------------------------------------------------
bool FShadowPass::BuildSlices(const FRenderPassContext* Context,
                              const FShadowRequest& Req,
                              TArray<FShadowSlice>& OutShadowSlices)
{
    auto MakeSlice = [](uint32 Idx, EShadowSliceType Type, uint32 LightId) -> FShadowSlice
    {
        FShadowSlice S;
        S.Index = Idx;
        S.Type = Type;
        S.UVOffset = FVector2(0.0f, 0.0f);
        S.UVScale = FVector2(1.0f, 1.0f);
        S.LightId = LightId;
        return S;
    };

    switch (Req.Type)
    {
    case ELightType::LightType_Directional:
        for (uint32 i = 0; i < static_cast<uint32>(Req.Cascades.size()); ++i)
            OutShadowSlices.push_back(MakeSlice(i, EShadowSliceType::CSM, Req.LightId));
        break;

    case ELightType::LightType_Spot:
        for (uint32 i = 0; i < static_cast<uint32>(Req.Cascades.size()); ++i)
            OutShadowSlices.push_back(MakeSlice(i, EShadowSliceType::Atlas, Req.LightId));
        break;

    case ELightType::LightType_Point:
        for (uint32 i = 0; i < 6; ++i)
            OutShadowSlices.push_back(MakeSlice(i, EShadowSliceType::CubeFace, Req.LightId));
        break;

    default:
        return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
// AcquireResource
// ---------------------------------------------------------------------------
bool FShadowPass::AcquireResource(const FRenderPassContext* Context,
                                  const FShadowRequestDesc& Desc,
                                  FShadowResource** OutShadowResource)
{
    *OutShadowResource = Context->ShadowResourcePool->Acquire(Context->Device, Desc);
    return (*OutShadowResource != nullptr);
}