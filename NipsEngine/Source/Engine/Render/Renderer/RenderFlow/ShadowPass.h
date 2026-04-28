#pragma once

#include "RenderPass.h"
#include "Render/Scene/ShadowLightSelector.h"
#include "Render/Resource/ShadowAtlasAllocator.h"
#include "Render/Renderer/RenderFlow/OpaqueRenderPass.h"
class FShadowPass : public FBaseRenderPass
{
public:
    bool Initialize();
    bool Release();
    static TArray<FShadowMap>& GetShadowMaps();
    static const TArray<int32>& GetLightToShadowIndices();
    static const FOpaqueRenderPass::FShadowArrayCB& GetShadowCBData();

protected:
	bool Begin(const FRenderPassContext* Context);
	bool DrawCommand(const FRenderPassContext* Context);
	bool End(const FRenderPassContext* Context);

private:
	bool MakeShadowMap(const FRenderPassContext* Context, const FShadowRequest& Req, FShadowMap& OutShadowMap);
	// View 정보 (Light View, Projection, Split Depth) 반환
	// 예를 들어 Cubemap 인 경우 6개
	bool BuildViews(const FRenderPassContext* Context, const FShadowRequest& Req, TArray<FShadowViewInfo>& OutViewInfoArray);
	// Cascade, Cubemap Index, Atlas Slot 등 Map 별 정보 반환
	bool BuildSlices(const FRenderPassContext* Context, const FShadowRequest& Req, TArray<FShadowSlice>& OutShadowSlices);
	// Desc에 맞는 적절한 Resource 반환
	bool AcquireResource(const FRenderPassContext* Context, const FShadowRequestDesc& Req, FShadowResource** OutShadowResource);

private:
    FShadowLightSelector ShadowLightSelector;
    bool bSkip = false;
    FShadowAtlasAllocator AtlasAllocator;
    std::shared_ptr<FShaderBindingInstance> ShaderBinding;
};
