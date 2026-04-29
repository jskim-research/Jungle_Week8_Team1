#include "ShadowPass.h"
#include "Core/Logging/GPUProfiler.h"
#include "Core/Logging/Stats.h"
#include "Render/Scene/ShadowLightSelector.h"
#include "Core/ResourceManager.h"
#include "Editor/UI/EditorConsoleWidget.h"
#include "Render/Common/PSMCalculator.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

static constexpr uint32 kAtlasSize = 4096;
static constexpr float kDefaultPsmSliderBack = 10.0f;

namespace
{
	TArray<FShadowMap> GShadowMaps;

	struct FShadowVSMResource
	{
		TComPtr<ID3D11Texture2D> MomentsTexture;
		TComPtr<ID3D11ShaderResourceView> MomentsSRV;
		TComPtr<ID3D11ShaderResourceView> MomentsCubeSRV;
		TArray<TComPtr<ID3D11RenderTargetView>> MomentRTVOwners;
		TArray<ID3D11RenderTargetView*> MomentRTVs;

		TComPtr<ID3D11Texture2D> TempTexture;
		TComPtr<ID3D11ShaderResourceView> TempSRV;
		TArray<TComPtr<ID3D11RenderTargetView>> TempRTVOwners;
		TArray<ID3D11RenderTargetView*> TempRTVs;
	};

	struct FShadowVSMResourceDesc
	{
		uint32 Resolution = 0;
		uint32 SliceCount = 0;
		bool bCreateCubeSRV = false;
	};

	struct FPooledShadowVSMResource
	{
		FShadowVSMResourceDesc Desc = {};
		FShadowVSMResource Resource;
		bool bInUse = false;
	};

	TArray<FPooledShadowVSMResource> GVSMResourcePool;
	TArray<FShadowVSMResource*> GVSMResources;
	TArray<int32> GLightToShadowIndices;
	FOpaqueRenderPass::FShadowArrayCB GShadowCBData;

	constexpr DXGI_FORMAT GVSMMomentsFormat = DXGI_FORMAT_R32G32_FLOAT;

	bool MatchesVSMResourceDesc(const FShadowVSMResourceDesc& Lhs, const FShadowVSMResourceDesc& Rhs)
	{
		return Lhs.Resolution == Rhs.Resolution &&
			   Lhs.SliceCount == Rhs.SliceCount &&
			   Lhs.bCreateCubeSRV == Rhs.bCreateCubeSRV;
	}

	bool SupportsPSMProjection(ELightType LightType)
	{
		return LightType == ELightType::LightType_Directional ||
			   LightType == ELightType::LightType_Spot;
	}

	float ComputeShadowCompareBias(const FRenderLight& Light)
	{
		const float UserBias = std::clamp(Light.ShadowBias, 0.0f, 1.0f);
		return 0.005f * std::max(UserBias * 2.0f, 0.1f);
	}

	float ComputeShadowFilterScale(const FRenderLight& Light)
	{
		const float UserSharpen = std::clamp(Light.ShadowSharpen, 0.0f, 1.0f);
		return std::max(0.25f, 1.0f - (UserSharpen * 0.75f));
	}

	EShadowProjectionMode ResolveShadowProjectionMode(const FShowFlags& ShowFlags, const FRenderLight& Light)
	{
		const ELightType LightType = static_cast<ELightType>(Light.Type);
		if (!ShowFlags.UsesPSMShadowProjection() || !Light.bPSM || !SupportsPSMProjection(LightType))
		{
			return EShadowProjectionMode::Standard;
		}

		return EShadowProjectionMode::PSM;
	}

	FMatrix ComputePSMMatrix(const FRenderPassContext* Context, const FRenderLight& Light)
	{
		FRAME_SPIKE_SCOPE("PSM shadow matrix/build step");

		if (Context == nullptr || Context->RenderBus == nullptr)
		{
			return FMatrix::Identity;
		}

		FCamera Camera = {};
		Camera.Forward = Context->RenderBus->GetCameraForward();
		Camera.Up = Context->RenderBus->GetCameraUp();
		Camera.Right = Context->RenderBus->GetCameraRight();
		Camera.Position = Context->RenderBus->GetCameraPosition();
		Camera.CameraState = Context->RenderBus->GetCameraState();

		PSM::GetCameraFitNearZ(Context->RenderBus->GetCommands(ERenderPass::Opaque), Camera);

		const float SliderBack = (Light.CameraSliderBack > 0.0f) ? Light.CameraSliderBack : kDefaultPsmSliderBack;
		FMatrix VirtualCameraView;
		FMatrix VirtualCameraProjection;
		PSM::GenerateVirtualCameraViewProjection(SliderBack, Camera, VirtualCameraProjection, VirtualCameraView);

		const bool bDirectional = Light.Type == static_cast<uint32>(ELightType::LightType_Directional);
		const FVector LightDir = bDirectional
			? -Light.Direction.GetSafeNormal()
			: Light.Direction.GetSafeNormal();

		FMatrix PostPerspectiveView;
		FMatrix PostPerspectiveProjection;
		PSM::GeneratePostPerspectiveViewProjection(
			LightDir,
			PostPerspectiveProjection,
			PostPerspectiveView,
			VirtualCameraView,
			VirtualCameraProjection);

		return VirtualCameraView * VirtualCameraProjection * PostPerspectiveView * PostPerspectiveProjection;
	}

	void ApplyShadowProjectionMode(
		FOpaqueRenderPass::FShadowCB& ShadowData,
		const FRenderPassContext* Context,
		const FRenderLight& Light,
		EShadowProjectionMode ProjectionMode)
	{
		const bool bUsePSM = ProjectionMode == EShadowProjectionMode::PSM;
		ShadowData.isPSM = bUsePSM ? 1u : 0u;
		ShadowData.PSM = bUsePSM ? ComputePSMMatrix(Context, Light) : FMatrix::Identity;
	}

	float ComputeVSMDepthBias(const FRenderLight& Light, EShadowMapType MapType)
	{
		const float UserBias = std::clamp(Light.ShadowBias, 0.0f, 1.0f);
		if (MapType == EShadowMapType::VSMCube)
		{
			return std::max(0.02f, std::max(Light.Radius, 1.0f) * 1.0e-4f * std::max(UserBias * 2.0f, 0.1f));
		}

		return 5.0e-4f * std::max(UserBias * 2.0f, 0.1f);
	}

	float ComputeVSMMinVariance(const FRenderLight& Light, EShadowMapType MapType)
	{
		if (MapType == EShadowMapType::VSMCube)
		{
			const float ShadowFar = std::max(Light.Radius, 1.0f);
			return std::max(ShadowFar * ShadowFar * 1.0e-6f, 1.0e-4f);
		}

		return 2.0e-5f;
	}

	float ComputeVSMLightBleedingReduction(EShadowMapType MapType)
	{
		return (MapType == EShadowMapType::VSMCube) ? 0.35f : 0.2f;
	}

	void ApplyVSMParameters(FOpaqueRenderPass::FShadowCB& ShadowData, const FRenderLight& Light, EShadowMapType MapType)
	{
		ShadowData.VSMDepthBias = ComputeVSMDepthBias(Light, MapType);
		ShadowData.VSMMinVariance = ComputeVSMMinVariance(Light, MapType);
		ShadowData.VSMLightBleedingReduction = ComputeVSMLightBleedingReduction(MapType);
	}

	EShadowFilterMode ResolveShadowFilterMode(EShadowFilterMode RequestedMode, EShadowMapType MapType)
	{
		if (MapType == EShadowMapType::VSM2D || MapType == EShadowMapType::VSMCube)
		{
			return EShadowFilterMode::VSM;
		}

		return (RequestedMode == EShadowFilterMode::SSM)
			? EShadowFilterMode::SSM
			: EShadowFilterMode::SSM_PCF;
	}

	void ApplyShadowFilterMode(FOpaqueRenderPass::FShadowCB& ShadowData, EShadowFilterMode RequestedMode, EShadowMapType MapType)
	{
		ShadowData.ShadowFilterMode = static_cast<uint32>(ResolveShadowFilterMode(RequestedMode, MapType));
	}

	bool CreateVSMTextureResource(
		ID3D11Device* Device,
		uint32 Resolution,
		uint32 SliceCount,
		bool bCreateCubeSRV,
		TComPtr<ID3D11Texture2D>& OutTexture,
		TComPtr<ID3D11ShaderResourceView>& OutSRV,
		TComPtr<ID3D11ShaderResourceView>& OutCubeSRV,
		TArray<TComPtr<ID3D11RenderTargetView>>& OutRTVOwners,
		TArray<ID3D11RenderTargetView*>& OutRTVs)
	{
		if (Device == nullptr || Resolution == 0 || SliceCount == 0 || (bCreateCubeSRV && (SliceCount % 6u) != 0u))
		{
			return false;
		}

		OutTexture.Reset();
		OutSRV.Reset();
		OutCubeSRV.Reset();
		OutRTVOwners.clear();
		OutRTVs.clear();

		D3D11_TEXTURE2D_DESC TextureDesc = {};
		TextureDesc.Width = Resolution;
		TextureDesc.Height = Resolution;
		TextureDesc.MipLevels = 1;
		TextureDesc.ArraySize = SliceCount;
		TextureDesc.Format = GVSMMomentsFormat;
		TextureDesc.SampleDesc.Count = 1;
		TextureDesc.Usage = D3D11_USAGE_DEFAULT;
		TextureDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
		TextureDesc.MiscFlags = bCreateCubeSRV ? D3D11_RESOURCE_MISC_TEXTURECUBE : 0u;

		if (FAILED(Device->CreateTexture2D(&TextureDesc, nullptr, OutTexture.GetAddressOf())))
		{
			return false;
		}

		D3D11_SHADER_RESOURCE_VIEW_DESC SRVDesc = {};
		SRVDesc.Format = GVSMMomentsFormat;
		SRVDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
		SRVDesc.Texture2DArray.MostDetailedMip = 0;
		SRVDesc.Texture2DArray.MipLevels = 1;
		SRVDesc.Texture2DArray.FirstArraySlice = 0;
		SRVDesc.Texture2DArray.ArraySize = SliceCount;
		if (FAILED(Device->CreateShaderResourceView(OutTexture.Get(), &SRVDesc, OutSRV.GetAddressOf())))
		{
			return false;
		}

		if (bCreateCubeSRV)
		{
			D3D11_SHADER_RESOURCE_VIEW_DESC CubeSRVDesc = {};
			CubeSRVDesc.Format = GVSMMomentsFormat;
			CubeSRVDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBEARRAY;
			CubeSRVDesc.TextureCubeArray.MostDetailedMip = 0;
			CubeSRVDesc.TextureCubeArray.MipLevels = 1;
			CubeSRVDesc.TextureCubeArray.First2DArrayFace = 0;
			CubeSRVDesc.TextureCubeArray.NumCubes = SliceCount / 6u;
			if (FAILED(Device->CreateShaderResourceView(OutTexture.Get(), &CubeSRVDesc, OutCubeSRV.GetAddressOf())))
			{
				return false;
			}
		}

		OutRTVOwners.resize(SliceCount);
		OutRTVs.resize(SliceCount, nullptr);

		for (uint32 SliceIndex = 0; SliceIndex < SliceCount; ++SliceIndex)
		{
			D3D11_RENDER_TARGET_VIEW_DESC RTVDesc = {};
			RTVDesc.Format = GVSMMomentsFormat;
			RTVDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
			RTVDesc.Texture2DArray.MipSlice = 0;
			RTVDesc.Texture2DArray.FirstArraySlice = SliceIndex;
			RTVDesc.Texture2DArray.ArraySize = 1;

			if (FAILED(Device->CreateRenderTargetView(OutTexture.Get(), &RTVDesc, OutRTVOwners[SliceIndex].GetAddressOf())))
			{
				return false;
			}

			OutRTVs[SliceIndex] = OutRTVOwners[SliceIndex].Get();
		}

		return true;
	}

	bool CreateVSMResources(ID3D11Device* Device, uint32 Resolution, uint32 SliceCount, FShadowVSMResource& OutResource)
	{
		TComPtr<ID3D11ShaderResourceView> UnusedCubeSRV;
		return CreateVSMTextureResource(
				   Device,
				   Resolution,
				   SliceCount,
				   false,
				   OutResource.MomentsTexture,
				   OutResource.MomentsSRV,
				   OutResource.MomentsCubeSRV,
				   OutResource.MomentRTVOwners,
				   OutResource.MomentRTVs) &&
			   CreateVSMTextureResource(
				   Device,
				   Resolution,
				   SliceCount,
				   false,
				   OutResource.TempTexture,
				   OutResource.TempSRV,
				   UnusedCubeSRV,
				   OutResource.TempRTVOwners,
				   OutResource.TempRTVs);
	}

	bool CreateVSMCubeResources(ID3D11Device* Device, uint32 Resolution, uint32 CubeCount, FShadowVSMResource& OutResource)
	{
		const uint32 SliceCount = CubeCount * 6u;
		TComPtr<ID3D11ShaderResourceView> UnusedCubeSRV;
		return CreateVSMTextureResource(
				   Device,
				   Resolution,
				   SliceCount,
				   true,
				   OutResource.MomentsTexture,
				   OutResource.MomentsSRV,
				   OutResource.MomentsCubeSRV,
				   OutResource.MomentRTVOwners,
				   OutResource.MomentRTVs) &&
			   CreateVSMTextureResource(
				   Device,
				   Resolution,
				   SliceCount,
				   false,
				   OutResource.TempTexture,
				   OutResource.TempSRV,
				   UnusedCubeSRV,
				   OutResource.TempRTVOwners,
				   OutResource.TempRTVs);
	}

	bool HasVSMResources(const FShadowVSMResource& Resource)
	{
		return Resource.MomentsSRV != nullptr &&
			   Resource.TempSRV != nullptr &&
			   !Resource.MomentRTVs.empty() &&
			   !Resource.TempRTVs.empty();
	}

	FShadowVSMResource* AcquirePooledVSMResource(
		ID3D11Device* Device,
		uint32 Resolution,
		uint32 SliceCount,
		bool bCreateCubeSRV)
	{
		if (Device == nullptr || Resolution == 0u || SliceCount == 0u || (bCreateCubeSRV && (SliceCount % 6u) != 0u))
		{
			return nullptr;
		}

		const FShadowVSMResourceDesc Desc = { Resolution, SliceCount, bCreateCubeSRV };
		for (FPooledShadowVSMResource& Entry : GVSMResourcePool)
		{
			if (!Entry.bInUse && MatchesVSMResourceDesc(Entry.Desc, Desc))
			{
				Entry.bInUse = true;
				FFrameSpikeProfiler::Get().AddCounter("VSM resource reuses");
				return &Entry.Resource;
			}
		}

		FPooledShadowVSMResource Entry;
		Entry.Desc = Desc;
		{
			FRAME_SPIKE_SCOPE("VSM resource create");
			const bool bCreated =
				bCreateCubeSRV
					? CreateVSMCubeResources(Device, Resolution, SliceCount / 6u, Entry.Resource)
					: CreateVSMResources(Device, Resolution, SliceCount, Entry.Resource);
			if (!bCreated)
			{
				return nullptr;
			}
		}

		Entry.bInUse = true;
		FFrameSpikeProfiler::Get().AddCounter("VSM resource creates");
		GVSMResourcePool.push_back(std::move(Entry));
		return &GVSMResourcePool.back().Resource;
	}

	void ReleasePooledVSMResource(FShadowVSMResource* Resource)
	{
		if (Resource == nullptr)
		{
			return;
		}

		for (FPooledShadowVSMResource& Entry : GVSMResourcePool)
		{
			if (&Entry.Resource == Resource)
			{
				Entry.bInUse = false;
				return;
			}
		}
	}
} // namespace

bool FShadowPass::Initialize()
{
	return true;
}

bool FShadowPass::Release()
{
	ShaderBinding.reset();
	VSMConvertShaderBinding.reset();
	VSMBlurShaderBinding.reset();
	PointVSMShaderBinding.reset();
	GShadowMaps.clear();
	GVSMResources.clear();
	GVSMResourcePool.clear();
	GLightToShadowIndices.clear();
	GShadowCBData = FOpaqueRenderPass::FShadowArrayCB{};
	bSkip = false;
	OutSRV = nullptr;
	OutRTV = nullptr;
	return true;
}

TArray<FShadowMap>& FShadowPass::GetShadowMaps()
{
	return GShadowMaps;
}

const TArray<int32>& FShadowPass::GetLightToShadowIndices()
{
	return GLightToShadowIndices;
}

const FOpaqueRenderPass::FShadowArrayCB& FShadowPass::GetShadowCBData()
{
	return GShadowCBData;
}

ID3D11ShaderResourceView* FShadowPass::GetVSM2DShadowSRV()
{
	for (size_t ShadowMapIndex = 0; ShadowMapIndex < GShadowMaps.size() && ShadowMapIndex < GVSMResources.size(); ++ShadowMapIndex)
	{
		if (GShadowMaps[ShadowMapIndex].MapType == EShadowMapType::VSM2D &&
			GVSMResources[ShadowMapIndex] != nullptr &&
			GVSMResources[ShadowMapIndex]->MomentsSRV != nullptr)
		{
			return GVSMResources[ShadowMapIndex]->MomentsSRV.Get();
		}
	}

	return nullptr;
}

ID3D11ShaderResourceView* FShadowPass::GetVSMCubeShadowSRV()
{
	for (size_t ShadowMapIndex = 0; ShadowMapIndex < GShadowMaps.size() && ShadowMapIndex < GVSMResources.size(); ++ShadowMapIndex)
	{
		if (GShadowMaps[ShadowMapIndex].MapType == EShadowMapType::VSMCube &&
			GVSMResources[ShadowMapIndex] != nullptr &&
			GVSMResources[ShadowMapIndex]->MomentsCubeSRV != nullptr)
		{
			return GVSMResources[ShadowMapIndex]->MomentsCubeSRV.Get();
		}
	}

	return nullptr;
}

bool FShadowPass::EnsureVSMBindings(const FRenderPassContext* Context)
{
	if (Context == nullptr || Context->Device == nullptr)
	{
		return false;
	}

	UShader* ConvertShader = FResourceManager::Get().GetShader("Shaders/Multipass/ShadowVSMConvertPass.hlsl");
	UShader* BlurShader = FResourceManager::Get().GetShader("Shaders/Multipass/ShadowVSMBlurPass.hlsl");
	UShader* PointVSMShader = FResourceManager::Get().GetShader("Shaders/Multipass/ShadowPointVSMPass.hlsl");
	if (ConvertShader == nullptr || BlurShader == nullptr || PointVSMShader == nullptr)
	{
		return false;
	}

	if (!VSMConvertShaderBinding || VSMConvertShaderBinding->GetShader() != ConvertShader)
	{
		VSMConvertShaderBinding = ConvertShader->CreateBindingInstance(Context->Device);
	}

	if (!VSMBlurShaderBinding || VSMBlurShaderBinding->GetShader() != BlurShader)
	{
		VSMBlurShaderBinding = BlurShader->CreateBindingInstance(Context->Device);
	}

	if (!PointVSMShaderBinding || PointVSMShaderBinding->GetShader() != PointVSMShader)
	{
		PointVSMShaderBinding = PointVSMShader->CreateBindingInstance(Context->Device);
	}

	return VSMConvertShaderBinding != nullptr && VSMBlurShaderBinding != nullptr && PointVSMShaderBinding != nullptr;
}

bool FShadowPass::Begin(const FRenderPassContext* Context)
{
	UShader* Shader = FResourceManager::Get().GetShader("Shaders/ShadowMap.hlsl");
	if (!ShaderBinding || ShaderBinding->GetShader() != Shader)
	{
		ShaderBinding = (Shader != nullptr) ? Shader->CreateBindingInstance(Context->Device) : nullptr;
	}

	if (!ShaderBinding)
	{
		bSkip = true;
		return true;
	}

	for (size_t ShadowMapIndex = 0; ShadowMapIndex < GShadowMaps.size(); ++ShadowMapIndex)
	{
		FShadowMap& ShadowMap = GShadowMaps[ShadowMapIndex];
		if (ShadowMap.bOwnsResource && ShadowMap.Resource != nullptr)
		{
			Context->ShadowResourcePool->Release(ShadowMap.Resource);
		}

		if (ShadowMap.bOwnsResource && ShadowMapIndex < GVSMResources.size())
		{
			ReleasePooledVSMResource(GVSMResources[ShadowMapIndex]);
		}
	}

	GShadowMaps.clear();
	GVSMResources.clear();
	GLightToShadowIndices.clear();
	GShadowCBData = FOpaqueRenderPass::FShadowArrayCB{};
	OutSRV = nullptr;
	OutRTV = nullptr;
	bSkip = false;

	const TArray<FRenderLight>& Lights = Context->RenderBus->GetLights();
	GLightToShadowIndices.assign(Lights.size(), -1);

	std::vector<FShadowRequest> ShadowRequests =
		ShadowLightSelector.SelectShadowLights(
			Lights,
			Context->RenderBus->GetCameraPosition(),
			Context->RenderBus->GetCameraState());

	if (ShadowRequests.empty())
	{
		bSkip = true;
		return true;
	}

	std::array<std::vector<FShadowRequest>, static_cast<size_t>(ELightType::Max)> Buckets;
	for (const FShadowRequest& Req : ShadowRequests)
	{
		Buckets[static_cast<size_t>(Req.Type)].push_back(Req);
	}

	for (std::vector<FShadowRequest>& Bucket : Buckets)
	{
		std::sort(
			Bucket.begin(),
			Bucket.end(),
			[](const FShadowRequest& A, const FShadowRequest& B)
			{
				return A.Resolution > B.Resolution;
			});
	}

	ShadowRequests.clear();
	for (const std::vector<FShadowRequest>& Bucket : Buckets)
	{
		for (const FShadowRequest& Req : Bucket)
		{
			ShadowRequests.push_back(Req);
		}
	}

	const FShowFlags ShowFlags = Context->RenderBus->GetShowFlags();
	const EShadowFilterMode RequestedShadowFilter = ShowFlags.ShadowFilter;
	bool bUseVSMFilter = ShowFlags.UsesVSMShadowFilter();
	if (bUseVSMFilter && !EnsureVSMBindings(Context))
	{
		bUseVSMFilter = false;
	}

	for (FShadowRequest& ShadowRequest : ShadowRequests)
	{
		const FRenderLight& ShadowLight = Lights[ShadowRequest.LightId];
		ShadowRequest.ProjectionMode = ResolveShadowProjectionMode(ShowFlags, ShadowLight);
		ShadowRequest.bPSM = ShadowRequest.ProjectionMode == EShadowProjectionMode::PSM;
		ShadowRequest.bUseVSM =
			bUseVSMFilter &&
			(ShadowRequest.Type == ELightType::LightType_Directional ||
			 ShadowRequest.Type == ELightType::LightType_Point);
	}

	uint32 ShadowIndexCounter = 0;
	uint32 PointShadowTextureIndexCounter = 0;

	AtlasAllocator.Reset();

	FShadowResource* SharedPointShadowResource = nullptr;
	FShadowVSMResource* SharedPointVSMResource = nullptr;
	uint32 SharedPointShadowFaceOffset = 0;
	uint32 PointShadowRequestCount = 0;
	uint32 SharedPointShadowResolution = 0;

	for (const FShadowRequest& ShadowRequest : ShadowRequests)
	{
		if (ShadowRequest.Type != ELightType::LightType_Point)
		{
			continue;
		}

		++PointShadowRequestCount;
		SharedPointShadowResolution = std::max<uint32>(SharedPointShadowResolution, ShadowRequest.Resolution);
	}

	if (PointShadowRequestCount > 0)
	{
		FShadowRequestDesc PointArrayDesc = {};
		PointArrayDesc.AllocationMode = EShadowAllocationMode::ArrayBased;
		PointArrayDesc.MapType = EShadowMapType::DepthCube;
		PointArrayDesc.Resolution = SharedPointShadowResolution;
		PointArrayDesc.CubeCount = std::min<uint32>(PointShadowRequestCount, MAX_SHADOW_LIGHTS);

		if (!AcquireResource(Context, PointArrayDesc, &SharedPointShadowResource))
		{
			SharedPointShadowResource = nullptr;
			PointShadowRequestCount = 0;
		}
		else if (bUseVSMFilter)
		{
			const uint32 PointSliceCount = PointArrayDesc.CubeCount * 6u;
			SharedPointVSMResource =
				AcquirePooledVSMResource(Context->Device, SharedPointShadowResolution, PointSliceCount, true);
		}
	}

	for (const FShadowRequest& ShadowRequest : ShadowRequests)
	{
		if (ShadowIndexCounter >= MAX_SHADOW_LIGHTS)
		{
			break;
		}

		const FRenderLight& ShadowLight = Lights[ShadowRequest.LightId];

		if (ShadowRequest.Type == ELightType::LightType_Point)
		{
			if (SharedPointShadowResource == nullptr)
			{
				continue;
			}

			FShadowMap ShadowMap;
			ShadowMap.Resource = SharedPointShadowResource;
			ShadowMap.MapType =
				(ShadowRequest.bUseVSM &&
				 SharedPointVSMResource != nullptr &&
				 HasVSMResources(*SharedPointVSMResource) &&
				 SharedPointVSMResource->MomentsCubeSRV != nullptr)
					? EShadowMapType::VSMCube
					: EShadowMapType::DepthCube;
			ShadowMap.LightId = ShadowRequest.LightId;
			ShadowMap.SourceLightSlotIndex = ShadowLight.SourceLightSlotIndex;
			ShadowMap.ResourceSliceOffset = SharedPointShadowFaceOffset;
			ShadowMap.bOwnsResource = (PointShadowTextureIndexCounter == 0);
			ShadowMap.LightType = ShadowRequest.Type;

			if (!BuildViews(Context, ShadowRequest, ShadowMap.Views) ||
				!BuildSlices(Context, ShadowRequest, ShadowMap.Slices))
			{
				if (ShadowMap.bOwnsResource)
				{
					Context->ShadowResourcePool->Release(SharedPointShadowResource);
					SharedPointShadowResource = nullptr;
					ReleasePooledVSMResource(SharedPointVSMResource);
					SharedPointVSMResource = nullptr;
				}
				continue;
			}

			GShadowMaps.push_back(ShadowMap);
			GVSMResources.push_back(ShadowMap.MapType == EShadowMapType::VSMCube ? SharedPointVSMResource : nullptr);
			GLightToShadowIndices[ShadowRequest.LightId] = ShadowIndexCounter;

			FOpaqueRenderPass::FShadowCB& CB = GShadowCBData.ShadowDataArray[ShadowIndexCounter];
			CB.UVOffset = FVector2(0.0f, 0.0f);
			CB.UVScale = FVector2(1.0f, 1.0f);
			CB.ShadowLightPosition = ShadowLight.Position;
			CB.ShadowFar = std::max(ShadowLight.Radius, 0.1f);
			CB.ShadowBias = ComputeShadowCompareBias(ShadowLight);
			CB.ShadowSlopeBias = ShadowLight.ShadowSlopeBias;
			CB.ShadowFilterScale = ComputeShadowFilterScale(ShadowLight);
			CB.ShadowMapType = static_cast<uint32>(ShadowMap.MapType);
			CB.SliceCount = 1;
			CB.ShadowTextureIndex = PointShadowTextureIndexCounter;
			ApplyShadowFilterMode(CB, RequestedShadowFilter, ShadowMap.MapType);
			CB.PointShadowTexelSize =
				2.0f / std::max<float>(static_cast<float>(SharedPointShadowResource->Resolution), 1.0f);
			ApplyVSMParameters(CB, ShadowLight, ShadowMap.MapType);
			ApplyShadowProjectionMode(CB, Context, ShadowLight, EShadowProjectionMode::Standard);

			SharedPointShadowFaceOffset += 6;
			++PointShadowTextureIndexCounter;
			++ShadowIndexCounter;
			continue;
		}

		if (ShadowRequest.Type == ELightType::LightType_Spot)
		{
			FAtlasAllocationResult AllocResult;
			if (!AtlasAllocator.Allocate(ShadowRequest.Resolution, AllocResult))
			{
				FShadowRequestDesc Desc = {};
				Desc.AllocationMode = EShadowAllocationMode::AtlasPacked;
				Desc.MapType = EShadowMapType::Depth2D;
				Desc.Resolution = kAtlasSize;
				Desc.CascadeCount = static_cast<uint32>(ShadowRequest.Cascades.size());

				FShadowResource* NewAtlasRes = nullptr;
				if (!AcquireResource(Context, Desc, &NewAtlasRes))
				{
					continue;
				}

				AtlasAllocator.AddNewAtlasResource(NewAtlasRes);

				FShadowMap NewAtlasMap;
				NewAtlasMap.Resource = NewAtlasRes;
				NewAtlasMap.MapType = EShadowMapType::Depth2D;
				NewAtlasMap.LightType = ELightType::LightType_Spot;
				NewAtlasMap.bOwnsResource = true;
				GShadowMaps.push_back(NewAtlasMap);
				GVSMResources.push_back(nullptr);

				const uint32 NewAtlasIndex = static_cast<uint32>(GShadowMaps.size() - 1);
				AtlasAllocator.SetCurrentAtlasIndex(NewAtlasIndex);
				if (!AtlasAllocator.Allocate(ShadowRequest.Resolution, AllocResult))
				{
					continue;
				}
			}

			const uint32 AtlasIndex = AtlasAllocator.GetCurrentAtlasIndex();
			FShadowMap& CurrentAtlasMap = GShadowMaps[AtlasIndex];
			if (!BuildViews(Context, ShadowRequest, CurrentAtlasMap.Views))
			{
				continue;
			}

			FShadowSlice Slice;
			Slice.Index = static_cast<uint32>(CurrentAtlasMap.Slices.size());
			Slice.Type = EShadowSliceType::Atlas;
			Slice.UVOffset = AllocResult.UVOffset;
			Slice.UVScale = AllocResult.UVScale;
			Slice.LightId = ShadowRequest.LightId;
			Slice.SourceLightSlotIndex = ShadowLight.SourceLightSlotIndex;
			CurrentAtlasMap.Slices.push_back(Slice);

			const uint32 ViewIndex = static_cast<uint32>(CurrentAtlasMap.Views.size() - 1);
			GLightToShadowIndices[ShadowRequest.LightId] = ShadowIndexCounter;

			FOpaqueRenderPass::FShadowCB& CB = GShadowCBData.ShadowDataArray[ShadowIndexCounter];
			CB.ShadowLightView[0] = CurrentAtlasMap.Views[ViewIndex].LightView;
			CB.ShadowLightProjection[0] = CurrentAtlasMap.Views[ViewIndex].LightProjection;
			CB.UVOffset = AllocResult.UVOffset;
			CB.UVScale = AllocResult.UVScale;
			CB.ShadowBias = ComputeShadowCompareBias(ShadowLight);
			CB.ShadowSlopeBias = ShadowLight.ShadowSlopeBias;
			CB.ShadowFilterScale = ComputeShadowFilterScale(ShadowLight);
			CB.ShadowMapType = static_cast<uint32>(CurrentAtlasMap.MapType);
			CB.SliceCount = 1;
			CB.ShadowTextureIndex = 1u;
			ApplyShadowFilterMode(CB, RequestedShadowFilter, CurrentAtlasMap.MapType);
			CB.PointShadowTexelSize = 0.0f;
			ApplyVSMParameters(CB, ShadowLight, CurrentAtlasMap.MapType);
			ApplyShadowProjectionMode(CB, Context, ShadowLight, ShadowRequest.ProjectionMode);

			++ShadowIndexCounter;
			continue;
		}

		FShadowMap ShadowMap;
		if (!MakeShadowMap(Context, ShadowRequest, ShadowMap))
		{
			continue;
		}

		FShadowVSMResource* VSMResource = nullptr;
		if (ShadowRequest.bUseVSM)
		{
			const uint32 SliceCount = static_cast<uint32>(ShadowMap.Views.size());
			VSMResource = AcquirePooledVSMResource(
				Context->Device,
				ShadowMap.Resource ? ShadowMap.Resource->Resolution : 0u,
				SliceCount,
				false);
			if (VSMResource == nullptr)
			{
				ShadowMap.MapType = EShadowMapType::Depth2D;
			}
		}

		GShadowMaps.push_back(ShadowMap);
		GVSMResources.push_back(VSMResource);
		GLightToShadowIndices[ShadowRequest.LightId] = ShadowIndexCounter;

		FOpaqueRenderPass::FShadowCB& CB = GShadowCBData.ShadowDataArray[ShadowIndexCounter];
		for (size_t CascadeIndex = 0; CascadeIndex < ShadowRequest.Cascades.size(); ++CascadeIndex)
		{
			CB.ShadowLightView[CascadeIndex] = ShadowMap.Views[CascadeIndex].LightView;
			CB.ShadowLightProjection[CascadeIndex] = ShadowMap.Views[CascadeIndex].LightProjection;
			CB.CascadeSplits[CascadeIndex] = ShadowRequest.Cascades[CascadeIndex].Far;
		}

		CB.UVOffset = FVector2(0.0f, 0.0f);
		CB.UVScale = FVector2(1.0f, 1.0f);
		CB.ShadowLightPosition = ShadowLight.Position;
		CB.ShadowFar = std::max(ShadowLight.Radius, 0.1f);
		CB.ShadowBias = ComputeShadowCompareBias(ShadowLight);
		CB.ShadowSlopeBias = ShadowLight.ShadowSlopeBias;
		CB.ShadowFilterScale = ComputeShadowFilterScale(ShadowLight);
		CB.ShadowMapType = static_cast<uint32>(ShadowMap.MapType);
		CB.SliceCount = static_cast<uint32>(ShadowRequest.Cascades.size());
		CB.ShadowTextureIndex = 0u;
		ApplyShadowFilterMode(CB, RequestedShadowFilter, ShadowMap.MapType);
		CB.PointShadowTexelSize = 0.0f;
		ApplyVSMParameters(CB, ShadowLight, ShadowMap.MapType);
		ApplyShadowProjectionMode(CB, Context, ShadowLight, ShadowRequest.ProjectionMode);

		++ShadowIndexCounter;
	}

	if (SharedPointShadowResource != nullptr && PointShadowTextureIndexCounter == 0)
	{
		Context->ShadowResourcePool->Release(SharedPointShadowResource);
		SharedPointShadowResource = nullptr;
		ReleasePooledVSMResource(SharedPointVSMResource);
		SharedPointVSMResource = nullptr;
	}

	if (GShadowMaps.empty())
	{
		bSkip = true;
		return true;
	}

	OutSRV = (GShadowMaps[0].Resource != nullptr) ? GShadowMaps[0].Resource->SRV : nullptr;
	OutRTV = nullptr;
	ShaderBinding->ApplyFrameParameters(*Context->RenderBus);
	return true;
}

bool FShadowPass::DrawCommand(const FRenderPassContext* Context)
{
	if (bSkip || !ShaderBinding)
	{
		return true;
	}

	const FRenderBus* RenderBus = Context->RenderBus;
	const TArray<FRenderCommand>& Commands = RenderBus->GetCommands(ERenderPass::Opaque);
	if (Commands.empty())
	{
		return true;
	}

	D3D11_VIEWPORT OldVP[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
	UINT OldVPCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
	Context->DeviceContext->RSGetViewports(&OldVPCount, OldVP);
	Context->DeviceContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

	ID3D11DepthStencilState* DefaultDepthStencilState =
		FResourceManager::Get().GetOrCreateDepthStencilState(EDepthStencilType::Default, Context->Device);
	ID3D11BlendState* OpaqueBlendState =
		FResourceManager::Get().GetOrCreateBlendState(EBlendType::Opaque, Context->Device);
	if (DefaultDepthStencilState == nullptr || OpaqueBlendState == nullptr)
	{
		Context->DeviceContext->RSSetViewports(OldVPCount, OldVP);
		return false;
	}

	ID3D11ShaderResourceView* NullShadowSRVs[5] = { nullptr, nullptr, nullptr, nullptr, nullptr };
	Context->DeviceContext->PSSetShaderResources(14, 5, NullShadowSRVs);
	Context->DeviceContext->OMSetDepthStencilState(DefaultDepthStencilState, 0);
	Context->DeviceContext->OMSetBlendState(OpaqueBlendState, nullptr, 0xFFFFFFFF);

	const auto& Lights = RenderBus->GetLights();
	const FShowFlags ShowFlags = RenderBus->GetShowFlags();

	auto GetLight = [&](uint32 LightId) -> const FRenderLight*
	{
		return (LightId < static_cast<uint32>(Lights.size())) ? &Lights[LightId] : nullptr;
	};

	auto DrawShadowCommands = [&](const FShadowViewInfo& ViewInfo, const FRenderLight* ShadowLight) -> bool
	{
		if (ShadowLight == nullptr)
		{
			return true;
		}

		ShaderBinding->SetMatrix4("View", ViewInfo.LightView);
		ShaderBinding->SetMatrix4("Projection", ViewInfo.LightProjection);

		const EShadowProjectionMode ProjectionMode = ResolveShadowProjectionMode(ShowFlags, *ShadowLight);
		const bool bUsePSM = ProjectionMode == EShadowProjectionMode::PSM;
		ShaderBinding->SetMatrix4("PSM", bUsePSM ? ComputePSMMatrix(Context, *ShadowLight) : FMatrix::Identity);
		ShaderBinding->SetUInt("isPSM", bUsePSM ? 1u : 0u);

		for (const FRenderCommand& Cmd : Commands)
		{
			if (Cmd.Type == ERenderCommandType::PostProcessOutline)
			{
				continue;
			}

			if (Cmd.WorldBounds.IsValid() &&
				ShadowLight->Type != static_cast<uint32>(ELightType::LightType_Directional))
			{
				const FVector BoundsCenter = Cmd.WorldBounds.GetCenter();
				const float BoundsRadius = Cmd.WorldBounds.GetExtent().Size();
				if (FVector::Dist(BoundsCenter, ShadowLight->Position) - BoundsRadius > ShadowLight->Radius)
				{
					continue;
				}
			}

			if (Cmd.MeshBuffer == nullptr || !Cmd.MeshBuffer->IsValid())
			{
				return false;
			}

			ID3D11Buffer* VertexBuffer = Cmd.MeshBuffer->GetVertexBuffer().GetBuffer();
			const uint32 Stride = Cmd.MeshBuffer->GetVertexBuffer().GetStride();
			const uint32 VertexCount = Cmd.MeshBuffer->GetVertexBuffer().GetVertexCount();
			if (VertexBuffer == nullptr || VertexCount == 0 || Stride == 0)
			{
				return false;
			}

			if (Cmd.Material)
			{
				ShaderBinding->ApplyPerObjectParameters(Cmd.PerObjectConstants);
				ShaderBinding->Bind(Context->DeviceContext);
				Context->DeviceContext->PSSetShader(nullptr, nullptr, 0);
			}

			CheckOverrideViewMode(Context);

			uint32 Offset = 0;
			Context->DeviceContext->IASetVertexBuffers(0, 1, &VertexBuffer, &Stride, &Offset);

			ID3D11Buffer* IndexBuffer = Cmd.MeshBuffer->GetIndexBuffer().GetBuffer();
			if (IndexBuffer != nullptr)
			{
				Context->DeviceContext->IASetIndexBuffer(IndexBuffer, DXGI_FORMAT_R32_UINT, 0);
				Context->DeviceContext->DrawIndexed(Cmd.SectionIndexCount, Cmd.SectionIndexStart, 0);
			}
			else
			{
				Context->DeviceContext->Draw(VertexCount, 0);
			}
		}

		return true;
	};

	for (size_t ShadowMapIndex = 0; ShadowMapIndex < GShadowMaps.size(); ++ShadowMapIndex)
	{
		FShadowMap& ShadowMap = GShadowMaps[ShadowMapIndex];
		if (ShadowMap.Resource == nullptr)
		{
			continue;
		}

		const bool bPointVSM =
			ShadowMap.MapType == EShadowMapType::VSMCube &&
			ShadowMapIndex < GVSMResources.size() &&
			GVSMResources[ShadowMapIndex] != nullptr &&
			HasVSMResources(*GVSMResources[ShadowMapIndex]) &&
			PointVSMShaderBinding != nullptr;

		if (bPointVSM)
		{
			FShadowVSMResource& VSMResource = *GVSMResources[ShadowMapIndex];
			const FRenderLight* DrawShadowLight = GetLight(ShadowMap.LightId);
			if (DrawShadowLight == nullptr)
			{
				continue;
			}

			const uint32 DrawSliceCount = std::min<uint32>(
				static_cast<uint32>(ShadowMap.Views.size()),
				std::min<uint32>(
					static_cast<uint32>(
						ShadowMap.Resource->DSVs.size() > ShadowMap.ResourceSliceOffset
							? ShadowMap.Resource->DSVs.size() - ShadowMap.ResourceSliceOffset
							: 0),
					static_cast<uint32>(
						VSMResource.MomentRTVs.size() > ShadowMap.ResourceSliceOffset
							? VSMResource.MomentRTVs.size() - ShadowMap.ResourceSliceOffset
							: 0)));
			if (DrawSliceCount == 0)
			{
				continue;
			}

			D3D11_VIEWPORT ShadowViewport = {};
			ShadowViewport.TopLeftX = 0.0f;
			ShadowViewport.TopLeftY = 0.0f;
			ShadowViewport.Width = static_cast<float>(ShadowMap.Resource->Resolution);
			ShadowViewport.Height = static_cast<float>(ShadowMap.Resource->Resolution);
			ShadowViewport.MinDepth = 0.0f;
			ShadowViewport.MaxDepth = 1.0f;
			Context->DeviceContext->RSSetViewports(1, &ShadowViewport);

			for (uint32 ViewIndex = 0; ViewIndex < DrawSliceCount; ++ViewIndex)
			{
				const uint32 ResourceSliceIndex = ShadowMap.ResourceSliceOffset + ViewIndex;
				ID3D11RenderTargetView* MomentRTV = VSMResource.MomentRTVs[ResourceSliceIndex];
				ID3D11DepthStencilView* DepthDSV = ShadowMap.Resource->DSVs[ResourceSliceIndex];
				const float ShadowFar = std::max(DrawShadowLight->Radius, 0.1f);
				const float ClearMoments[4] = { ShadowFar, ShadowFar * ShadowFar, 0.0f, 0.0f };

				Context->DeviceContext->ClearRenderTargetView(MomentRTV, ClearMoments);
				Context->DeviceContext->ClearDepthStencilView(
					DepthDSV,
					D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL,
					1.0f,
					0);
				Context->DeviceContext->OMSetDepthStencilState(DefaultDepthStencilState, 0);
				Context->DeviceContext->OMSetBlendState(OpaqueBlendState, nullptr, 0xFFFFFFFF);
				Context->DeviceContext->OMSetRenderTargets(1, &MomentRTV, DepthDSV);

				PointVSMShaderBinding->ApplyFrameParameters(*RenderBus);
				PointVSMShaderBinding->SetMatrix4("View", ShadowMap.Views[ViewIndex].LightView);
				PointVSMShaderBinding->SetMatrix4("Projection", ShadowMap.Views[ViewIndex].LightProjection);
				PointVSMShaderBinding->SetFloat("ShadowFar", ShadowFar);

				for (const FRenderCommand& Cmd : Commands)
				{
					if (Cmd.Type == ERenderCommandType::PostProcessOutline)
					{
						continue;
					}

					if (Cmd.WorldBounds.IsValid())
					{
						const FVector BoundsCenter = Cmd.WorldBounds.GetCenter();
						const float BoundsRadius = Cmd.WorldBounds.GetExtent().Size();
						if (FVector::Dist(BoundsCenter, DrawShadowLight->Position) - BoundsRadius > DrawShadowLight->Radius)
						{
							continue;
						}
					}

					if (Cmd.MeshBuffer == nullptr || !Cmd.MeshBuffer->IsValid())
					{
						Context->DeviceContext->RSSetViewports(OldVPCount, OldVP);
						return false;
					}

					uint32 Offset = 0;
					ID3D11Buffer* VertexBuffer = Cmd.MeshBuffer->GetVertexBuffer().GetBuffer();
					if (VertexBuffer == nullptr)
					{
						Context->DeviceContext->RSSetViewports(OldVPCount, OldVP);
						return false;
					}

					const uint32 VertexCount = Cmd.MeshBuffer->GetVertexBuffer().GetVertexCount();
					const uint32 Stride = Cmd.MeshBuffer->GetVertexBuffer().GetStride();
					if (VertexCount == 0 || Stride == 0)
					{
						Context->DeviceContext->RSSetViewports(OldVPCount, OldVP);
						return false;
					}

					PointVSMShaderBinding->ApplyPerObjectParameters(Cmd.PerObjectConstants);
					PointVSMShaderBinding->Bind(Context->DeviceContext);
					CheckOverrideViewMode(Context);
					Context->DeviceContext->IASetVertexBuffers(0, 1, &VertexBuffer, &Stride, &Offset);

					ID3D11Buffer* IndexBuffer = Cmd.MeshBuffer->GetIndexBuffer().GetBuffer();
					if (IndexBuffer != nullptr)
					{
						Context->DeviceContext->IASetIndexBuffer(IndexBuffer, DXGI_FORMAT_R32_UINT, 0);
						Context->DeviceContext->DrawIndexed(Cmd.SectionIndexCount, Cmd.SectionIndexStart, 0);
					}
					else
					{
						Context->DeviceContext->Draw(VertexCount, 0);
					}
				}
			}

			continue;
		}

		const bool bAtlasMap =
			ShadowMap.MapType == EShadowMapType::Depth2D &&
			!ShadowMap.Slices.empty() &&
			ShadowMap.Slices[0].Type == EShadowSliceType::Atlas;

		if (bAtlasMap)
		{
			if (ShadowMap.Resource->DSVs.empty())
			{
				continue;
			}

			Context->DeviceContext->ClearDepthStencilView(
				ShadowMap.Resource->DSVs[0],
				D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL,
				1.0f,
				0);
			Context->DeviceContext->OMSetRenderTargets(0, nullptr, ShadowMap.Resource->DSVs[0]);

			const uint32 DrawSliceCount = std::min<uint32>(
				static_cast<uint32>(ShadowMap.Views.size()),
				static_cast<uint32>(ShadowMap.Slices.size()));

			for (uint32 SliceIndex = 0; SliceIndex < DrawSliceCount; ++SliceIndex)
			{
				const FShadowSlice& Slice = ShadowMap.Slices[SliceIndex];
				D3D11_VIEWPORT ShadowViewport = {};
				ShadowViewport.TopLeftX = Slice.UVOffset.X * ShadowMap.Resource->Resolution;
				ShadowViewport.TopLeftY = Slice.UVOffset.Y * ShadowMap.Resource->Resolution;
				ShadowViewport.Width = std::max(1.0f, Slice.UVScale.X * ShadowMap.Resource->Resolution);
				ShadowViewport.Height = std::max(1.0f, Slice.UVScale.Y * ShadowMap.Resource->Resolution);
				ShadowViewport.MinDepth = 0.0f;
				ShadowViewport.MaxDepth = 1.0f;
				Context->DeviceContext->RSSetViewports(1, &ShadowViewport);

				if (!DrawShadowCommands(ShadowMap.Views[SliceIndex], GetLight(Slice.LightId)))
				{
					Context->DeviceContext->RSSetViewports(OldVPCount, OldVP);
					return false;
				}
			}

			continue;
		}

		const uint32 DrawSliceCount = std::min<uint32>(
			static_cast<uint32>(
				ShadowMap.Resource->DSVs.size() > ShadowMap.ResourceSliceOffset
					? ShadowMap.Resource->DSVs.size() - ShadowMap.ResourceSliceOffset
					: 0),
			static_cast<uint32>(ShadowMap.Views.size()));
		if (DrawSliceCount == 0)
		{
			continue;
		}

		D3D11_VIEWPORT ShadowViewport = {};
		ShadowViewport.TopLeftX = 0.0f;
		ShadowViewport.TopLeftY = 0.0f;
		ShadowViewport.Width = static_cast<float>(ShadowMap.Resource->Resolution);
		ShadowViewport.Height = static_cast<float>(ShadowMap.Resource->Resolution);
		ShadowViewport.MinDepth = 0.0f;
		ShadowViewport.MaxDepth = 1.0f;
		Context->DeviceContext->RSSetViewports(1, &ShadowViewport);

		const FRenderLight* DrawShadowLight = GetLight(ShadowMap.LightId);
		for (uint32 ViewIndex = 0; ViewIndex < DrawSliceCount; ++ViewIndex)
		{
			Context->DeviceContext->ClearDepthStencilView(
				ShadowMap.Resource->DSVs[ShadowMap.ResourceSliceOffset + ViewIndex],
				D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL,
				1.0f,
				0);
			Context->DeviceContext->OMSetRenderTargets(0, nullptr, ShadowMap.Resource->DSVs[ShadowMap.ResourceSliceOffset + ViewIndex]);

			if (!DrawShadowCommands(ShadowMap.Views[ViewIndex], DrawShadowLight))
			{
				Context->DeviceContext->RSSetViewports(OldVPCount, OldVP);
				return false;
			}
		}
	}

	Context->DeviceContext->RSSetViewports(OldVPCount, OldVP);
	return true;
}

bool FShadowPass::End(const FRenderPassContext* Context)
{
	if (bSkip)
	{
		return true;
	}

	bool bHasVSMWork = false;
	for (size_t ShadowMapIndex = 0; ShadowMapIndex < GShadowMaps.size() && ShadowMapIndex < GVSMResources.size(); ++ShadowMapIndex)
	{
		if ((GShadowMaps[ShadowMapIndex].MapType == EShadowMapType::VSM2D ||
			 GShadowMaps[ShadowMapIndex].MapType == EShadowMapType::VSMCube) &&
			GVSMResources[ShadowMapIndex] != nullptr &&
			HasVSMResources(*GVSMResources[ShadowMapIndex]))
		{
			bHasVSMWork = true;
			break;
		}
	}

	if (!bHasVSMWork)
	{
		return true;
	}

	FRAME_SPIKE_SCOPE("VSM moment pass");
	GPU_SCOPE_STAT("VSM moment pass");

	if (!EnsureVSMBindings(Context))
	{
		return false;
	}

	ID3D11SamplerState* LinearClampSampler =
		FResourceManager::Get().GetOrCreateSamplerState(ESamplerType::EST_LinearClamp, Context->Device);
	ID3D11BlendState* OpaqueBlendState =
		FResourceManager::Get().GetOrCreateBlendState(EBlendType::Opaque, Context->Device);
	if (LinearClampSampler == nullptr || OpaqueBlendState == nullptr)
	{
		return false;
	}

	D3D11_VIEWPORT OldVP[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
	UINT OldVPCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
	Context->DeviceContext->RSGetViewports(&OldVPCount, OldVP);

	Context->DeviceContext->IASetInputLayout(nullptr);
	Context->DeviceContext->IASetVertexBuffers(0, 0, nullptr, nullptr, nullptr);
	Context->DeviceContext->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);
	Context->DeviceContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

	ID3D11ShaderResourceView* NullSRV = nullptr;
	auto UnbindPixelShaderInput = [&]()
	{
		Context->DeviceContext->PSSetShaderResources(0, 1, &NullSRV);
	};

	for (size_t ShadowMapIndex = 0; ShadowMapIndex < GShadowMaps.size() && ShadowMapIndex < GVSMResources.size(); ++ShadowMapIndex)
	{
		const FShadowMap& ShadowMap = GShadowMaps[ShadowMapIndex];
		if (GVSMResources[ShadowMapIndex] == nullptr)
		{
			continue;
		}

		FShadowVSMResource& VSMResource = *GVSMResources[ShadowMapIndex];
		const bool bPointVSM = ShadowMap.MapType == EShadowMapType::VSMCube;
		if ((ShadowMap.MapType != EShadowMapType::VSM2D && !bPointVSM) ||
			!HasVSMResources(VSMResource) ||
			ShadowMap.Resource == nullptr)
		{
			continue;
		}

		ID3D11ShaderResourceView* DepthInputSRV = bPointVSM ? nullptr : ShadowMap.Resource->SRV;
		if ((!bPointVSM && DepthInputSRV == nullptr) || (bPointVSM && VSMResource.MomentsCubeSRV == nullptr))
		{
			continue;
		}

		const uint32 AvailableMomentSlices =
			static_cast<uint32>(VSMResource.MomentRTVs.size() > ShadowMap.ResourceSliceOffset
				? VSMResource.MomentRTVs.size() - ShadowMap.ResourceSliceOffset
				: 0u);
		const uint32 AvailableTempSlices =
			static_cast<uint32>(VSMResource.TempRTVs.size() > ShadowMap.ResourceSliceOffset
				? VSMResource.TempRTVs.size() - ShadowMap.ResourceSliceOffset
				: 0u);
		const uint32 DrawSliceCount = std::min<uint32>(
			static_cast<uint32>(ShadowMap.Views.size()),
			std::min<uint32>(AvailableMomentSlices, AvailableTempSlices));
		if (DrawSliceCount == 0)
		{
			continue;
		}

		const FRenderLight* ShadowLight =
			(ShadowMap.LightId < static_cast<uint32>(Context->RenderBus->GetLights().size()))
				? &Context->RenderBus->GetLights()[ShadowMap.LightId]
				: nullptr;
		const float FilterScale = (ShadowLight != nullptr) ? ComputeShadowFilterScale(*ShadowLight) : 1.0f;
		const float InvResolution =
			1.0f / std::max<float>(static_cast<float>(ShadowMap.Resource->Resolution), 1.0f);
		D3D11_VIEWPORT ShadowViewport = {};
		ShadowViewport.TopLeftX = 0.0f;
		ShadowViewport.TopLeftY = 0.0f;
		ShadowViewport.Width = static_cast<float>(ShadowMap.Resource->Resolution);
		ShadowViewport.Height = static_cast<float>(ShadowMap.Resource->Resolution);
		ShadowViewport.MinDepth = 0.0f;
		ShadowViewport.MaxDepth = 1.0f;
		Context->DeviceContext->RSSetViewports(1, &ShadowViewport);

		for (uint32 SliceIndex = 0; SliceIndex < DrawSliceCount; ++SliceIndex)
		{
			const uint32 ResourceSliceIndex = ShadowMap.ResourceSliceOffset + SliceIndex;

			ID3D11RenderTargetView* MomentRTV = VSMResource.MomentRTVs[ResourceSliceIndex];
			if (!bPointVSM)
			{
				UnbindPixelShaderInput();
				Context->DeviceContext->OMSetBlendState(OpaqueBlendState, nullptr, 0xFFFFFFFF);
				Context->DeviceContext->OMSetRenderTargets(1, &MomentRTV, nullptr);
				VSMConvertShaderBinding->ApplyFrameParameters(*Context->RenderBus);
				VSMConvertShaderBinding->SetSRV("DepthShadowInput", DepthInputSRV);
				VSMConvertShaderBinding->SetInt("SliceIndex", static_cast<int32>(ResourceSliceIndex));
				VSMConvertShaderBinding->SetInt("LinearizeDepth", 0);
				VSMConvertShaderBinding->SetFloat("DepthLinearizeA", 0.0f);
				VSMConvertShaderBinding->SetFloat("DepthLinearizeB", 0.0f);
				VSMConvertShaderBinding->SetFloat("InvDepthRange", 1.0f);
				VSMConvertShaderBinding->Bind(Context->DeviceContext);
				Context->DeviceContext->Draw(3, 0);
			}

			if (bPointVSM)
			{
				continue;
			}

			ID3D11RenderTargetView* TempRTV = VSMResource.TempRTVs[ResourceSliceIndex];
			UnbindPixelShaderInput();
			Context->DeviceContext->OMSetBlendState(OpaqueBlendState, nullptr, 0xFFFFFFFF);
			Context->DeviceContext->OMSetRenderTargets(1, &TempRTV, nullptr);
			VSMBlurShaderBinding->ApplyFrameParameters(*Context->RenderBus);
			VSMBlurShaderBinding->SetSRV("MomentsInput", VSMResource.MomentsSRV.Get());
			VSMBlurShaderBinding->SetSampler("LinearClampSampler", LinearClampSampler);
			VSMBlurShaderBinding->SetInt("SliceIndex", static_cast<int32>(ResourceSliceIndex));
			VSMBlurShaderBinding->SetVector2("BlurDirection", FVector2(InvResolution * FilterScale, 0.0f));
			VSMBlurShaderBinding->Bind(Context->DeviceContext);
			Context->DeviceContext->Draw(3, 0);

			UnbindPixelShaderInput();
			Context->DeviceContext->OMSetBlendState(OpaqueBlendState, nullptr, 0xFFFFFFFF);
			Context->DeviceContext->OMSetRenderTargets(1, &MomentRTV, nullptr);
			VSMBlurShaderBinding->ApplyFrameParameters(*Context->RenderBus);
			VSMBlurShaderBinding->SetSRV("MomentsInput", VSMResource.TempSRV.Get());
			VSMBlurShaderBinding->SetSampler("LinearClampSampler", LinearClampSampler);
			VSMBlurShaderBinding->SetInt("SliceIndex", static_cast<int32>(ResourceSliceIndex));
			VSMBlurShaderBinding->SetVector2("BlurDirection", FVector2(0.0f, InvResolution * FilterScale));
			VSMBlurShaderBinding->Bind(Context->DeviceContext);
			Context->DeviceContext->Draw(3, 0);
		}
	}

	Context->DeviceContext->PSSetShaderResources(0, 1, &NullSRV);
	ID3D11SamplerState* NullSampler = nullptr;
	Context->DeviceContext->PSSetSamplers(0, 1, &NullSampler);
	Context->DeviceContext->OMSetRenderTargets(0, nullptr, nullptr);
	Context->DeviceContext->RSSetViewports(OldVPCount, OldVP);
	return true;
}

bool FShadowPass::MakeShadowMap(const FRenderPassContext* Context, const FShadowRequest& Req, FShadowMap& OutShadowMap)
{
	FShadowRequestDesc Desc = {};
	Desc.AllocationMode = EShadowAllocationMode::ArrayBased;
	Desc.MapType =
		Req.Type == ELightType::LightType_Point
			? (Req.bUseVSM ? EShadowMapType::VSMCube : EShadowMapType::DepthCube)
			: (Req.bUseVSM ? EShadowMapType::VSM2D : EShadowMapType::Depth2D);
	Desc.Resolution = Req.Resolution;
	Desc.CascadeCount = static_cast<uint32>(Req.Cascades.size());
	Desc.CubeCount = (Req.Type == ELightType::LightType_Point) ? 1u : 0u;

	if (!AcquireResource(Context, Desc, &OutShadowMap.Resource))
	{
		return false;
	}

	if (!BuildViews(Context, Req, OutShadowMap.Views))
	{
		if (OutShadowMap.Resource != nullptr)
		{
			Context->ShadowResourcePool->Release(OutShadowMap.Resource);
			OutShadowMap.Resource = nullptr;
		}
		return false;
	}

	if (!BuildSlices(Context, Req, OutShadowMap.Slices))
	{
		if (OutShadowMap.Resource != nullptr)
		{
			Context->ShadowResourcePool->Release(OutShadowMap.Resource);
			OutShadowMap.Resource = nullptr;
		}
		return false;
	}

	OutShadowMap.bOwnsResource = true;
	OutShadowMap.MapType = Desc.MapType;
	OutShadowMap.LightId = Req.LightId;
	OutShadowMap.SourceLightSlotIndex = Context->RenderBus->GetLights()[Req.LightId].SourceLightSlotIndex;
	OutShadowMap.LightType = Req.Type;
	return true;
}

bool FShadowPass::BuildViews(const FRenderPassContext* Context,
                             const FShadowRequest& Req,
                             TArray<FShadowViewInfo>& OutViewInfoArray)
{
    const auto& Lights = Context->RenderBus->GetLights();

    switch (Req.Type)
    {
    case ELightType::LightType_Directional:
    {
        const FCameraState& Cam = Context->RenderBus->GetCameraState();
        const FVector CamPos = Context->RenderBus->GetCameraPosition();
        const FVector CamFwd = Context->RenderBus->GetCameraForward();
        const FVector CamRight = Context->RenderBus->GetCameraRight();
        const FVector CamUp = Context->RenderBus->GetCameraUp();
        const FVector LightDir = Lights[Req.LightId].Direction.GetSafeNormal();

        FVector LightUp = FVector::UpVector;
        if (std::abs(FVector::DotProduct(LightDir, LightUp)) > 0.99f)
            LightUp = FVector::RightVector;

        const float HalfTan = tanf(Cam.FOV * 0.5f);

        for (uint32 i = 0; i < static_cast<uint32>(Req.Cascades.size()); ++i)
        {
            const float Near = Req.Cascades[i].Near;
            const float Far = Req.Cascades[i].Far;

            const float NearH = 2.0f * Near * HalfTan;
            const float NearW = NearH * Cam.AspectRatio;
            const float FarH = 2.0f * Far * HalfTan;
            const float FarW = FarH * Cam.AspectRatio;

            const FVector NC = CamPos + CamFwd * Near;
            const FVector FC = CamPos + CamFwd * Far;

            FVector Corners[8] = {
                NC + CamUp * (NearH * 0.5f) - CamRight * (NearW * 0.5f),
                NC + CamUp * (NearH * 0.5f) + CamRight * (NearW * 0.5f),
                NC - CamUp * (NearH * 0.5f) - CamRight * (NearW * 0.5f),
                NC - CamUp * (NearH * 0.5f) + CamRight * (NearW * 0.5f),
                FC + CamUp * (FarH * 0.5f) - CamRight * (FarW * 0.5f),
                FC + CamUp * (FarH * 0.5f) + CamRight * (FarW * 0.5f),
                FC - CamUp * (FarH * 0.5f) - CamRight * (FarW * 0.5f),
                FC - CamUp * (FarH * 0.5f) + CamRight * (FarW * 0.5f),
            };

            FVector Center = FVector::ZeroVector;
            for (const auto& C : Corners)
                Center += C;
            Center *= (1.0f / 8.0f);

            float Radius = 0.0f;
            for (const auto& C : Corners)
                Radius = std::max(Radius, (C - Center).Size());

            const FVector Eye = Center + LightDir * Radius;
            const FMatrix LightView = FMatrix::MakeViewLookAtLH(Eye, Center, LightUp);

            FVector LS[8];
            for (int j = 0; j < 8; ++j)
                LS[j] = LightView.TransformPosition(Corners[j]);

            FVector LSMin = LS[0], LSMax = LS[0];
            for (int j = 1; j < 8; ++j)
            {
                LSMin.X = std::min(LSMin.X, LS[j].X);
                LSMax.X = std::max(LSMax.X, LS[j].X);
                LSMin.Y = std::min(LSMin.Y, LS[j].Y);
                LSMax.Y = std::max(LSMax.Y, LS[j].Y);
                LSMin.Z = std::min(LSMin.Z, LS[j].Z);
                LSMax.Z = std::max(LSMax.Z, LS[j].Z);
            }

            float NearZ = LSMin.Z;
            float FarZ = LSMax.Z;
            const float Padding = std::max(50.0f, (FarZ - NearZ) * 0.1f);
            NearZ -= Padding;
            FarZ += Padding;
            FarZ = std::max(FarZ, Radius + 1000.0f);
            if (FarZ - NearZ < 1.0f)
            {
                NearZ -= 0.5f;
                FarZ += 0.5f;
            }

			float ViewWidth = Radius * 2;
            float ViewHeight = Radius * 2;

            FShadowViewInfo View;
            View.LightView = LightView;
            View.LightProjection = FMatrix::MakeOrthographicLH(ViewWidth, ViewHeight, NearZ, FarZ);
            View.SplitDepth = Far;
            OutViewInfoArray.push_back(View);
        }
        break;
    }

    case ELightType::LightType_Spot:
    {
        const FRenderLight& Light = Lights[Req.LightId];
        const FVector LightDir = Light.Direction.GetSafeNormal();

        FVector Up = FVector(0.0f, 0.0f, 1.0f);
        if (std::abs(FVector::DotProduct(LightDir, Up)) > 0.99f)
            Up = FVector(1.0f, 0.0f, 0.0f);

        const float FovRad = std::acos(Light.SpotOuterCos) * 2.0f;
        const float NearZ = 0.1f;
        const float FarZ = std::max(Light.Radius, NearZ + 0.1f);

        for (uint32 i = 0; i < static_cast<uint32>(Req.Cascades.size()); ++i)
        {
            FShadowViewInfo View;
            View.LightView = FMatrix::MakeViewLookAtLH(Light.Position, Light.Position + LightDir, Up);
            View.LightProjection = FMatrix::MakePerspectiveFovLH(FovRad, 1.0f, NearZ, FarZ);
            View.SplitDepth = Context->RenderBus->GetCameraState().FarZ;
            OutViewInfoArray.push_back(View);
        }
        break;
    }

    case ELightType::LightType_Point:
    {
        static const FVector CubeDirs[6] = {
            FVector::ForwardVector,
            -FVector::ForwardVector,
            FVector::RightVector,
            -FVector::RightVector,
            FVector::UpVector,
            -FVector::UpVector,
        };
        static const FVector CubeUps[6] = {
            FVector::RightVector,
            FVector::RightVector,
            -FVector::UpVector,
            FVector::UpVector,
            FVector::RightVector,
            FVector::RightVector,
        };

        const FRenderLight& Light = Lights[Req.LightId];
        const float NearZ = 0.1f;
        const float FarZ = std::max(Light.Radius, NearZ + 0.1f);
        const float FovRad = 90.0f * (3.141592f / 180.0f);

        for (uint32 i = 0; i < 6; ++i)
        {
            FShadowViewInfo View;
            View.LightView = FMatrix::MakeViewLookAtLH(Light.Position, Light.Position + CubeDirs[i], CubeUps[i]);
            View.LightProjection = FMatrix::MakePerspectiveFovLH(FovRad, 1.0f, NearZ, FarZ);
            View.SplitDepth = Context->RenderBus->GetCameraState().FarZ;
            OutViewInfoArray.push_back(View);
        }
        break;
    }

    default:
        return false;
    }

    return true;
}

bool FShadowPass::BuildSlices(const FRenderPassContext* Context,
                              const FShadowRequest& Req,
                              TArray<FShadowSlice>& OutShadowSlices)
{
    auto MakeSlice = [](uint32 Idx, EShadowSliceType Type, uint32 LightId) -> FShadowSlice
    {
        FShadowSlice S;
        S.Index = Idx;
        S.Type = Type;
        S.UVOffset = FVector2(0.0f, 0.0f);
        S.UVScale = FVector2(1.0f, 1.0f);
        S.LightId = LightId;
        return S;
    };

    switch (Req.Type)
    {
    case ELightType::LightType_Directional:
        for (uint32 i = 0; i < static_cast<uint32>(Req.Cascades.size()); ++i)
            OutShadowSlices.push_back(MakeSlice(i, EShadowSliceType::CSM, Req.LightId));
        break;

    case ELightType::LightType_Spot:
        for (uint32 i = 0; i < static_cast<uint32>(Req.Cascades.size()); ++i)
            OutShadowSlices.push_back(MakeSlice(i, EShadowSliceType::Atlas, Req.LightId));
        break;

    case ELightType::LightType_Point:
        for (uint32 i = 0; i < 6; ++i)
            OutShadowSlices.push_back(MakeSlice(i, EShadowSliceType::CubeFace, Req.LightId));
        break;

    default:
        return false;
    }

    return true;
}

bool FShadowPass::AcquireResource(const FRenderPassContext* Context,
                                  const FShadowRequestDesc& Desc,
                                  FShadowResource** OutShadowResource)
{
    *OutShadowResource = Context->ShadowResourcePool->Acquire(Context->Device, Desc);
    return (*OutShadowResource != nullptr);
}
