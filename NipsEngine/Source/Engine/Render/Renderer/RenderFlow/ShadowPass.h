#pragma once

#include "Core/Containers/Array.h"
#include "Core/Containers/String.h"
#include "RenderPass.h"
#include "Render/Scene/ShadowLightSelector.h"
#include "Render/Resource/ShadowAtlasAllocator.h"
#include "Render/Renderer/RenderFlow/OpaqueRenderPass.h"

#include <dxgiformat.h>

struct ID3D11ShaderResourceView;
struct D3D11_TEXTURE2D_DESC;
class UWorld;

struct FShadowMapStat
{
	FString Name;
	uint32 Width = 0;
	uint32 Height = 0;
	uint32 ArraySlice = 0;
	uint32 MipLevel = 0;
	int32 CascadeIndex = -1;
	int32 FaceIndex = -1;
	DXGI_FORMAT Format = DXGI_FORMAT_UNKNOWN;
	uint64 MemoryBytes = 0;
	EShadowProjectionMode ProjectionMode = EShadowProjectionMode::Standard;
	EShadowFilterMode FilterMode = EShadowFilterMode::SSM_PCF;
	bool bUsesCSM = false;
	bool bSharedResource = false;
	bool bHasAtlasRegion = false;
	uint32 AtlasX = 0;
	uint32 AtlasY = 0;
};

struct FLightShadowStat
{
	uint32 LightIndex = 0;
	uint32 SourceLightSlotIndex = 0xFFFFFFFF;
	FString LightName;
	ELightType LightType = ELightType::Max;
	bool bCastShadow = false;
	EShadowProjectionMode ProjectionMode = EShadowProjectionMode::Standard;
	EShadowFilterMode FilterMode = EShadowFilterMode::SSM_PCF;
	bool bUsesCSM = false;
	uint32 LogicalShadowMapCount = 0;
	uint32 ResourceViewCount = 0;
	TArray<FShadowMapStat> ShadowMaps;
	uint64 TotalMemoryBytes = 0;
};

struct FShadowStats
{
	TArray<FLightShadowStat> Lights;
	uint64 TotalMemoryBytes = 0;
	uint64 TotalDepthMemoryBytes = 0;
	uint64 TotalVSMMomentMemoryBytes = 0;
	uint64 TotalVSMTempMemoryBytes = 0;
	uint64 TotalAtlasMemoryBytes = 0;
	uint32 ShadowCastingLightCount = 0;
	uint32 TotalShadowMapCount = 0;
	uint32 TotalResourceViewCount = 0;
};

class FShadowPass : public FBaseRenderPass
{
public:
	bool Initialize();
	bool Release();
	static TArray<FShadowMap>& GetShadowMaps();
	static const TArray<int32>& GetLightToShadowIndices();
	static const FOpaqueRenderPass::FShadowArrayCB& GetShadowCBData();
	static ID3D11ShaderResourceView* GetVSM2DShadowSRV();
	static ID3D11ShaderResourceView* GetVSMCubeShadowSRV();
	static FShadowStats GetShadowStats(int32 ViewportIndex, const UWorld* World);
	static uint32 GetBytesPerPixel(DXGI_FORMAT Format);
	static uint64 EstimateTexture2DMemoryBytes(
		const D3D11_TEXTURE2D_DESC& Desc,
		DXGI_FORMAT FormatOverride = DXGI_FORMAT_UNKNOWN);
	static FString FormatBytes(uint64 Bytes);

protected:
	bool Begin(const FRenderPassContext* Context);
	bool DrawCommand(const FRenderPassContext* Context);
	bool End(const FRenderPassContext* Context);

private:
	bool EnsureVSMBindings(const FRenderPassContext* Context);
	bool MakeShadowMap(const FRenderPassContext* Context, const FShadowRequest& Req, FShadowMap& OutShadowMap);
	// Builds the per-slice shadow view/projection data.
	// Cubemap lights emit six views.
	bool BuildViews(const FRenderPassContext* Context, const FShadowRequest& Req, TArray<FShadowViewInfo>& OutViewInfoArray);
	// Builds the logical slice metadata for cascades, cubemap faces, or atlas slots.
	bool BuildSlices(const FRenderPassContext* Context, const FShadowRequest& Req, TArray<FShadowSlice>& OutShadowSlices);
	// Acquires a matching pooled shadow resource.
	bool AcquireResource(const FRenderPassContext* Context, const FShadowRequestDesc& Req, FShadowResource** OutShadowResource);

private:
	FShadowLightSelector ShadowLightSelector;
	bool bSkip = false;
	FShadowAtlasAllocator AtlasAllocator;
	std::shared_ptr<FShaderBindingInstance> ShaderBinding;
	std::shared_ptr<FShaderBindingInstance> VSMConvertShaderBinding;
	std::shared_ptr<FShaderBindingInstance> VSMBlurShaderBinding;
	std::shared_ptr<FShaderBindingInstance> PointVSMShaderBinding;
};
