#include "HitMapRenderPass.h"
#include "Core/Paths.h"
#include "Render/Scene/RenderBus.h"
#include "Render/Resource/RenderResources.h"
#include "Render/Renderer/RenderFlow/LightCullingPass.h"
#include "Editor/Settings/EditorSettings.h"
#include "Core/ResourceManager.h"
#include <d3dcompiler.h>
#include <cstring>

namespace
{
    struct FHitMapConstants
    {
        uint32 TileCountX = 0;
        uint32 TileCountY = 0;
        uint32 TileSize = 0;
        uint32 MaxPointLightsPerTile = 0;
        uint32 MaxSpotLightsPerTile = 0;
        uint32 VisiblePointLightCount = 0;
        uint32 VisibleSpotLightCount = 0;
        uint32 Padding0 = 0;
    };
}

bool FHitMapRenderPass::Initialize()
{
    return true;
}

bool FHitMapRenderPass::Release()
{
    VertexShader.Reset();
    PixelShader.Reset();
    HitMapConstantBuffer.Reset();
    return true;
}

bool FHitMapRenderPass::Begin(const FRenderPassContext* Context)
{
    OutSRV = PrevPassSRV;
    OutRTV = PrevPassRTV;
    bSkipHitMapDraw = true;

    if (!FEditorSettings::Get().bShowLightHitMap)
    {
        return true;
    }

    if (!PrevPassRTV)
    {
        return true;
    }

    if (!EnsureResources(Context->Device) || !EnsureConstantBuffer(Context->Device))
    {
        return true;
    }

    bSkipHitMapDraw = false;

    ID3D11RenderTargetView* RTV = PrevPassRTV;
    Context->DeviceContext->OMSetRenderTargets(1, &RTV, nullptr);

    Context->DeviceContext->IASetInputLayout(nullptr);
    Context->DeviceContext->IASetVertexBuffers(0, 0, nullptr, nullptr, nullptr);
    Context->DeviceContext->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);
    Context->DeviceContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    return true;
}

bool FHitMapRenderPass::DrawCommand(const FRenderPassContext* Context)
{
    if (bSkipHitMapDraw)
        return true;

    const FLightCullingOutputs& Outputs = FLightCullingPass::GetOutputs();
    if (!Outputs.TilePointLightGridSRV || !Outputs.TileSpotLightGridSRV || !HitMapConstantBuffer)
        return true;

    FHitMapConstants Constants = {};
    Constants.TileCountX = Outputs.TileCountX;
    Constants.TileCountY = Outputs.TileCountY;
    Constants.TileSize = Outputs.TileSize;
    Constants.MaxPointLightsPerTile = Outputs.MaxPointLightsPerTile;
    Constants.MaxSpotLightsPerTile = Outputs.MaxSpotLightsPerTile;
    Constants.VisiblePointLightCount = Outputs.PointLightCount;
    Constants.VisibleSpotLightCount = Outputs.SpotLightCount;

    D3D11_MAPPED_SUBRESOURCE Mapped = {};
    if (FAILED(Context->DeviceContext->Map(HitMapConstantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &Mapped)))
    {
        return true;
    }
    std::memcpy(Mapped.pData, &Constants, sizeof(Constants));
    Context->DeviceContext->Unmap(HitMapConstantBuffer.Get(), 0);

    ID3D11BlendState* BlendState = FResourceManager::Get().GetOrCreateBlendState(EBlendType::AlphaBlend);
    Context->DeviceContext->OMSetBlendState(BlendState, nullptr, 0xFFFFFFFF);

    Context->DeviceContext->VSSetShader(VertexShader.Get(), nullptr, 0);
    Context->DeviceContext->PSSetShader(PixelShader.Get(), nullptr, 0);

    ID3D11ShaderResourceView* SRVs[6] =
    {
        Outputs.PointLightBufferSRV,
        Outputs.SpotLightBufferSRV,
        Outputs.TilePointLightGridSRV,
        Outputs.TilePointLightIndexSRV,
        Outputs.TileSpotLightGridSRV,
        Outputs.TileSpotLightIndexSRV
    };
    Context->DeviceContext->PSSetShaderResources(8, 6, SRVs);
    ID3D11Buffer* ConstantBuffer = HitMapConstantBuffer.Get();
    Context->DeviceContext->PSSetConstantBuffers(4, 1, &ConstantBuffer);

    Context->DeviceContext->Draw(3, 0);

    ID3D11BlendState* OpaqueBlendState = FResourceManager::Get().GetOrCreateBlendState(EBlendType::Opaque);
    Context->DeviceContext->OMSetBlendState(OpaqueBlendState, nullptr, 0xFFFFFFFF);

    return true;
}

bool FHitMapRenderPass::End(const FRenderPassContext* Context)
{
    if (bSkipHitMapDraw)
        return true;

    ID3D11ShaderResourceView* NullSRVs[6] = { nullptr, nullptr, nullptr, nullptr, nullptr, nullptr };
    Context->DeviceContext->PSSetShaderResources(8, 6, NullSRVs);
    ID3D11Buffer* NullCB = nullptr;
    Context->DeviceContext->PSSetConstantBuffers(4, 1, &NullCB);
    return true;
}

bool FHitMapRenderPass::EnsureResources(ID3D11Device* Device)
{
    if (VertexShader && PixelShader)
        return true;

    const std::wstring ShaderPathAbsolute = FPaths::ToAbsolute(FPaths::ToWide("Shaders/Multipass/HitMapPass.hlsl"));
    const std::wstring ShaderPathRelative = FPaths::ToWide("Shaders/Multipass/HitMapPass.hlsl");

    TComPtr<ID3DBlob> VSBlob;
    TComPtr<ID3DBlob> PSBlob;
    TComPtr<ID3DBlob> ErrorBlob;

    auto CompileShader = [&](const std::wstring& Path, const char* EntryPoint, const char* Target, TComPtr<ID3DBlob>& OutBlob) -> bool
    {
        ErrorBlob.Reset();
        const HRESULT Result = D3DCompileFromFile(
            Path.c_str(),
            nullptr,
            D3D_COMPILE_STANDARD_FILE_INCLUDE,
            EntryPoint,
            Target,
            0,
            0,
            OutBlob.GetAddressOf(),
            ErrorBlob.GetAddressOf());
        return SUCCEEDED(Result);
    };

    if (!CompileShader(ShaderPathAbsolute, "VSMain", "vs_5_0", VSBlob) &&
        !CompileShader(ShaderPathRelative, "VSMain", "vs_5_0", VSBlob))
    {
        return false;
    }

    if (!CompileShader(ShaderPathAbsolute, "PSMain", "ps_5_0", PSBlob) &&
        !CompileShader(ShaderPathRelative, "PSMain", "ps_5_0", PSBlob))
    {
        return false;
    }

    if (FAILED(Device->CreateVertexShader(VSBlob->GetBufferPointer(), VSBlob->GetBufferSize(), nullptr, &VertexShader)))
    {
        return false;
    }
    if (FAILED(Device->CreatePixelShader(PSBlob->GetBufferPointer(), PSBlob->GetBufferSize(), nullptr, &PixelShader)))
    {
        VertexShader.Reset();
        return false;
    }

    return true;
}

bool FHitMapRenderPass::EnsureConstantBuffer(ID3D11Device* Device)
{
    if (HitMapConstantBuffer)
    {
        return true;
    }

    D3D11_BUFFER_DESC Desc = {};
    Desc.ByteWidth = sizeof(FHitMapConstants);
    Desc.Usage = D3D11_USAGE_DYNAMIC;
    Desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    Desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    return SUCCEEDED(Device->CreateBuffer(&Desc, nullptr, HitMapConstantBuffer.GetAddressOf()));
}
