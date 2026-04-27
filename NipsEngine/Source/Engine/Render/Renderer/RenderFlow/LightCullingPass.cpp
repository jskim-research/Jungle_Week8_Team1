#include "LightCullingPass.h"

#include "Core/Paths.h"
#include "Render/Scene/RenderBus.h"
#include "Render/Scene/RenderCommand.h"
#include "UI/EditorConsoleWidget.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <d3dcompiler.h>

namespace
{
    constexpr uint32 LightCullingTileSize = 16;
    constexpr uint32 MaxPointLightsPerTile = 256;
    constexpr uint32 MaxSpotLightsPerTile = 256;
    constexpr uint32 DebugLogIntervalFrames = 30;

    struct FForwardPlusFrameConstants
    {
        FMatrix View;
        FMatrix InvProjection;
    };

    struct FForwardPlusConstants
    {
        uint32 ViewportMinX = 0;
        uint32 ViewportMinY = 0;
        uint32 ViewportSizeX = 0;
        uint32 ViewportSizeY = 0;
        uint32 DepthTextureSizeX = 0;
        uint32 DepthTextureSizeY = 0;
        uint32 TileCountX = 0;
        uint32 TileCountY = 0;
        uint32 bEnable25DMask = 1;
        float Padding[3] = { 0.0f, 0.0f, 0.0f };
    };

    struct FLightingConstants
    {
        FVector UnusedAmbientColor = FVector::ZeroVector;
        float UnusedAmbientIntensity = 0.0f;
        uint32 DirectionalLightCount = 0;
        uint32 PointLightCount = 0;
        uint32 SpotLightCount = 0;
        float LightingPad = 0.0f;
    };

    struct alignas(16) FPointLightInfo
    {
        FVector Position = FVector::ZeroVector;
        float Radius = 0.0f;
        FVector Color = FVector::ZeroVector;
        float Intensity = 0.0f;
        float RadiusFalloff = 1.0f;
        float Padding[3] = { 0.0f, 0.0f, 0.0f };
    };

    struct alignas(16) FSpotLightInfo
    {
        FVector Position = FVector::ZeroVector;
        float Radius = 0.0f;

        FVector Color = FVector::ZeroVector;
        float Intensity = 0.0f;

        FVector Direction = FVector::ForwardVector;
        float InnerConeCos = 1.0f;

        float OuterConeCos = 0.0f;
        float RadiusFalloff = 1.0f;
        float Padding[2] = { 0.0f, 0.0f };
    };

    struct FSpotBroadPhaseBounds
    {
        FVector Center = FVector::ZeroVector;
        float Radius = 0.0f;
    };

    struct FUInt2
    {
        uint32 X = 0;
        uint32 Y = 0;
    };

    static_assert(sizeof(FPointLightInfo) == 48, "FPointLightInfo layout must match HLSL.");
    static_assert(sizeof(FSpotLightInfo) == 64, "FSpotLightInfo layout must match HLSL.");
    static_assert(sizeof(FUInt2) == 8, "FUInt2 layout must match HLSL uint2.");

    uint32 CeilDivide(uint32 Numerator, uint32 Denominator)
    {
        return (Numerator + Denominator - 1u) / Denominator;
    }

    float ComputePointLightScore(const FRenderLight& Light, const FVector& CameraPos)
    {
        const float Radius = std::max(Light.Radius, 0.001f);
        const float DistanceToCenter = FVector::Dist(CameraPos, Light.Position);
        const float VolumeDist = std::max(0.0f, DistanceToCenter - Radius);
        const float Influence = std::max(Light.Intensity, 0.0f) * Radius * Radius;
        return Influence / (1.0f + VolumeDist * VolumeDist);
    }

    FSpotBroadPhaseBounds BuildSpotBroadPhaseBounds(const FRenderLight& Light)
    {
        const float Height = std::max(Light.Radius, 0.001f);
        const FVector Axis = Light.Direction.GetSafeNormal();

        const float CosTheta = std::clamp(Light.SpotOuterCos, 0.001f, 0.9999f);
        const float SinTheta = std::sqrt(std::max(0.0f, 1.0f - CosTheta * CosTheta));
        const float BaseRadius = Height * (SinTheta / CosTheta);
        const FVector BaseCenter = Light.Position + Axis * Height;

        FSpotBroadPhaseBounds Bounds = {};
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

    float ComputeSpotLightScore(const FRenderLight& Light, const FVector& CameraPos)
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

    PointLightBuffer.Reset();
    PointLightBufferSRV.Reset();
    SpotLightBuffer.Reset();
    SpotLightBufferSRV.Reset();

    TilePointLightGridBuffer.Reset();
    TilePointLightGridReadbackBuffer.Reset();
    TilePointLightGridUAV.Reset();
    TilePointLightGridSRV.Reset();
    TilePointLightIndexBuffer.Reset();
    TilePointLightIndexUAV.Reset();
    TilePointLightIndexSRV.Reset();

    TileSpotLightGridBuffer.Reset();
    TileSpotLightGridReadbackBuffer.Reset();
    TileSpotLightGridUAV.Reset();
    TileSpotLightGridSRV.Reset();
    TileSpotLightIndexBuffer.Reset();
    TileSpotLightIndexUAV.Reset();
    TileSpotLightIndexSRV.Reset();

    FrameConstantBuffer.Reset();
    ForwardPlusConstantBuffer.Reset();
    LightingConstantBuffer.Reset();

    PointLightBufferCapacity = 0;
    SpotLightBufferCapacity = 0;
    TileBufferCapacity = 0;

    GLightCullingOutputs = {};
    GDebugStats = {};
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

    if (!EnsureComputeShader(Context->Device) || !EnsureConstantBuffers(Context->Device))
    {
        return false;
    }

    const float WidthF = Context->RenderTargets->Width;
    const float HeightF = Context->RenderTargets->Height;
    if (WidthF <= 0.0f || HeightF <= 0.0f)
    {
        return true;
    }

    const uint32 ViewportWidth = static_cast<uint32>(WidthF);
    const uint32 ViewportHeight = static_cast<uint32>(HeightF);
    const uint32 TileCountX = CeilDivide(ViewportWidth, LightCullingTileSize);
    const uint32 TileCountY = CeilDivide(ViewportHeight, LightCullingTileSize);
    const uint32 TileCount = TileCountX * TileCountY;
    if (TileCount == 0)
    {
        return true;
    }

    if (!EnsureTileBuffers(Context->Device, TileCount))
    {
        return false;
    }

    const TArray<FRenderLight>& SceneLights = Context->RenderBus->GetLights();
    const FVector& CameraPos = Context->RenderBus->GetCameraPosition();

    using FPointWithScore = TPair<float, FPointLightInfo>;
    using FSpotWithScore = TPair<float, FSpotLightInfo>;

    TArray<FPointWithScore> PointHeap;
    TArray<FSpotWithScore> SpotHeap;
    PointHeap.reserve(MaxPointLightsPerTile + 1);
    SpotHeap.reserve(MaxSpotLightsPerTile + 1);

    auto HeapCmp = [](const auto& A, const auto& B)
    {
        return A.first > B.first;
    };

    for (const FRenderLight& Light : SceneLights)
    {
        if (Light.Type == static_cast<uint32>(ELightType::LightType_Point))
        {
            FPointLightInfo Point = {};
            Point.Position = Light.Position;
            Point.Radius = Light.Radius;
            Point.Color = Light.Color;
            Point.Intensity = Light.Intensity;
            Point.RadiusFalloff = Light.FalloffExponent;

            const float Score = ComputePointLightScore(Light, CameraPos);
            PointHeap.push_back({ Score, Point });
            std::push_heap(PointHeap.begin(), PointHeap.end(), HeapCmp);
            if (PointHeap.size() > MaxPointLightsPerTile)
            {
                std::pop_heap(PointHeap.begin(), PointHeap.end(), HeapCmp);
                PointHeap.pop_back();
            }
        }
        else if (Light.Type == static_cast<uint32>(ELightType::LightType_Spot))
        {
            FSpotLightInfo Spot = {};
            Spot.Position = Light.Position;
            Spot.Radius = Light.Radius;
            Spot.Color = Light.Color;
            Spot.Intensity = Light.Intensity;
            Spot.Direction = Light.Direction;
            Spot.InnerConeCos = Light.SpotInnerCos;
            Spot.OuterConeCos = Light.SpotOuterCos;
            Spot.RadiusFalloff = Light.FalloffExponent;

            const float Score = ComputeSpotLightScore(Light, CameraPos);
            SpotHeap.push_back({ Score, Spot });
            std::push_heap(SpotHeap.begin(), SpotHeap.end(), HeapCmp);
            if (SpotHeap.size() > MaxSpotLightsPerTile)
            {
                std::pop_heap(SpotHeap.begin(), SpotHeap.end(), HeapCmp);
                SpotHeap.pop_back();
            }
        }
    }

    TArray<FPointLightInfo> PointLights;
    TArray<FSpotLightInfo> SpotLights;
    PointLights.reserve(PointHeap.size());
    SpotLights.reserve(SpotHeap.size());

    for (FPointWithScore& Entry : PointHeap)
    {
        PointLights.push_back(std::move(Entry.second));
    }
    for (FSpotWithScore& Entry : SpotHeap)
    {
        SpotLights.push_back(std::move(Entry.second));
    }

    const uint32 PointLightCount = static_cast<uint32>(PointLights.size());
    const uint32 SpotLightCount = static_cast<uint32>(SpotLights.size());
    const uint32 LocalLightCount = PointLightCount + SpotLightCount;

    if (!EnsureInputLightBuffers(Context->Device, PointLightCount, SpotLightCount))
    {
        return false;
    }

    if (PointLightCount > 0)
    {
        D3D11_MAPPED_SUBRESOURCE MappedPoint = {};
        if (FAILED(Context->DeviceContext->Map(PointLightBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &MappedPoint)))
        {
            return false;
        }

        std::memcpy(MappedPoint.pData, PointLights.data(), sizeof(FPointLightInfo) * PointLightCount);
        Context->DeviceContext->Unmap(PointLightBuffer.Get(), 0);
    }

    if (SpotLightCount > 0)
    {
        D3D11_MAPPED_SUBRESOURCE MappedSpot = {};
        if (FAILED(Context->DeviceContext->Map(SpotLightBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &MappedSpot)))
        {
            return false;
        }

        std::memcpy(MappedSpot.pData, SpotLights.data(), sizeof(FSpotLightInfo) * SpotLightCount);
        Context->DeviceContext->Unmap(SpotLightBuffer.Get(), 0);
    }

    const FForwardPlusFrameConstants FrameConstants =
    {
        Context->RenderBus->GetView(),
        Context->RenderBus->GetProj().GetInverse()
    };

    const FForwardPlusConstants ForwardConstants =
    {
        0u,
        0u,
        ViewportWidth,
        ViewportHeight,
        ViewportWidth,
        ViewportHeight,
        TileCountX,
        TileCountY,
        1u,
        { 0.0f, 0.0f, 0.0f }
    };

    FLightingConstants LightingConstants = {};
    LightingConstants.DirectionalLightCount = 0;
    LightingConstants.PointLightCount = PointLightCount;
    LightingConstants.SpotLightCount = SpotLightCount;

    auto UploadConstant = [&](ID3D11Buffer* Buffer, const void* Data, size_t Size) -> bool
    {
        D3D11_MAPPED_SUBRESOURCE Mapped = {};
        if (FAILED(Context->DeviceContext->Map(Buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &Mapped)))
        {
            return false;
        }

        std::memcpy(Mapped.pData, Data, Size);
        Context->DeviceContext->Unmap(Buffer, 0);
        return true;
    };

    if (!UploadConstant(FrameConstantBuffer.Get(), &FrameConstants, sizeof(FrameConstants)) ||
        !UploadConstant(ForwardPlusConstantBuffer.Get(), &ForwardConstants, sizeof(ForwardConstants)) ||
        !UploadConstant(LightingConstantBuffer.Get(), &LightingConstants, sizeof(LightingConstants)))
    {
        return false;
    }

    Context->DeviceContext->CSSetShader(ComputeShader.Get(), nullptr, 0);

    ID3D11Buffer* FrameCB = FrameConstantBuffer.Get();
    ID3D11Buffer* ForwardCB = ForwardPlusConstantBuffer.Get();
    ID3D11Buffer* LightingCB = LightingConstantBuffer.Get();
    Context->DeviceContext->CSSetConstantBuffers(0, 1, &FrameCB);
    Context->DeviceContext->CSSetConstantBuffers(1, 1, &ForwardCB);
    Context->DeviceContext->CSSetConstantBuffers(2, 1, &LightingCB);

    ID3D11ShaderResourceView* DepthSRV = Context->RenderTargets->SceneDepthSRV;
    if (!DepthSRV)
    {
        return true;
    }
    ID3D11ShaderResourceView* SRVs[3] =
    {
        DepthSRV,
        PointLightCount > 0 ? PointLightBufferSRV.Get() : nullptr,
        SpotLightCount > 0 ? SpotLightBufferSRV.Get() : nullptr
    };
    Context->DeviceContext->CSSetShaderResources(0, 3, SRVs);

    const UINT ClearValues[4] = { 0u, 0u, 0u, 0u };
    Context->DeviceContext->ClearUnorderedAccessViewUint(TilePointLightGridUAV.Get(), ClearValues);
    Context->DeviceContext->ClearUnorderedAccessViewUint(TilePointLightIndexUAV.Get(), ClearValues);
    Context->DeviceContext->ClearUnorderedAccessViewUint(TileSpotLightGridUAV.Get(), ClearValues);
    Context->DeviceContext->ClearUnorderedAccessViewUint(TileSpotLightIndexUAV.Get(), ClearValues);

    ID3D11UnorderedAccessView* UAVs[4] =
    {
        TilePointLightGridUAV.Get(),
        TilePointLightIndexUAV.Get(),
        TileSpotLightGridUAV.Get(),
        TileSpotLightIndexUAV.Get()
    };
    Context->DeviceContext->CSSetUnorderedAccessViews(0, 4, UAVs, nullptr);

    Context->DeviceContext->Dispatch(TileCountX, TileCountY, 1);

    GLightCullingOutputs.PointLightBufferSRV = PointLightCount > 0 ? PointLightBufferSRV.Get() : nullptr;
    GLightCullingOutputs.SpotLightBufferSRV = SpotLightCount > 0 ? SpotLightBufferSRV.Get() : nullptr;
    GLightCullingOutputs.TilePointLightGridSRV = TilePointLightGridSRV.Get();
    GLightCullingOutputs.TilePointLightIndexSRV = TilePointLightIndexSRV.Get();
    GLightCullingOutputs.TileSpotLightGridSRV = TileSpotLightGridSRV.Get();
    GLightCullingOutputs.TileSpotLightIndexSRV = TileSpotLightIndexSRV.Get();
    GLightCullingOutputs.TileCountX = TileCountX;
    GLightCullingOutputs.TileCountY = TileCountY;
    GLightCullingOutputs.TileSize = LightCullingTileSize;
    GLightCullingOutputs.MaxPointLightsPerTile = MaxPointLightsPerTile;
    GLightCullingOutputs.MaxSpotLightsPerTile = MaxSpotLightsPerTile;
    GLightCullingOutputs.PointLightCount = PointLightCount;
    GLightCullingOutputs.SpotLightCount = SpotLightCount;
    GLightCullingOutputs.MaxLightsPerTile = MaxPointLightsPerTile + MaxSpotLightsPerTile;
    GLightCullingOutputs.LightCount = LocalLightCount;

    EmitDebugStats(Context, TileCountX, TileCountY);
    return true;
}

bool FLightCullingPass::End(const FRenderPassContext* Context)
{
    ID3D11ShaderResourceView* NullSRVs[3] = { nullptr, nullptr, nullptr };
    Context->DeviceContext->CSSetShaderResources(0, 3, NullSRVs);

    ID3D11UnorderedAccessView* NullUAVs[4] = { nullptr, nullptr, nullptr, nullptr };
    Context->DeviceContext->CSSetUnorderedAccessViews(0, 4, NullUAVs, nullptr);

    ID3D11Buffer* NullCB = nullptr;
    Context->DeviceContext->CSSetConstantBuffers(0, 1, &NullCB);
    Context->DeviceContext->CSSetConstantBuffers(1, 1, &NullCB);
    Context->DeviceContext->CSSetConstantBuffers(2, 1, &NullCB);

    Context->DeviceContext->CSSetShader(nullptr, nullptr, 0);
    return true;
}

bool FLightCullingPass::EnsureComputeShader(ID3D11Device* Device)
{
    if (ComputeShader)
    {
        return true;
    }

    const std::wstring ShaderPath = FPaths::ToAbsolute(FPaths::ToWide("Shaders/Multipass/LightCulling25DCS.hlsl"));

    TComPtr<ID3DBlob> CSBlob;
    TComPtr<ID3DBlob> ErrorBlob;
    const HRESULT CompileResult = D3DCompileFromFile(
        ShaderPath.c_str(),
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
            UE_LOG("LightCulling25D CS Compile Error: %s", static_cast<const char*>(ErrorBlob->GetBufferPointer()));
        }
        else
        {
            UE_LOG("Failed to compile LightCulling25DCS.hlsl");
        }
        return false;
    }

    if (FAILED(Device->CreateComputeShader(CSBlob->GetBufferPointer(), CSBlob->GetBufferSize(), nullptr, ComputeShader.GetAddressOf())))
    {
        UE_LOG("Failed to create LightCulling25D compute shader");
        return false;
    }

    return true;
}

bool FLightCullingPass::EnsureInputLightBuffers(ID3D11Device* Device, uint32 RequiredPointLightCount, uint32 RequiredSpotLightCount)
{
    auto EnsureBuffer = [&](uint32 RequiredCount,
                            uint32 ElementSize,
                            TComPtr<ID3D11Buffer>& Buffer,
                            TComPtr<ID3D11ShaderResourceView>& SRV,
                            uint32& Capacity) -> bool
    {
        if (RequiredCount == 0)
        {
            return true;
        }

        if (RequiredCount <= Capacity && Buffer && SRV)
        {
            return true;
        }

        D3D11_BUFFER_DESC BufferDesc = {};
        BufferDesc.Usage = D3D11_USAGE_DYNAMIC;
        BufferDesc.ByteWidth = ElementSize * RequiredCount;
        BufferDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        BufferDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        BufferDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        BufferDesc.StructureByteStride = ElementSize;

        TComPtr<ID3D11Buffer> NewBuffer;
        if (FAILED(Device->CreateBuffer(&BufferDesc, nullptr, NewBuffer.GetAddressOf())))
        {
            return false;
        }

        D3D11_SHADER_RESOURCE_VIEW_DESC SRVDesc = {};
        SRVDesc.Format = DXGI_FORMAT_UNKNOWN;
        SRVDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        SRVDesc.Buffer.FirstElement = 0;
        SRVDesc.Buffer.NumElements = RequiredCount;

        TComPtr<ID3D11ShaderResourceView> NewSRV;
        if (FAILED(Device->CreateShaderResourceView(NewBuffer.Get(), &SRVDesc, NewSRV.GetAddressOf())))
        {
            return false;
        }

        Buffer = std::move(NewBuffer);
        SRV = std::move(NewSRV);
        Capacity = RequiredCount;
        return true;
    };

    return EnsureBuffer(RequiredPointLightCount,
                        sizeof(FPointLightInfo),
                        PointLightBuffer,
                        PointLightBufferSRV,
                        PointLightBufferCapacity)
        && EnsureBuffer(RequiredSpotLightCount,
                        sizeof(FSpotLightInfo),
                        SpotLightBuffer,
                        SpotLightBufferSRV,
                        SpotLightBufferCapacity);
}

bool FLightCullingPass::EnsureTileBuffers(ID3D11Device* Device, uint32 RequiredTileCount)
{
    const bool bHasAllResources =
        TilePointLightGridBuffer && TilePointLightGridReadbackBuffer && TilePointLightGridUAV && TilePointLightGridSRV &&
        TilePointLightIndexBuffer && TilePointLightIndexUAV && TilePointLightIndexSRV &&
        TileSpotLightGridBuffer && TileSpotLightGridReadbackBuffer && TileSpotLightGridUAV && TileSpotLightGridSRV &&
        TileSpotLightIndexBuffer && TileSpotLightIndexUAV && TileSpotLightIndexSRV;

    if (RequiredTileCount <= TileBufferCapacity && bHasAllResources)
    {
        return true;
    }

    TilePointLightGridBuffer.Reset();
    TilePointLightGridReadbackBuffer.Reset();
    TilePointLightGridUAV.Reset();
    TilePointLightGridSRV.Reset();
    TilePointLightIndexBuffer.Reset();
    TilePointLightIndexUAV.Reset();
    TilePointLightIndexSRV.Reset();

    TileSpotLightGridBuffer.Reset();
    TileSpotLightGridReadbackBuffer.Reset();
    TileSpotLightGridUAV.Reset();
    TileSpotLightGridSRV.Reset();
    TileSpotLightIndexBuffer.Reset();
    TileSpotLightIndexUAV.Reset();
    TileSpotLightIndexSRV.Reset();

    const uint32 PointIndexCount = RequiredTileCount * MaxPointLightsPerTile;
    const uint32 SpotIndexCount = RequiredTileCount * MaxSpotLightsPerTile;

    D3D11_BUFFER_DESC GridBufferDesc = {};
    GridBufferDesc.Usage = D3D11_USAGE_DEFAULT;
    GridBufferDesc.ByteWidth = sizeof(FUInt2) * RequiredTileCount;
    GridBufferDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
    GridBufferDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    GridBufferDesc.StructureByteStride = sizeof(FUInt2);

    D3D11_BUFFER_DESC GridReadbackDesc = {};
    GridReadbackDesc.Usage = D3D11_USAGE_STAGING;
    GridReadbackDesc.ByteWidth = sizeof(FUInt2) * RequiredTileCount;
    GridReadbackDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    GridReadbackDesc.StructureByteStride = sizeof(FUInt2);

    D3D11_UNORDERED_ACCESS_VIEW_DESC GridUAVDesc = {};
    GridUAVDesc.Format = DXGI_FORMAT_UNKNOWN;
    GridUAVDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    GridUAVDesc.Buffer.FirstElement = 0;
    GridUAVDesc.Buffer.NumElements = RequiredTileCount;

    D3D11_SHADER_RESOURCE_VIEW_DESC GridSRVDesc = {};
    GridSRVDesc.Format = DXGI_FORMAT_UNKNOWN;
    GridSRVDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    GridSRVDesc.Buffer.FirstElement = 0;
    GridSRVDesc.Buffer.NumElements = RequiredTileCount;

    if (FAILED(Device->CreateBuffer(&GridBufferDesc, nullptr, TilePointLightGridBuffer.GetAddressOf())) ||
        FAILED(Device->CreateBuffer(&GridReadbackDesc, nullptr, TilePointLightGridReadbackBuffer.GetAddressOf())) ||
        FAILED(Device->CreateUnorderedAccessView(TilePointLightGridBuffer.Get(), &GridUAVDesc, TilePointLightGridUAV.GetAddressOf())) ||
        FAILED(Device->CreateShaderResourceView(TilePointLightGridBuffer.Get(), &GridSRVDesc, TilePointLightGridSRV.GetAddressOf())))
    {
        return false;
    }

    if (FAILED(Device->CreateBuffer(&GridBufferDesc, nullptr, TileSpotLightGridBuffer.GetAddressOf())) ||
        FAILED(Device->CreateBuffer(&GridReadbackDesc, nullptr, TileSpotLightGridReadbackBuffer.GetAddressOf())) ||
        FAILED(Device->CreateUnorderedAccessView(TileSpotLightGridBuffer.Get(), &GridUAVDesc, TileSpotLightGridUAV.GetAddressOf())) ||
        FAILED(Device->CreateShaderResourceView(TileSpotLightGridBuffer.Get(), &GridSRVDesc, TileSpotLightGridSRV.GetAddressOf())))
    {
        return false;
    }

    D3D11_BUFFER_DESC IndexBufferDesc = {};
    IndexBufferDesc.Usage = D3D11_USAGE_DEFAULT;
    IndexBufferDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
    IndexBufferDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    IndexBufferDesc.StructureByteStride = sizeof(uint32);

    D3D11_UNORDERED_ACCESS_VIEW_DESC IndexUAVDesc = {};
    IndexUAVDesc.Format = DXGI_FORMAT_UNKNOWN;
    IndexUAVDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    IndexUAVDesc.Buffer.FirstElement = 0;

    D3D11_SHADER_RESOURCE_VIEW_DESC IndexSRVDesc = {};
    IndexSRVDesc.Format = DXGI_FORMAT_UNKNOWN;
    IndexSRVDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    IndexSRVDesc.Buffer.FirstElement = 0;

    IndexBufferDesc.ByteWidth = sizeof(uint32) * PointIndexCount;
    IndexUAVDesc.Buffer.NumElements = PointIndexCount;
    IndexSRVDesc.Buffer.NumElements = PointIndexCount;

    if (FAILED(Device->CreateBuffer(&IndexBufferDesc, nullptr, TilePointLightIndexBuffer.GetAddressOf())) ||
        FAILED(Device->CreateUnorderedAccessView(TilePointLightIndexBuffer.Get(), &IndexUAVDesc, TilePointLightIndexUAV.GetAddressOf())) ||
        FAILED(Device->CreateShaderResourceView(TilePointLightIndexBuffer.Get(), &IndexSRVDesc, TilePointLightIndexSRV.GetAddressOf())))
    {
        return false;
    }

    IndexBufferDesc.ByteWidth = sizeof(uint32) * SpotIndexCount;
    IndexUAVDesc.Buffer.NumElements = SpotIndexCount;
    IndexSRVDesc.Buffer.NumElements = SpotIndexCount;

    if (FAILED(Device->CreateBuffer(&IndexBufferDesc, nullptr, TileSpotLightIndexBuffer.GetAddressOf())) ||
        FAILED(Device->CreateUnorderedAccessView(TileSpotLightIndexBuffer.Get(), &IndexUAVDesc, TileSpotLightIndexUAV.GetAddressOf())) ||
        FAILED(Device->CreateShaderResourceView(TileSpotLightIndexBuffer.Get(), &IndexSRVDesc, TileSpotLightIndexSRV.GetAddressOf())))
    {
        return false;
    }

    TileBufferCapacity = RequiredTileCount;
    return true;
}

bool FLightCullingPass::EnsureConstantBuffers(ID3D11Device* Device)
{
    const auto RoundUp16 = [](uint32 Size) -> uint32
    {
        return (Size + 15u) & ~15u;
    };

    auto EnsureConstantBuffer = [&](TComPtr<ID3D11Buffer>& Buffer, uint32 ByteWidth) -> bool
    {
        if (Buffer)
        {
            return true;
        }

        D3D11_BUFFER_DESC Desc = {};
        Desc.ByteWidth = RoundUp16(ByteWidth);
        Desc.Usage = D3D11_USAGE_DYNAMIC;
        Desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        Desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

        return SUCCEEDED(Device->CreateBuffer(&Desc, nullptr, Buffer.GetAddressOf()));
    };

    return EnsureConstantBuffer(FrameConstantBuffer, sizeof(FForwardPlusFrameConstants)) &&
           EnsureConstantBuffer(ForwardPlusConstantBuffer, sizeof(FForwardPlusConstants)) &&
           EnsureConstantBuffer(LightingConstantBuffer, sizeof(FLightingConstants));
}

void FLightCullingPass::EmitDebugStats(const FRenderPassContext* Context, uint32 TileCountX, uint32 TileCountY)
{
    static uint64 FrameCounter = 0;
    ++FrameCounter;

    if ((FrameCounter % DebugLogIntervalFrames) != 0)
    {
        return;
    }

    if (!Context || !Context->DeviceContext || !TilePointLightGridBuffer || !TilePointLightGridReadbackBuffer ||
        !TileSpotLightGridBuffer || !TileSpotLightGridReadbackBuffer)
    {
        return;
    }

    const uint32 TileCount = TileCountX * TileCountY;
    if (TileCount == 0)
    {
        return;
    }

    Context->DeviceContext->CopyResource(TilePointLightGridReadbackBuffer.Get(), TilePointLightGridBuffer.Get());
    Context->DeviceContext->CopyResource(TileSpotLightGridReadbackBuffer.Get(), TileSpotLightGridBuffer.Get());

    D3D11_MAPPED_SUBRESOURCE MappedPoint = {};
    D3D11_MAPPED_SUBRESOURCE MappedSpot = {};
    if (FAILED(Context->DeviceContext->Map(TilePointLightGridReadbackBuffer.Get(), 0, D3D11_MAP_READ, 0, &MappedPoint)) ||
        FAILED(Context->DeviceContext->Map(TileSpotLightGridReadbackBuffer.Get(), 0, D3D11_MAP_READ, 0, &MappedSpot)))
    {
        if (MappedPoint.pData)
        {
            Context->DeviceContext->Unmap(TilePointLightGridReadbackBuffer.Get(), 0);
        }
        return;
    }

    const FUInt2* PointGrid = static_cast<const FUInt2*>(MappedPoint.pData);
    const FUInt2* SpotGrid = static_cast<const FUInt2*>(MappedSpot.pData);

    uint64 TotalVisibleLights = 0;
    uint32 MaxVisibleLightsInTile = 0;
    uint32 NonZeroTileCount = 0;

    for (uint32 TileIndex = 0; TileIndex < TileCount; ++TileIndex)
    {
        const uint32 VisiblePoint = PointGrid[TileIndex].Y;
        const uint32 VisibleSpot = SpotGrid[TileIndex].Y;
        const uint32 VisibleTotal = VisiblePoint + VisibleSpot;

        TotalVisibleLights += VisibleTotal;
        MaxVisibleLightsInTile = std::max(MaxVisibleLightsInTile, VisibleTotal);
        if (VisibleTotal > 0)
        {
            ++NonZeroTileCount;
        }
    }

    Context->DeviceContext->Unmap(TileSpotLightGridReadbackBuffer.Get(), 0);
    Context->DeviceContext->Unmap(TilePointLightGridReadbackBuffer.Get(), 0);

    GDebugStats.LightCount = GLightCullingOutputs.LightCount;
    GDebugStats.TileCountX = TileCountX;
    GDebugStats.TileCountY = TileCountY;
    GDebugStats.TileCount = TileCount;
    GDebugStats.NonZeroTileCount = NonZeroTileCount;
    GDebugStats.MaxLightsInTile = MaxVisibleLightsInTile;
    GDebugStats.AvgLightsPerTile = (TileCount > 0)
        ? static_cast<float>(TotalVisibleLights) / static_cast<float>(TileCount)
        : 0.0f;
}
