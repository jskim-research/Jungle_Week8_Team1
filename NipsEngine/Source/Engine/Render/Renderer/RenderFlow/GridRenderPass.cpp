#include "GridRenderPass.h"

#include "Core/ResourceManager.h"
#include "Render/Scene/RenderBus.h"
#include "Render/Scene/RenderCommand.h"
#include "Render/Resource/RenderResources.h"
#include "Render/Resource/Shader.h"

#include <algorithm>
#include <cmath>

namespace
{
    constexpr float GridPlaneN = 0.0f;

    FGridRenderSettings SanitizeGridRenderSettings(const FGridRenderSettings& InSettings)
    {
        FGridRenderSettings Settings = InSettings;
        Settings.LineThickness = std::clamp(Settings.LineThickness, 0.0f, 8.0f);
        Settings.MajorLineThickness = std::clamp(Settings.MajorLineThickness, 0.0f, 12.0f);
        Settings.MajorLineInterval = std::clamp<int32>(Settings.MajorLineInterval, 1, 100);
        Settings.MinorIntensity = std::clamp(Settings.MinorIntensity, 0.0f, 2.0f);
        Settings.MajorIntensity = std::clamp(Settings.MajorIntensity, 0.0f, 2.0f);
        Settings.AxisThickness = std::clamp(Settings.AxisThickness, 0.0f, 12.0f);
        Settings.AxisIntensity = std::clamp(Settings.AxisIntensity, 0.0f, 2.0f);
        Settings.AxisLengthScale = std::clamp(Settings.AxisLengthScale, 0.1f, 10.0f);
        return Settings;
    }

    enum class EGridPlane
    {
        XY,
        XZ,
        YZ
    };

    struct FGridPlaneDesc
    {
        int32 A = 0;
        int32 B = 1;
        int32 N = 2;
        FVector AxisA = FVector(1.0f, 0.0f, 0.0f);
        FVector AxisB = FVector(0.0f, 1.0f, 0.0f);
        FVector4 ColorA = FVector4(1.0f, 0.2f, 0.2f, 1.0f);
        FVector4 ColorB = FVector4(0.2f, 1.0f, 0.2f, 1.0f);
    };

    inline float GetComp(const FVector& V, int32 Index)
    {
        return (&V.X)[Index];
    }

    inline float& GetComp(FVector& V, int32 Index)
    {
        return (&V.X)[Index];
    }

    EGridPlane DetermineGridPlane(bool bOrthographic, const FVector& CameraForward)
    {
        if (!bOrthographic)
        {
            return EGridPlane::XY;
        }

        const float AxX = std::fabs(CameraForward.X);
        const float AxY = std::fabs(CameraForward.Y);
        const float AxZ = std::fabs(CameraForward.Z);

        if (AxX >= AxY && AxX >= AxZ) return EGridPlane::YZ;
        if (AxY >= AxX && AxY >= AxZ) return EGridPlane::XZ;
        return EGridPlane::XY;
    }

    FGridPlaneDesc GetGridPlaneDesc(EGridPlane Plane)
    {
        switch (Plane)
        {
        case EGridPlane::XZ:
            return { 0, 2, 1, FVector(1.0f, 0.0f, 0.0f), FVector(0.0f, 0.0f, 1.0f),
                FVector4(1.0f, 0.2f, 0.2f, 1.0f), FVector4(0.2f, 0.2f, 1.0f, 1.0f) };
        case EGridPlane::YZ:
            return { 1, 2, 0, FVector(0.0f, 1.0f, 0.0f), FVector(0.0f, 0.0f, 1.0f),
                FVector4(0.2f, 1.0f, 0.2f, 1.0f), FVector4(0.2f, 0.2f, 1.0f, 1.0f) };
        case EGridPlane::XY:
        default:
            return { 0, 1, 2, FVector(1.0f, 0.0f, 0.0f), FVector(0.0f, 1.0f, 0.0f),
                FVector4(1.0f, 0.2f, 0.2f, 1.0f), FVector4(0.2f, 1.0f, 0.2f, 1.0f) };
        }
    }

    FVector MakeGridPoint(const FGridPlaneDesc& Desc, float A, float B, float N)
    {
        FVector P = FVector::ZeroVector;
        GetComp(P, Desc.A) = A;
        GetComp(P, Desc.B) = B;
        GetComp(P, Desc.N) = N;
        return P;
    }

    FVector ComputeGridFocusPointOnPlane(const FVector& CameraPosition, const FVector& CameraForward, const FGridPlaneDesc& Desc)
    {
        const float PosN = GetComp(CameraPosition, Desc.N);
        const float FwdN = GetComp(CameraForward, Desc.N);

        if (std::fabs(FwdN) > 1.0e-4f)
        {
            const float T = -PosN / FwdN;
            const float MaxT = std::fabs(PosN) * 10.0f;
            if (T > 0.0f && T < MaxT)
            {
                return CameraPosition + CameraForward * T;
            }
        }

        FVector Fallback = CameraPosition;
        GetComp(Fallback, Desc.N) = GridPlaneN;
        return Fallback;
    }

    int32 ComputeDynamicHalfCountOnPlane(float Spacing, int32 BaseHalfCount, const FVector& CameraPosition, const FGridPlaneDesc& Desc)
    {
        const float BaseExtent = Spacing * static_cast<float>((std::max)(BaseHalfCount, 1));
        const float HeightDriven = (std::fabs(GetComp(CameraPosition, Desc.N)) * 2.0f) + (Spacing * 4.0f);
        const float RequiredExtent = (std::max)(BaseExtent, HeightDriven);
        return (std::max)(BaseHalfCount, static_cast<int32>(std::ceil(RequiredExtent / Spacing)));
    }

    float SnapToGrid(float Value, float Spacing)
    {
        return std::round(Value / Spacing) * Spacing;
    }

    struct FGridDrawParams
    {
        EGridPlane Plane = EGridPlane::XY;
        FGridPlaneDesc PlaneDesc;
        FVector Center = FVector::ZeroVector;
        float Spacing = 1.0f;
        float Range = 100.0f;
        float MaxDistance = 125.0f;
        float AxisLength = 100.0f;
    };

    FGridDrawParams BuildGridDrawParams(const FRenderCommand& Cmd, const FRenderBus& RenderBus)
    {
        FGridDrawParams Params = {};
        Params.Spacing = (std::max)(Cmd.Constants.Grid.GridSpacing, 0.0001f);

        const FVector CameraPosition = RenderBus.GetCameraPosition();
        const FVector CameraForward = RenderBus.GetCameraForward();
        Params.Plane = DetermineGridPlane(Cmd.Constants.Grid.bOrthographic, CameraForward);
        Params.PlaneDesc = GetGridPlaneDesc(Params.Plane);

        const int32 BaseHalfCount = (std::max)(Cmd.Constants.Grid.GridHalfLineCount, 1);
        const FVector FocusPoint = ComputeGridFocusPointOnPlane(CameraPosition, CameraForward, Params.PlaneDesc);
        const float CenterA = SnapToGrid(GetComp(FocusPoint, Params.PlaneDesc.A), Params.Spacing);
        const float CenterB = SnapToGrid(GetComp(FocusPoint, Params.PlaneDesc.B), Params.Spacing);
        const int32 DynamicHalfCount = ComputeDynamicHalfCountOnPlane(
            Params.Spacing,
            BaseHalfCount,
            CameraPosition,
            Params.PlaneDesc);

        Params.Center = MakeGridPoint(Params.PlaneDesc, CenterA, CenterB, GridPlaneN);
        Params.Range = Params.Spacing * static_cast<float>(DynamicHalfCount);
        Params.MaxDistance = (std::max)(Params.Range * 1.25f, Params.Spacing * 16.0f);
        Params.AxisLength = (std::max)(Params.Range, Params.Spacing * 10.0f);
        return Params;
    }

    void BindGridParameters(
        const std::shared_ptr<FShaderBindingInstance>& Binding,
        const FGridDrawParams& Params,
        const FGridRenderSettings& Settings,
        bool bDrawGrid,
        bool bDrawAxis)
    {
        Binding->SetFloat("GridSize", Params.Spacing);
        Binding->SetFloat("Range", Params.Range);
        Binding->SetFloat("LineThickness", Settings.LineThickness);
        Binding->SetFloat("MajorLineInterval", static_cast<float>(Settings.MajorLineInterval));
        Binding->SetFloat("MajorLineThickness", Settings.MajorLineThickness);
        Binding->SetFloat("MinorIntensity", bDrawGrid ? Settings.MinorIntensity : 0.0f);
        Binding->SetFloat("MajorIntensity", bDrawGrid ? Settings.MajorIntensity : 0.0f);
        Binding->SetFloat("MaxDistance", Params.MaxDistance);
        Binding->SetFloat("AxisThickness", bDrawAxis ? Settings.AxisThickness : 0.0f);
        Binding->SetFloat("AxisLength", Params.AxisLength * Settings.AxisLengthScale);
        Binding->SetFloat("AxisIntensity", bDrawAxis ? Settings.AxisIntensity : 0.0f);
        Binding->SetVector4("GridCenter", FVector4(Params.Center.X, Params.Center.Y, Params.Center.Z, 0.0f));
        Binding->SetVector4("GridAxisA", FVector4(Params.PlaneDesc.AxisA.X, Params.PlaneDesc.AxisA.Y, Params.PlaneDesc.AxisA.Z, 0.0f));
        Binding->SetVector4("GridAxisB", FVector4(Params.PlaneDesc.AxisB.X, Params.PlaneDesc.AxisB.Y, Params.PlaneDesc.AxisB.Z, 0.0f));
        Binding->SetVector4("AxisColorA", Params.PlaneDesc.ColorA);
        Binding->SetVector4("AxisColorB", Params.PlaneDesc.ColorB);
    }

    void BindAxisParameters(
        const std::shared_ptr<FShaderBindingInstance>& Binding,
        const FGridDrawParams& Params,
        const FGridRenderSettings& Settings)
    {
        Binding->SetFloat("GridSize", Params.Spacing);
        Binding->SetFloat("AxisThickness", Settings.AxisThickness);
        Binding->SetFloat("AxisLength", Params.AxisLength * Settings.AxisLengthScale);
        Binding->SetFloat("MaxDistance", Params.MaxDistance);
        Binding->SetFloat("AxisIntensity", Settings.AxisIntensity);
    }
}

bool FGridRenderPass::Initialize()
{
    return true;
}

bool FGridRenderPass::Release()
{
    GridShaderBinding.reset();
    AxisShaderBinding.reset();
    return true;
}

bool FGridRenderPass::Begin(const FRenderPassContext* Context)
{
    ID3D11RenderTargetView* RTV = PrevPassRTV;
    ID3D11DepthStencilView* DSV = Context->RenderTargets->DepthStencilView;
    Context->DeviceContext->OMSetRenderTargets(1, &RTV, DSV);

    ID3D11DepthStencilState* DepthStencilState =
        FResourceManager::Get().GetOrCreateDepthStencilState(EDepthStencilType::DepthReadOnly);
    ID3D11BlendState* BlendState =
        FResourceManager::Get().GetOrCreateBlendState(EBlendType::AlphaBlend);
    ID3D11RasterizerState* RasterizerState =
        FResourceManager::Get().GetOrCreateRasterizerState(ERasterizerType::SolidNoCull);

    Context->DeviceContext->OMSetDepthStencilState(DepthStencilState, 0);
    Context->DeviceContext->OMSetBlendState(BlendState, nullptr, 0xFFFFFFFF);
    Context->DeviceContext->RSSetState(RasterizerState);

    Context->DeviceContext->IASetInputLayout(nullptr);
    Context->DeviceContext->IASetVertexBuffers(0, 0, nullptr, nullptr, nullptr);
    Context->DeviceContext->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);
    Context->DeviceContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    OutSRV = PrevPassSRV;
    OutRTV = PrevPassRTV;
    return true;
}

bool FGridRenderPass::DrawCommand(const FRenderPassContext* Context)
{
    if (!Context || !Context->RenderBus)
    {
        return true;
    }

    const FShowFlags ShowFlags = Context->RenderBus->GetShowFlags();
    if (!ShowFlags.bGrid && !ShowFlags.bAxis)
    {
        return true;
    }

    const TArray<FRenderCommand>& Commands = Context->RenderBus->GetCommands(ERenderPass::Grid);
    if (Commands.empty())
    {
        return true;
    }

    UShader* GridShader = FResourceManager::Get().GetShader("Shaders/ShaderGrid.hlsl");
    UShader* AxisShader = FResourceManager::Get().GetShader("Shaders/ShaderAxis.hlsl");
    if (!GridShader)
    {
        return true;
    }

    if (!GridShaderBinding || GridShaderBinding->GetShader() != GridShader)
    {
        GridShaderBinding = GridShader->CreateBindingInstance(Context->Device);
    }

    if (AxisShader && (!AxisShaderBinding || AxisShaderBinding->GetShader() != AxisShader))
    {
        AxisShaderBinding = AxisShader->CreateBindingInstance(Context->Device);
    }

    if (!GridShaderBinding)
    {
        return true;
    }

    for (const FRenderCommand& Cmd : Commands)
    {
        if (Cmd.Type != ERenderCommandType::Grid || Cmd.Constants.Grid.GridSpacing <= 0.0f)
        {
            continue;
        }

        const FGridDrawParams Params = BuildGridDrawParams(Cmd, *Context->RenderBus);
        const FGridRenderSettings GridSettings = SanitizeGridRenderSettings(Cmd.Constants.Grid.RenderSettings);

        GridShaderBinding->ApplyFrameParameters(*Context->RenderBus);
        BindGridParameters(GridShaderBinding, Params, GridSettings, ShowFlags.bGrid, ShowFlags.bAxis);
        GridShaderBinding->Bind(Context->DeviceContext);
        Context->DeviceContext->Draw(6, 0);

        if (ShowFlags.bAxis && Params.Plane == EGridPlane::XY && AxisShaderBinding)
        {
            AxisShaderBinding->ApplyFrameParameters(*Context->RenderBus);
            BindAxisParameters(AxisShaderBinding, Params, GridSettings);
            AxisShaderBinding->Bind(Context->DeviceContext);
            Context->DeviceContext->Draw(12, 0);
        }
    }

    return true;
}

bool FGridRenderPass::End(const FRenderPassContext* Context)
{
    return true;
}
