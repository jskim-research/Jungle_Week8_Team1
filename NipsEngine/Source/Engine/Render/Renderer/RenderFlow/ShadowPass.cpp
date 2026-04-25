#include "ShadowPass.h"
#include "Render/Scene/ShadowLightSelector.h"

bool FShadowPass::Initialize()
{
	return true;
}

bool FShadowPass::Release()
{
	return true;
}

bool FShadowPass::Begin(const FRenderPassContext* Context)
{
	if (!ShadowMaps.empty())
	{
		for (FShadowMap& ShadowMap : ShadowMaps)
		{
            Context->ShadowResourcePool->Release(ShadowMap.Resource);
		}
        ShadowMaps.clear();
	}
    bSkip = false;
	std::vector<FShadowRequest> ShadowRequests = ShadowLightSelector.SelectShadowLights(Context->RenderBus->GetLights());

	if (ShadowRequests.empty())
    {
        bSkip = true;
        return true;
	}

	// 테스트 용도로 첫 번째만 사용
    FCascadeData CascadeData;
    FShadowSlice ShadowSlice;

	FShadowRequestDesc Desc;

	// 테스트 용도로 Directional Light 만 제대로 해놨음
	switch (ShadowRequests[0].Type)
	{
    case ELightType::LightType_Directional:
        Desc.AllocationMode = EShadowAllocationMode::ArrayBased;  // CSM
        Desc.MapType = EShadowMapType::Depth2D;
        Desc.Resolution = ShadowRequests[0].Resolution;
        Desc.CascadeCount = 1; // 일단은 한 장씩만 사용
		
		// 테스트 용도로 Orthographic 가정
        CascadeData.LightViewProj = FMatrix::MakeViewLookAtLH(Context->RenderBus->GetLights()[ShadowRequests[0].LightId].Position, Context->RenderBus->GetLights()[ShadowRequests[0].LightId].Direction);
        CascadeData.SplitDepth = 1000;
		
		ShadowSlice.Index = 0;
        ShadowSlice.Type = EShadowSliceType::CSM;
        ShadowSlice.UVOffset = FVector2(0, 0);
        ShadowSlice.UVScale = FVector2(1, 1);		
		break;

	default:
		// Ambient 등은 따로 처리 안함
        bSkip = true;
        return true;
	}

	FShadowMap ShadowMap;

	ShadowMap.Resource = Context->ShadowResourcePool->Acquire(Context->Device, Desc);
    ShadowMap.MapType = Desc.MapType;
	// 테스트 용도 -> Cascade Count = 1
	ShadowMap.Cascades.push_back(CascadeData);
    ShadowMap.Slices.push_back(ShadowSlice);

	ShadowMaps.push_back(ShadowMap);

	OutSRV = ShadowMap.Resource->SRV;
    OutRTV = nullptr;

	return true;
}

bool FShadowPass::DrawCommand(const FRenderPassContext* Context)
{

    Context->DeviceContext->OMSetRenderTargets(1, nullptr, ShadowMaps[0].Resource->DSVs[0]);


    if (bSkip)
        return true;
	return true;
}

bool FShadowPass::End(const FRenderPassContext* Context)
{
    if (bSkip)
        return true;
	return true;
}