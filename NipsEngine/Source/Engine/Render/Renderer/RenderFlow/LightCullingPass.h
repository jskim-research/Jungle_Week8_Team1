#pragma once
#include "RenderPass.h"
#include "Render/Common/ComPtr.h"

struct FLightCullingOutputs
{
    ID3D11ShaderResourceView* PointLightBufferSRV = nullptr;
    ID3D11ShaderResourceView* SpotLightBufferSRV = nullptr;
    ID3D11ShaderResourceView* TilePointLightGridSRV = nullptr;
    ID3D11ShaderResourceView* TilePointLightIndexSRV = nullptr;
    ID3D11ShaderResourceView* TileSpotLightGridSRV = nullptr;
    ID3D11ShaderResourceView* TileSpotLightIndexSRV = nullptr;
    uint32 TileCountX = 0;
    uint32 TileCountY = 0;
    uint32 TileSize = 0;
    uint32 MaxPointLightsPerTile = 0;
    uint32 MaxSpotLightsPerTile = 0;
    uint32 PointLightCount = 0;
    uint32 SpotLightCount = 0;
    uint32 MaxLightsPerTile = 0;
    uint32 LightCount = 0;
};

struct FLightCullingDebugStats
{
    uint32 LightCount        = 0;
    uint32 TileCountX        = 0;
    uint32 TileCountY        = 0;
    uint32 TileCount         = 0;
    uint32 NonZeroTileCount  = 0;
    uint32 MaxLightsInTile   = 0;
    float  AvgLightsPerTile  = 0.0f;
};

class FLightCullingPass : public FBaseRenderPass
{
public:
    bool Initialize() override;
    bool Release() override;
    static const FLightCullingOutputs& GetOutputs();
    static const FLightCullingDebugStats& GetDebugStats();

private:
    bool Begin(const FRenderPassContext* Context) override;
    bool DrawCommand(const FRenderPassContext* Context) override;
    bool End(const FRenderPassContext* Context) override;

    bool EnsureComputeShader(ID3D11Device* Device);
    bool EnsureInputLightBuffers(ID3D11Device* Device, uint32 RequiredPointLightCount, uint32 RequiredSpotLightCount);
    bool EnsureTileBuffers(ID3D11Device* Device, uint32 RequiredTileCount);
    bool EnsureConstantBuffers(ID3D11Device* Device);
    void EmitDebugStats(const FRenderPassContext* Context, uint32 TileCountX, uint32 TileCountY);


private:
    TComPtr<ID3D11ComputeShader> ComputeShader;
    TComPtr<ID3D11Buffer> PointLightBuffer;
    TComPtr<ID3D11ShaderResourceView> PointLightBufferSRV;
    TComPtr<ID3D11Buffer> SpotLightBuffer;
    TComPtr<ID3D11ShaderResourceView> SpotLightBufferSRV;

    TComPtr<ID3D11Buffer> TilePointLightGridBuffer;
    TComPtr<ID3D11Buffer> TilePointLightGridReadbackBuffer;
    TComPtr<ID3D11UnorderedAccessView> TilePointLightGridUAV;
    TComPtr<ID3D11ShaderResourceView> TilePointLightGridSRV;

    TComPtr<ID3D11Buffer> TilePointLightIndexBuffer;
    TComPtr<ID3D11UnorderedAccessView> TilePointLightIndexUAV;
    TComPtr<ID3D11ShaderResourceView> TilePointLightIndexSRV;

    TComPtr<ID3D11Buffer> TileSpotLightGridBuffer;
    TComPtr<ID3D11Buffer> TileSpotLightGridReadbackBuffer;
    TComPtr<ID3D11UnorderedAccessView> TileSpotLightGridUAV;
    TComPtr<ID3D11ShaderResourceView> TileSpotLightGridSRV;

    TComPtr<ID3D11Buffer> TileSpotLightIndexBuffer;
    TComPtr<ID3D11UnorderedAccessView> TileSpotLightIndexUAV;
    TComPtr<ID3D11ShaderResourceView> TileSpotLightIndexSRV;

    TComPtr<ID3D11Buffer> FrameConstantBuffer;
    TComPtr<ID3D11Buffer> ForwardPlusConstantBuffer;
    TComPtr<ID3D11Buffer> LightingConstantBuffer;

    uint32 PointLightBufferCapacity = 0;
    uint32 SpotLightBufferCapacity = 0;
    uint32 TileBufferCapacity = 0;
};
