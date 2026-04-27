#include "HitMapRenderPass.h"
#include "Core/Paths.h"
#include "Core/ResourceManager.h"
#include "Render/Scene/RenderBus.h"
#include "Render/Renderer/RenderFlow/LightCullingPass.h"

struct FForwardPlusConstants
{
    uint32 ScreenSize[2];
    uint32 TileCount[2];
    uint32 bEnable25DMask;
    float Padding[3];
};

bool FHitMapRenderPass::Initialize()
{
    return true;
}

bool FHitMapRenderPass::Release()
{
    HitMapShader = nullptr;
    return true;
}

bool FHitMapRenderPass::Begin(const FRenderPassContext* Context)
{
    if (!Context || !Context->Device || !Context->DeviceContext || !Context->RenderTargets || !Context->RenderBus)
    {
        return false;
    }

    if (!Context->RenderBus->GetShowFlags().bShowLightHitmapOverlay)
    {
        return false;
    }

    if (!EnsureShader(Context->Device))
    {
        return false;
    }

    // We draw onto the scene color
    ID3D11RenderTargetView* RTV = Context->RenderTargets->SceneColorRTV;
    Context->DeviceContext->OMSetRenderTargets(1, &RTV, nullptr);
    Context->DeviceContext->OMSetDepthStencilState(nullptr, 0);

    ID3D11BlendState* AlphaBlend = FResourceManager::Get().GetOrCreateBlendState(EBlendType::AlphaBlend);
    Context->DeviceContext->OMSetBlendState(AlphaBlend, nullptr, 0xFFFFFFFF);

    ID3D11RasterizerState* RS = FResourceManager::Get().GetOrCreateRasterizerState(ERasterizerType::SolidNoCull);
    Context->DeviceContext->RSSetState(RS);

    Context->DeviceContext->IASetInputLayout(nullptr);
    Context->DeviceContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    OutSRV = PrevPassSRV;
    OutRTV = PrevPassRTV;
    return true;
}

bool FHitMapRenderPass::DrawCommand(const FRenderPassContext* Context)
{
    const FLightCullingOutputs& CullingOutputs = FLightCullingPass::GetOutputs();
    if (CullingOutputs.TileCountX == 0 || CullingOutputs.TileCountY == 0)
    {
        return true;
    }

    ID3D11DeviceContext* DeviceContext = Context->DeviceContext;

    // Set constant buffer
    FForwardPlusConstants FPConstants = {};
    FPConstants.ScreenSize[0] = static_cast<uint32>(Context->RenderTargets->Width);
    FPConstants.ScreenSize[1] = static_cast<uint32>(Context->RenderTargets->Height);
    FPConstants.TileCount[0] = CullingOutputs.TileCountX;
    FPConstants.TileCount[1] = CullingOutputs.TileCountY;
    FPConstants.bEnable25DMask = 1;

    // Use a temporary constant buffer or one from RenderResources if available.
    // For now, let's use the one from RenderResources by updating it again (it was used in LightCullingPass)
    // Actually, we don't have easy access to it here. Let's just create a local one or use a static one.
    static TComPtr<ID3D11Buffer> HitMapCB;
    if (!HitMapCB)
    {
        D3D11_BUFFER_DESC Desc = {};
        Desc.ByteWidth = sizeof(FForwardPlusConstants);
        Desc.Usage = D3D11_USAGE_DYNAMIC;
        Desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        Desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        Context->Device->CreateBuffer(&Desc, nullptr, &HitMapCB);
    }

    D3D11_MAPPED_SUBRESOURCE Mapped = {};
    if (SUCCEEDED(DeviceContext->Map(HitMapCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &Mapped)))
    {
        std::memcpy(Mapped.pData, &FPConstants, sizeof(FPConstants));
        DeviceContext->Unmap(HitMapCB.Get(), 0);
    }

    ID3D11Buffer* CB = HitMapCB.Get();
    DeviceContext->PSSetConstantBuffers(11, 1, &CB);

    // Set SRVs
    ID3D11ShaderResourceView* SRVs[14] = {}; // We need t10 and t12
    SRVs[10] = CullingOutputs.TilePointLightGridSRV;
    SRVs[12] = CullingOutputs.TileSpotLightGridSRV;
    DeviceContext->PSSetShaderResources(0, 14, SRVs);

    HitMapShader->Bind(DeviceContext);
    DeviceContext->Draw(3, 0);

    return true;
}

bool FHitMapRenderPass::End(const FRenderPassContext* Context)
{
    ID3D11ShaderResourceView* NullSRVs[14] = {};
    Context->DeviceContext->PSSetShaderResources(0, 14, NullSRVs);
    return true;
}

bool FHitMapRenderPass::EnsureShader(ID3D11Device* Device)
{
    if (HitMapShader)
    {
        return true;
    }

    HitMapShader = FResourceManager::Get().GetShader("Shaders/HitMap.hlsl");
    if (!HitMapShader)
    {
        // If not loaded by manager yet, it might need manual load or wait for manager.
        // But manager usually loads all Shaders/*.hlsl if configured.
        return false;
    }

    return true;
}
