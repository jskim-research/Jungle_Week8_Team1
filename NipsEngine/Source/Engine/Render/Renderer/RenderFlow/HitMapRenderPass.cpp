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
    if (!FEditorSettings::Get().bShowLightHitMap)
    {
        OutSRV = PrevPassSRV;
        OutRTV = PrevPassRTV;
        return true;
    }

    if (!EnsureResources(Context->Device))
        return false;

    ID3D11RenderTargetView* RTV = PrevPassRTV;
    Context->DeviceContext->OMSetRenderTargets(1, &RTV, nullptr);

    Context->DeviceContext->IASetInputLayout(nullptr);
    Context->DeviceContext->IASetVertexBuffers(0, 0, nullptr, nullptr, nullptr);
    Context->DeviceContext->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);
    Context->DeviceContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    OutSRV = PrevPassSRV;
    OutRTV = PrevPassRTV;
    return true;
}

bool FHitMapRenderPass::DrawCommand(const FRenderPassContext* Context)
{
    if (!FEditorSettings::Get().bShowLightHitMap)
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
    if (!FEditorSettings::Get().bShowLightHitMap)
        return true;

    ID3D11ShaderResourceView* nullSRV = nullptr;
    Context->DeviceContext->PSSetShaderResources(8, 1, &nullSRV);
    return true;
}

bool FHitMapRenderPass::EnsureResources(ID3D11Device* Device)
{
    if (VertexShader && PixelShader)
        return true;

    TComPtr<ID3DBlob> VSBlob;
    TComPtr<ID3DBlob> PSBlob;
    TComPtr<ID3DBlob> ErrorBlob;

    HRESULT hr = D3DCompileFromFile(
        FPaths::ToWide("Shaders/Multipass/HitMapPass.hlsl").c_str(),
        nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
        "VSMain", "vs_5_0", 0, 0, &VSBlob, &ErrorBlob);
    
    if (FAILED(hr)) return false;

    hr = D3DCompileFromFile(
        FPaths::ToWide("Shaders/Multipass/HitMapPass.hlsl").c_str(),
        nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
        "PSMain", "ps_5_0", 0, 0, &PSBlob, &ErrorBlob);

    if (FAILED(hr)) return false;

    Device->CreateVertexShader(VSBlob->GetBufferPointer(), VSBlob->GetBufferSize(), nullptr, &VertexShader);
    Device->CreatePixelShader(PSBlob->GetBufferPointer(), PSBlob->GetBufferSize(), nullptr, &PixelShader);

    return true;
}