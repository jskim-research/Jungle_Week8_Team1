#include "LightCullingPass.h"
#include "Core/Paths.h"
#include "Render/Scene/RenderBus.h"
#include "Render/Scene/RenderCommand.h"
#include "UI/EditorConsoleWidget.h"

#include <algorithm>

#include "Render/Renderer/RenderFlow/ShadowPass.h"

#include <cmath>
#include <cstring>

namespace
{
    constexpr uint32 LightCullingTileSize = 16;
    
    struct FFrameConstantBuffer
    {
        FMatrix View;
        FMatrix Projection;
        FMatrix InverseViewProjection;
        FMatrix InverseProjection;
        FVector CameraPosition;
        float Padding1;
        float bIsWireframe;
        FVector WireframeRGB;
    };

    struct FForwardPlusConstants
    {
        uint32 ScreenSize[2];
        uint32 TileCount[2];
        uint32 bEnable25DMask;
        float Padding[3];
    };

	struct FLightCullingLight

    {
        FVector WorldPos = FVector::ZeroVector;
        float Radius = 0.0f;
        FVector Color = FVector::ZeroVector;
        float Intensity = 0.0f;
        float RadiusFalloff = 1.0f;
        uint32 Type = 0; // 0=Point, 1=Spot
        float SpotInnerCos = 1.0f;
        float SpotOuterCos = 0.0f;
        FVector Direction = FVector::ZeroVector;
        int ShadowIndex;
    };

    struct FLightingConstants
    {
        FVector UnusedAmbientColor;
        float UnusedAmbientIntensity;
        uint32 DirectionalLightCount;
        uint32 PointLightCount;
        uint32 SpotLightCount;
        float LightingPad;
    };

    struct FPointLightInfo
    {
        FVector Position;
        float Radius;
        FVector Color;
        float Intensity;
        int ShadowIndex; // [복구] C++ 쪽 FPointLightData에도 int ShadowIndex; float3 Padding; 추가 필수!
        FVector Padding;  // 16바이트 정렬을 위한 패딩
    };

    struct FSpotLightInfo
    {
        FVector Position;
        float Radius;
        FVector Color;
        float Intensity;
        FVector Direction;
        float InnerConeCos;
        float OuterConeCos;
        int ShadowIndex;
        FVector2 Padding;
    };

    struct FScoredLightIndex
    {
        float Score = 0.0f;
        uint32 Index = 0;
    };

    struct FSpotBroadPhaseBounds
    {
        FVector Center = FVector::ZeroVector;
        float Radius = 0.0f;
    };

    uint32 CeilDivide(uint32 Numerator, uint32 Denominator)
    {
        return (Numerator + Denominator - 1u) / Denominator;
    }

    bool SortByHigherScore(const FScoredLightIndex& Lhs, const FScoredLightIndex& Rhs)
    {
        return Lhs.Score > Rhs.Score;
    }

    float ComputePointLightScore(const FPointLightInfo& Light, const FVector& CameraPos)
    {
        const float Radius = std::max(Light.Radius, 0.001f);
        const float DistanceToCenter = FVector::Dist(CameraPos, Light.Position);
        const float VolumeDist = std::max(0.0f, DistanceToCenter - Radius);
        const float Influence = std::max(Light.Intensity, 0.0f) * Radius * Radius;
        return Influence / (1.0f + VolumeDist * VolumeDist);
    }

    FSpotBroadPhaseBounds BuildSpotBroadPhaseBounds(const FSpotLightInfo& Light)
    {
        const float Height = std::max(Light.Radius, 0.001f);
        const FVector Axis = Light.Direction.GetSafeNormal();
        const float CosTheta = std::clamp(Light.OuterConeCos, 0.001f, 0.9999f);
        const float SinTheta = std::sqrt(std::max(0.0f, 1.0f - CosTheta * CosTheta));
        const float BaseRadius = Height * (SinTheta / CosTheta);
        const FVector BaseCenter = Light.Position + Axis * Height;

        FSpotBroadPhaseBounds Bounds;
        if (BaseRadius <= Height)
        {
            Bounds.Radius = (Height * Height + BaseRadius * BaseRadius) / (2.0f * Height);
            Bounds.Center = Light.Position + Axis * Bounds.Radius;
        }
        else
        {
            Bounds.Radius = BaseRadius;
            Bounds.Center = BaseCenter;
        }

        return Bounds;
    }

    float ComputeSpotLightScore(const FSpotLightInfo& Light, const FVector& CameraPos)
    {
        const FSpotBroadPhaseBounds Bounds = BuildSpotBroadPhaseBounds(Light);
        const float DistanceToCenter = FVector::Dist(CameraPos, Bounds.Center);
        const float VolumeDist = std::max(0.0f, DistanceToCenter - Bounds.Radius);

        const FVector LightDirection = Light.Direction.GetSafeNormal();
        const FVector ToCamera = (CameraPos - Light.Position).GetSafeNormal();
        const float Facing = std::max(0.0f, FVector::DotProduct(LightDirection, ToCamera));
        const float FacingWeight = 0.25f + 0.75f * Facing * Facing;

        const float InfluenceRadius = std::max(Bounds.Radius, 0.001f);
        const float Influence = std::max(Light.Intensity, 0.0f) * InfluenceRadius * InfluenceRadius;
        return (Influence * FacingWeight) / (1.0f + VolumeDist * VolumeDist);
    }

    FLightCullingOutputs GLightCullingOutputs = {};
    FLightCullingDebugStats GDebugStats = {};
}

bool FLightCullingPass::Initialize()
{
    return true;
}

bool FLightCullingPass::Release()
{
    ComputeShader.Reset();
    FrameConstantBuffer.Reset();
    PointLightBuffer.Reset();
    PointLightBufferSRV.Reset();
    SpotLightBuffer.Reset();
    SpotLightBufferSRV.Reset();
    TilePointLightGridBuffer.Reset();
    TilePointLightGridUAV.Reset();
    TilePointLightGridSRV.Reset();
    TilePointLightIndexBuffer.Reset();
    TilePointLightIndexUAV.Reset();
    TilePointLightIndexSRV.Reset();
    TileSpotLightGridBuffer.Reset();
    TileSpotLightGridUAV.Reset();
    TileSpotLightGridSRV.Reset();
    TileSpotLightIndexBuffer.Reset();
    TileSpotLightIndexUAV.Reset();
    TileSpotLightIndexSRV.Reset();
    ForwardPlusConstantBuffer.Reset();
    LightingConstantBuffer.Reset();
    
    PointLightBufferCapacity = 0;
    SpotLightBufferCapacity = 0;
    TileBufferCapacityX = 0;
    TileBufferCapacityY = 0;
    GLightCullingOutputs = {};
    return true;
}

const FLightCullingOutputs& FLightCullingPass::GetOutputs()
{
    return GLightCullingOutputs;
}

const FLightCullingDebugStats& FLightCullingPass::GetDebugStats()
{
    return GDebugStats;
}

bool FLightCullingPass::Begin(const FRenderPassContext* Context)
{
    if (!Context || !Context->Device || !Context->DeviceContext || !Context->RenderBus || !Context->RenderTargets)
    {
        return false;
    }

    OutSRV = PrevPassSRV;
    OutRTV = PrevPassRTV;
    return true;
}
bool FLightCullingPass::DrawCommand(const FRenderPassContext* Context)
{
    GLightCullingOutputs = {};

    if (!EnsureComputeShader(Context->Device) || !EnsureFrameBuffer(Context->Device) || !EnsureConstantBuffers(Context->Device))
    {
        return false;
    }

    const float Width = Context->RenderTargets->Width;
    const float Height = Context->RenderTargets->Height;
    if (Width <= 0.0f || Height <= 0.0f)
    {
        return true;
    }

    const uint32 TileCountX = CeilDivide(static_cast<uint32>(Width), LightCullingTileSize);
    const uint32 TileCountY = CeilDivide(static_cast<uint32>(Height), LightCullingTileSize);
    
    if (!EnsureTileBuffers(Context->Device, TileCountX, TileCountY))
    {
        return false;
    }
    
    TArray<FPointLightInfo> VisiblePointLights;
    TArray<FSpotLightInfo> VisibleSpotLights;
    const TArray<FRenderLight>& SceneLights = Context->RenderBus->GetLights();

    for (uint32 i = 0; i < SceneLights.size(); ++i)
    {
        if (SceneLights[i].Type == (uint32)ELightType::LightType_Point)
        {
            FPointLightInfo Info = {};
            Info.Position = SceneLights[i].Position;
            Info.Radius = SceneLights[i].Radius;
            Info.Color = SceneLights[i].Color;
            Info.Intensity = SceneLights[i].Intensity;
            Info.ShadowIndex = FShadowPass::GetLightToShadowIndices()[i];
            VisiblePointLights.push_back(Info);
        }
        else if (SceneLights[i].Type == (uint32)ELightType::LightType_Spot)
        {
            FSpotLightInfo Info = {};
            Info.Position = SceneLights[i].Position;
            Info.Radius = SceneLights[i].Radius;
            Info.Color = SceneLights[i].Color;
            Info.Intensity = SceneLights[i].Intensity;
            Info.Direction = SceneLights[i].Direction;
            Info.InnerConeCos = SceneLights[i].SpotInnerCos;
            Info.OuterConeCos = SceneLights[i].SpotOuterCos;
            Info.ShadowIndex = FShadowPass::GetLightToShadowIndices()[i];

            VisibleSpotLights.push_back(Info);
        }
    }

    const FVector CameraPosition = Context->RenderBus->GetCameraPosition();
    TArray<FPointLightInfo> PointLights;
    TArray<FSpotLightInfo> SpotLights;

    const uint32 SelectedPointCount = std::min<uint32>(static_cast<uint32>(VisiblePointLights.size()), MaxLocalLightNum);
    if (SelectedPointCount > 0)
    {
        TArray<FScoredLightIndex> PointSortScratch;
        PointSortScratch.reserve(VisiblePointLights.size());

        for (uint32 Index = 0; Index < VisiblePointLights.size(); ++Index)
        {
            PointSortScratch.push_back({ ComputePointLightScore(VisiblePointLights[Index], CameraPosition), Index });
        }

        std::partial_sort(
            PointSortScratch.begin(),
            PointSortScratch.begin() + SelectedPointCount,
            PointSortScratch.end(),
            SortByHigherScore);

        PointLights.reserve(SelectedPointCount);
        for (uint32 Index = 0; Index < SelectedPointCount; ++Index)
        {
            PointLights.push_back(VisiblePointLights[PointSortScratch[Index].Index]);
        }
    }

    const uint32 SelectedSpotCount = std::min<uint32>(static_cast<uint32>(VisibleSpotLights.size()), MaxLocalLightNum);
    if (SelectedSpotCount > 0)
    {
        TArray<FScoredLightIndex> SpotSortScratch;
        SpotSortScratch.reserve(VisibleSpotLights.size());

        for (uint32 Index = 0; Index < VisibleSpotLights.size(); ++Index)
        {
            SpotSortScratch.push_back({ ComputeSpotLightScore(VisibleSpotLights[Index], CameraPosition), Index });
        }

        std::partial_sort(
            SpotSortScratch.begin(),
            SpotSortScratch.begin() + SelectedSpotCount,
            SpotSortScratch.end(),
            SortByHigherScore);

        SpotLights.reserve(SelectedSpotCount);
        for (uint32 Index = 0; Index < SelectedSpotCount; ++Index)
        {
            SpotLights.push_back(VisibleSpotLights[SpotSortScratch[Index].Index]);
        }
    }

    if (!EnsureInputBuffers(Context->Device, static_cast<uint32>(PointLights.size()), static_cast<uint32>(SpotLights.size())))
    {
        return false;
    }

    if (!PointLights.empty())
    {
        D3D11_MAPPED_SUBRESOURCE Mapped = {};
        if (SUCCEEDED(Context->DeviceContext->Map(PointLightBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &Mapped)))
        {
            std::memcpy(Mapped.pData, PointLights.data(), sizeof(FPointLightInfo) * PointLights.size());
            Context->DeviceContext->Unmap(PointLightBuffer.Get(), 0);
        }
    }

    if (!SpotLights.empty())
    {
        D3D11_MAPPED_SUBRESOURCE Mapped = {};
        if (SUCCEEDED(Context->DeviceContext->Map(SpotLightBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &Mapped)))
        {
            std::memcpy(Mapped.pData, SpotLights.data(), sizeof(FSpotLightInfo) * SpotLights.size());
            Context->DeviceContext->Unmap(SpotLightBuffer.Get(), 0);
        }
    }

    // Update Frame Constant Buffer
    FFrameConstantBuffer FrameConstants = {};
    FrameConstants.View = Context->RenderBus->GetView();
    FrameConstants.Projection = Context->RenderBus->GetProj();
    FrameConstants.InverseViewProjection = (FrameConstants.View * FrameConstants.Projection).GetInverse();
    FrameConstants.InverseProjection = FrameConstants.Projection.GetInverse();
    FrameConstants.CameraPosition = Context->RenderBus->GetCameraPosition();
    FrameConstants.bIsWireframe = Context->RenderBus->GetViewMode() == EViewMode::Wireframe ? 1.0f : 0.0f;
    FrameConstants.WireframeRGB = Context->RenderBus->GetWireframeColor();

    D3D11_MAPPED_SUBRESOURCE MappedFrame = {};
    if (SUCCEEDED(Context->DeviceContext->Map(FrameConstantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &MappedFrame)))
    {
        std::memcpy(MappedFrame.pData, &FrameConstants, sizeof(FrameConstants));
        Context->DeviceContext->Unmap(FrameConstantBuffer.Get(), 0);
    }

    FForwardPlusConstants FPConstants = {};
    FPConstants.ScreenSize[0] = static_cast<uint32>(Width);
    FPConstants.ScreenSize[1] = static_cast<uint32>(Height);
    FPConstants.TileCount[0] = TileCountX;
    FPConstants.TileCount[1] = TileCountY;
    FPConstants.bEnable25DMask = 1;

    D3D11_MAPPED_SUBRESOURCE MappedFP = {};
    if (SUCCEEDED(Context->DeviceContext->Map(ForwardPlusConstantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &MappedFP)))
    {
        std::memcpy(MappedFP.pData, &FPConstants, sizeof(FPConstants));
        Context->DeviceContext->Unmap(ForwardPlusConstantBuffer.Get(), 0);
    }

    FLightingConstants LConstants = {};
    LConstants.PointLightCount = static_cast<uint32>(PointLights.size());
    LConstants.SpotLightCount = static_cast<uint32>(SpotLights.size());
    
    D3D11_MAPPED_SUBRESOURCE MappedL = {};
    if (SUCCEEDED(Context->DeviceContext->Map(LightingConstantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &MappedL)))
    {
        std::memcpy(MappedL.pData, &LConstants, sizeof(LConstants));
        Context->DeviceContext->Unmap(LightingConstantBuffer.Get(), 0);
    }

    ID3D11DeviceContext* DeviceContext = Context->DeviceContext;
    DeviceContext->OMSetRenderTargets(0, nullptr, nullptr);
    DeviceContext->CSSetShader(ComputeShader.Get(), nullptr, 0);

    ID3D11Buffer* CBuffers[] = { FrameConstantBuffer.Get() };
    DeviceContext->CSSetConstantBuffers(0, 1, CBuffers);
    ID3D11Buffer* FPCB = ForwardPlusConstantBuffer.Get();
    DeviceContext->CSSetConstantBuffers(11, 1, &FPCB);
    ID3D11Buffer* LCB = LightingConstantBuffer.Get();
    DeviceContext->CSSetConstantBuffers(13, 1, &LCB);

    ID3D11ShaderResourceView* SRVs[] = { Context->RenderTargets->SceneDepthSRV, PointLightBufferSRV.Get(), SpotLightBufferSRV.Get() };
    DeviceContext->CSSetShaderResources(0, 3, SRVs);

    ID3D11UnorderedAccessView* UAVs[] = { TilePointLightGridUAV.Get(), TilePointLightIndexUAV.Get(), TileSpotLightGridUAV.Get(), TileSpotLightIndexUAV.Get() };
    DeviceContext->CSSetUnorderedAccessViews(0, 4, UAVs, nullptr);

    DeviceContext->Dispatch(TileCountX, TileCountY, 1);

    // Set Outputs
    GLightCullingOutputs.PointLightBufferSRV = PointLightBufferSRV.Get();
    GLightCullingOutputs.SpotLightBufferSRV = SpotLightBufferSRV.Get();
    GLightCullingOutputs.TilePointLightGridSRV = TilePointLightGridSRV.Get();
    GLightCullingOutputs.TilePointLightIndexSRV = TilePointLightIndexSRV.Get();
    GLightCullingOutputs.TileSpotLightGridSRV = TileSpotLightGridSRV.Get();
    GLightCullingOutputs.TileSpotLightIndexSRV = TileSpotLightIndexSRV.Get();
    GLightCullingOutputs.TileCountX = TileCountX;
    GLightCullingOutputs.TileCountY = TileCountY;
    GLightCullingOutputs.TileSize = LightCullingTileSize;
    GLightCullingOutputs.MaxPointLightsPerTile = MaxPointLightsPerTile;
    GLightCullingOutputs.MaxSpotLightsPerTile = MaxSpotLightsPerTile;
    GLightCullingOutputs.PointLightCount = static_cast<uint32>(PointLights.size());
    GLightCullingOutputs.SpotLightCount = static_cast<uint32>(SpotLights.size());

    GDebugStats.PointLightCount = static_cast<uint32>(PointLights.size());
    GDebugStats.SpotLightCount = static_cast<uint32>(SpotLights.size());
    GDebugStats.LightCount = GDebugStats.PointLightCount + GDebugStats.SpotLightCount;
    GDebugStats.TileCountX = TileCountX;
    GDebugStats.TileCountY = TileCountY;
    GDebugStats.TileCount = TileCountX * TileCountY;

    return true;
}
bool FLightCullingPass::End(const FRenderPassContext* Context)
{
    ID3D11ShaderResourceView* NullSRVs[3] = { nullptr, nullptr, nullptr };
    Context->DeviceContext->CSSetShaderResources(0, 3, NullSRVs);

    ID3D11UnorderedAccessView* NullUAVs[4] = { nullptr, nullptr, nullptr, nullptr };
    Context->DeviceContext->CSSetUnorderedAccessViews(0, 4, NullUAVs, nullptr);

    ID3D11Buffer* NullCBs[3] = { nullptr, nullptr, nullptr };
    Context->DeviceContext->CSSetConstantBuffers(0, 1, &NullCBs[0]);
    Context->DeviceContext->CSSetConstantBuffers(11, 1, &NullCBs[1]);
    Context->DeviceContext->CSSetConstantBuffers(13, 1, &NullCBs[2]);

    Context->DeviceContext->CSSetShader(nullptr, nullptr, 0);
    return true;
}

bool FLightCullingPass::EnsureComputeShader(ID3D11Device* Device)
{
    if (ComputeShader)
    {
        return true;
    }

    TComPtr<ID3DBlob> CSBlob;
    TComPtr<ID3DBlob> ErrorBlob;
    const HRESULT CompileResult = D3DCompileFromFile(
        FPaths::ToWide("Shaders/Multipass/LightCulling25DCS.hlsl").c_str(),
        nullptr,
        D3D_COMPILE_STANDARD_FILE_INCLUDE,
        "TileLightCulling25DCS",
        "cs_5_0",
        0,
        0,
        CSBlob.GetAddressOf(),
        ErrorBlob.GetAddressOf());

    if (FAILED(CompileResult))
    {
        if (ErrorBlob)
        {
            UE_LOG("LightCulling 2.5D CS Compile Error: %s", static_cast<const char*>(ErrorBlob->GetBufferPointer()));
        }
        else
        {
            UE_LOG("Failed to compile LightCulling25DCS.hlsl");
        }
        return false;
    }

    const HRESULT CreateResult =
        Device->CreateComputeShader(CSBlob->GetBufferPointer(), CSBlob->GetBufferSize(), nullptr, ComputeShader.GetAddressOf());
    if (FAILED(CreateResult))
    {
        UE_LOG("Failed to create LightCulling 2.5D compute shader");
        return false;
    }

    return true;
}

bool FLightCullingPass::EnsureFrameBuffer(ID3D11Device* Device)
{
    if (FrameConstantBuffer) return true;

    D3D11_BUFFER_DESC Desc = {};
    Desc.ByteWidth = sizeof(FFrameConstantBuffer);
    Desc.Usage = D3D11_USAGE_DYNAMIC;
    Desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    Desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    return SUCCEEDED(Device->CreateBuffer(&Desc, nullptr, FrameConstantBuffer.GetAddressOf()));
}

bool FLightCullingPass::EnsureInputBuffers(ID3D11Device* Device, uint32 PointCount, uint32 SpotCount)
{
    if (PointCount > PointLightBufferCapacity || !PointLightBuffer)
    {
        PointLightBuffer.Reset();
        PointLightBufferSRV.Reset();

        uint32 Capacity = std::max(PointCount, 16u);
        D3D11_BUFFER_DESC Desc = {};
        Desc.ByteWidth = sizeof(FPointLightInfo) * Capacity;
        Desc.Usage = D3D11_USAGE_DYNAMIC;
        Desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        Desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        Desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        Desc.StructureByteStride = sizeof(FPointLightInfo);

        if (FAILED(Device->CreateBuffer(&Desc, nullptr, PointLightBuffer.GetAddressOf()))) return false;
        
        D3D11_SHADER_RESOURCE_VIEW_DESC SRVDesc = {};
        SRVDesc.Format = DXGI_FORMAT_UNKNOWN;
        SRVDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        SRVDesc.Buffer.NumElements = Capacity;
        if (FAILED(Device->CreateShaderResourceView(PointLightBuffer.Get(), &SRVDesc, PointLightBufferSRV.GetAddressOf()))) return false;
        
        PointLightBufferCapacity = Capacity;
    }

    if (SpotCount > SpotLightBufferCapacity || !SpotLightBuffer)
    {
        SpotLightBuffer.Reset();
        SpotLightBufferSRV.Reset();

        uint32 Capacity = std::max(SpotCount, 16u);
        D3D11_BUFFER_DESC Desc = {};
        Desc.ByteWidth = sizeof(FSpotLightInfo) * Capacity;
        Desc.Usage = D3D11_USAGE_DYNAMIC;
        Desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        Desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        Desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        Desc.StructureByteStride = sizeof(FSpotLightInfo);

        if (FAILED(Device->CreateBuffer(&Desc, nullptr, SpotLightBuffer.GetAddressOf()))) return false;

        D3D11_SHADER_RESOURCE_VIEW_DESC SRVDesc = {};
        SRVDesc.Format = DXGI_FORMAT_UNKNOWN;
        SRVDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        SRVDesc.Buffer.NumElements = Capacity;
        if (FAILED(Device->CreateShaderResourceView(SpotLightBuffer.Get(), &SRVDesc, SpotLightBufferSRV.GetAddressOf()))) return false;

        SpotLightBufferCapacity = Capacity;
    }

    return true;
}

bool FLightCullingPass::EnsureTileBuffers(ID3D11Device* Device, uint32 TileCountX, uint32 TileCountY)
{
    if (TileCountX == TileBufferCapacityX && TileCountY == TileBufferCapacityY && TilePointLightGridBuffer)
    {
        return true;
    }

    TilePointLightGridBuffer.Reset();
    TilePointLightGridUAV.Reset();
    TilePointLightGridSRV.Reset();
    TilePointLightIndexBuffer.Reset();
    TilePointLightIndexUAV.Reset();
    TilePointLightIndexSRV.Reset();
    TileSpotLightGridBuffer.Reset();
    TileSpotLightGridUAV.Reset();
    TileSpotLightGridSRV.Reset();
    TileSpotLightIndexBuffer.Reset();
    TileSpotLightIndexUAV.Reset();
    TileSpotLightIndexSRV.Reset();

    uint32 TileCount = TileCountX * TileCountY;

    // Grid Buffers (uint2 per tile)
    D3D11_BUFFER_DESC GridDesc = {};
    GridDesc.ByteWidth = sizeof(uint32) * 2 * TileCount;
    GridDesc.Usage = D3D11_USAGE_DEFAULT;
    GridDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
    GridDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    GridDesc.StructureByteStride = sizeof(uint32) * 2;

    if (FAILED(Device->CreateBuffer(&GridDesc, nullptr, TilePointLightGridBuffer.GetAddressOf()))) return false;
    if (FAILED(Device->CreateBuffer(&GridDesc, nullptr, TileSpotLightGridBuffer.GetAddressOf()))) return false;

    D3D11_UNORDERED_ACCESS_VIEW_DESC GridUAV = {};
    GridUAV.Format = DXGI_FORMAT_UNKNOWN;
    GridUAV.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    GridUAV.Buffer.NumElements = TileCount;

    if (FAILED(Device->CreateUnorderedAccessView(TilePointLightGridBuffer.Get(), &GridUAV, TilePointLightGridUAV.GetAddressOf()))) return false;
    if (FAILED(Device->CreateUnorderedAccessView(TileSpotLightGridBuffer.Get(), &GridUAV, TileSpotLightGridUAV.GetAddressOf()))) return false;

    D3D11_SHADER_RESOURCE_VIEW_DESC GridSRV = {};
    GridSRV.Format = DXGI_FORMAT_UNKNOWN;
    GridSRV.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    GridSRV.Buffer.NumElements = TileCount;

    if (FAILED(Device->CreateShaderResourceView(TilePointLightGridBuffer.Get(), &GridSRV, TilePointLightGridSRV.GetAddressOf()))) return false;
    if (FAILED(Device->CreateShaderResourceView(TileSpotLightGridBuffer.Get(), &GridSRV, TileSpotLightGridSRV.GetAddressOf()))) return false;

    // Index Buffers
    D3D11_BUFFER_DESC IndexDesc = {};
    IndexDesc.Usage = D3D11_USAGE_DEFAULT;
    IndexDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
    IndexDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    IndexDesc.StructureByteStride = sizeof(uint32);

    IndexDesc.ByteWidth = sizeof(uint32) * TileCount * MaxPointLightsPerTile;
    if (FAILED(Device->CreateBuffer(&IndexDesc, nullptr, TilePointLightIndexBuffer.GetAddressOf()))) return false;

    IndexDesc.ByteWidth = sizeof(uint32) * TileCount * MaxSpotLightsPerTile;
    if (FAILED(Device->CreateBuffer(&IndexDesc, nullptr, TileSpotLightIndexBuffer.GetAddressOf()))) return false;

    D3D11_UNORDERED_ACCESS_VIEW_DESC IndexUAV = {};
    IndexUAV.Format = DXGI_FORMAT_UNKNOWN;
    IndexUAV.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;

    IndexUAV.Buffer.NumElements = TileCount * MaxPointLightsPerTile;
    if (FAILED(Device->CreateUnorderedAccessView(TilePointLightIndexBuffer.Get(), &IndexUAV, TilePointLightIndexUAV.GetAddressOf()))) return false;

    IndexUAV.Buffer.NumElements = TileCount * MaxSpotLightsPerTile;
    if (FAILED(Device->CreateUnorderedAccessView(TileSpotLightIndexBuffer.Get(), &IndexUAV, TileSpotLightIndexUAV.GetAddressOf()))) return false;

    D3D11_SHADER_RESOURCE_VIEW_DESC IndexSRV = {};
    IndexSRV.Format = DXGI_FORMAT_UNKNOWN;
    IndexSRV.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;

    IndexSRV.Buffer.NumElements = TileCount * MaxPointLightsPerTile;
    if (FAILED(Device->CreateShaderResourceView(TilePointLightIndexBuffer.Get(), &IndexSRV, TilePointLightIndexSRV.GetAddressOf()))) return false;

    IndexSRV.Buffer.NumElements = TileCount * MaxSpotLightsPerTile;
    if (FAILED(Device->CreateShaderResourceView(TileSpotLightIndexBuffer.Get(), &IndexSRV, TileSpotLightIndexSRV.GetAddressOf()))) return false;

    TileBufferCapacityX = TileCountX;
    TileBufferCapacityY = TileCountY;
    return true;
}

bool FLightCullingPass::EnsureConstantBuffers(ID3D11Device* Device)
{
    if (ForwardPlusConstantBuffer && LightingConstantBuffer) return true;

    D3D11_BUFFER_DESC Desc = {};
    Desc.Usage = D3D11_USAGE_DYNAMIC;
    Desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    Desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    if (!ForwardPlusConstantBuffer)
    {
        Desc.ByteWidth = sizeof(FForwardPlusConstants);
        if (FAILED(Device->CreateBuffer(&Desc, nullptr, ForwardPlusConstantBuffer.GetAddressOf()))) return false;
    }

    if (!LightingConstantBuffer)
    {
        Desc.ByteWidth = sizeof(FLightingConstants);
        if (FAILED(Device->CreateBuffer(&Desc, nullptr, LightingConstantBuffer.GetAddressOf()))) return false;
    }

    return true;
}
