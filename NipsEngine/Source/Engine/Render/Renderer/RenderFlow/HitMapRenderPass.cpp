#include "HitMapRenderPass.h"
#include "Core/Paths.h"
#include "Render/Scene/RenderBus.h"
#include "Render/Resource/RenderResources.h"
#include "Render/Renderer/RenderFlow/LightCullingPass.h"
#include "Editor/Settings/EditorSettings.h"
#include "Core/ResourceManager.h"
#include <d3dcompiler.h>

bool FHitMapRenderPass::Initialize()
{
    return true;
}

bool FHitMapRenderPass::Release()
{
    VertexShader.Reset();
    PixelShader.Reset();
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

    if (!EnsureResources(Context->Device))
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

    ID3D11ShaderResourceView* HitMapSRV = FLightCullingPass::GetOutputs().HitMapSRV;
    if (!HitMapSRV)
        return true;

    ID3D11BlendState* BlendState = FResourceManager::Get().GetOrCreateBlendState(EBlendType::AlphaBlend);
    Context->DeviceContext->OMSetBlendState(BlendState, nullptr, 0xFFFFFFFF);

    Context->DeviceContext->VSSetShader(VertexShader.Get(), nullptr, 0);
    Context->DeviceContext->PSSetShader(PixelShader.Get(), nullptr, 0);

    Context->DeviceContext->PSSetShaderResources(8, 1, &HitMapSRV);

    Context->DeviceContext->Draw(3, 0);

    return true;
}

bool FHitMapRenderPass::End(const FRenderPassContext* Context)
{
    if (bSkipHitMapDraw)
        return true;

    ID3D11ShaderResourceView* nullSRV = nullptr;
    Context->DeviceContext->PSSetShaderResources(8, 1, &nullSRV);
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
