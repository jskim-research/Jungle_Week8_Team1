#include "ShadowPass.h"
#include "Render/Scene/ShadowLightSelector.h"
#include "Core/ResourceManager.h"
#include "Editor/UI/EditorConsoleWidget.h"
#include <algorithm>

#define ATLAS_SIZE 4096
namespace
{
	// 현재 Pass 간 Input, Output 연결 구조가 아니어서 전역으로 놓았는데, 나중에 바꿔야 함
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

		TComPtr<ID3D11ShaderResourceView> DepthArraySRV;
	};
	TArray<FShadowVSMResource> GVSMResources;

	// 1. LightId -> ShadowDataArray Index (0~31) 매핑 테이블
	TArray<int32> GLightToShadowIndices;
	// 2. OpaquePass에 넘겨줄 상수 버퍼 데이터
	FOpaqueRenderPass::FShadowArrayCB GShadowCBData;

	constexpr DXGI_FORMAT GVSMMomentsFormat = DXGI_FORMAT_R32G32_FLOAT;

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
		if (Device == nullptr || Resolution == 0 || SliceCount == 0 ||
			(bCreateCubeSRV && (SliceCount % 6u) != 0u))
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
				   OutResource.DepthArraySRV,
				   OutResource.TempRTVOwners,
				   OutResource.TempRTVs);
	}

	bool CreateVSMCubeResources(ID3D11Device* Device, uint32 Resolution, uint32 CubeCount, FShadowVSMResource& OutResource)
	{
		const uint32 SliceCount = CubeCount * 6u;
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
				   OutResource.DepthArraySRV,
				   OutResource.TempRTVOwners,
				   OutResource.TempRTVs);
	}

	bool CreateDepthArraySRV(
		ID3D11Device* Device,
		ID3D11Texture2D* DepthTexture,
		uint32 SliceCount,
		TComPtr<ID3D11ShaderResourceView>& OutSRV)
	{
		if (Device == nullptr || DepthTexture == nullptr || SliceCount == 0u)
		{
			return false;
		}

		OutSRV.Reset();

		D3D11_SHADER_RESOURCE_VIEW_DESC SRVDesc = {};
		SRVDesc.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
		SRVDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
		SRVDesc.Texture2DArray.MostDetailedMip = 0;
		SRVDesc.Texture2DArray.MipLevels = 1;
		SRVDesc.Texture2DArray.FirstArraySlice = 0;
		SRVDesc.Texture2DArray.ArraySize = SliceCount;

		return SUCCEEDED(Device->CreateShaderResourceView(DepthTexture, &SRVDesc, OutSRV.GetAddressOf()));
	}

	bool HasVSMResources(const FShadowVSMResource& Resource)
	{
		return Resource.MomentsSRV != nullptr &&
			   Resource.TempSRV != nullptr &&
			   !Resource.MomentRTVs.empty() &&
			   !Resource.TempRTVs.empty();
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
			GVSMResources[ShadowMapIndex].MomentsSRV != nullptr)
		{
			return GVSMResources[ShadowMapIndex].MomentsSRV.Get();
		}
	}

	return nullptr;
}

ID3D11ShaderResourceView* FShadowPass::GetVSMCubeShadowSRV()
{
	for (size_t ShadowMapIndex = 0; ShadowMapIndex < GShadowMaps.size() && ShadowMapIndex < GVSMResources.size(); ++ShadowMapIndex)
	{
		if (GShadowMaps[ShadowMapIndex].MapType == EShadowMapType::VSMCube &&
			GVSMResources[ShadowMapIndex].MomentsCubeSRV != nullptr)
		{
			return GVSMResources[ShadowMapIndex].MomentsCubeSRV.Get();
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
	UShader* Shader = FResourceManager::Get().GetShader("Shaders/Primitive.hlsl");
	if (!ShaderBinding || ShaderBinding->GetShader() != Shader)
	{
		ShaderBinding = Shader->CreateBindingInstance(Context->Device);
	}

	if (!ShaderBinding)
	{
		bSkip = true;
		return true;
	}

	if (!GShadowMaps.empty())
	{
		for (FShadowMap& ShadowMap : GShadowMaps)
		{
			if (ShadowMap.bOwnsResource)
			{
				Context->ShadowResourcePool->Release(ShadowMap.Resource);
			}
		}
		GShadowMaps.clear();
	}
	GVSMResources.clear();

	bSkip = false;

	/***************/
	/*  Selection  */
	/***************/
	std::vector<FShadowRequest> ShadowRequests =
		ShadowLightSelector.SelectShadowLights(Context->RenderBus->GetLights(), Context->RenderBus->GetCameraPosition(), Context->RenderBus->GetCameraState());

	if (ShadowRequests.empty())
	{
		bSkip = true;
		return true;
	}

	/****************/
	/*  Allocation  */
	/****************/

	// LightType별로 모아두기 (atlas용)
	std::array<std::vector<FShadowRequest>, static_cast<size_t>(ELightType::Max)> buckets;

	for (auto& req : ShadowRequests)
	{
		buckets[static_cast<int>(req.Type)].push_back(req);
	}
	// 버킷별로 내림차순 정렬
	for (int t = 0; t < static_cast<size_t>(ELightType::Max); ++t)
	{
		std::sort(buckets[t].begin(), buckets[t].end(),
				  [](const FShadowRequest& a, const FShadowRequest& b)
				  {
					  return a.Resolution > b.Resolution; // 무조건 큰 놈부터!
				  });
	}
	// 정렬이 완료된 버킷들을 다시 하나의 순차 배열로 합치기
	ShadowRequests.clear();
	for (int t = 0; t < static_cast<size_t>(ELightType::Max); ++t)
	{
		for (const auto& req : buckets[t])
		{
			ShadowRequests.push_back(req);
		}
	}

	const EShadowFilterMode RequestedShadowFilter = Context->RenderBus->GetShowFlags().ShadowFilter;
	bool bUseVSMFilter = Context->RenderBus->GetShowFlags().UsesVSMShadowFilter();
	if (bUseVSMFilter && !EnsureVSMBindings(Context))
	{
		bUseVSMFilter = false;
	}

	for (FShadowRequest& ShadowRequest : ShadowRequests)
	{
		ShadowRequest.bUseVSM =
			bUseVSMFilter &&
			(ShadowRequest.Type == ELightType::LightType_Directional ||
			 ShadowRequest.Type == ELightType::LightType_Point);
	}

	// 원본 인덱스 저장한 룩업테이블
	struct FLightShadowMappingInfo
	{
		bool bHasShadow = false;
		uint32 ShadowMapIndex = 0; // GShadowMaps 배열에서의 인덱스
		uint32 SliceIndex = 0;     // 해당 ShadowMap 내부 Slices 배열에서의 인덱스
	};

	// 원본 라이트 개수만큼 매핑 테이블 할당
	TArray<FLightShadowMappingInfo> ShadowLookupTable(Context->RenderBus->GetLights().size());

	GLightToShadowIndices.assign(Context->RenderBus->GetLights().size(), -1);
	std::memset(&GShadowCBData, 0, sizeof(GShadowCBData));

	uint32 ShadowIndexCounter = 0; // GPU 버퍼 배열에 들어갈 인덱스 (0 ~ 31)
	uint32 PointShadowTextureIndexCounter = 0;

	AtlasAllocator.Reset();

	FShadowResource* SharedPointShadowResource = nullptr;
	FShadowVSMResource SharedPointVSMResource = {};
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
			if (!CreateVSMCubeResources(Context->Device, SharedPointShadowResolution, PointArrayDesc.CubeCount, SharedPointVSMResource) ||
				!CreateDepthArraySRV(
					Context->Device,
					SharedPointShadowResource->BackingResource.Texture.Get(),
					PointSliceCount,
					SharedPointVSMResource.DepthArraySRV))
			{
				SharedPointVSMResource = FShadowVSMResource{};
			}
		}
	}

	// 기존의 범위 기반 for 문을 인덱스 기반으로 교체
	for (int i = 0; i < ShadowRequests.size(); ++i)
	{
		const FShadowRequest& ShadowRequest = ShadowRequests[i];
		const FRenderLight& ShadowLight = Context->RenderBus->GetLights()[ShadowRequest.LightId];

		if (ShadowIndexCounter >= MAX_SHADOW_LIGHTS)
			break;

		if (ShadowRequest.Type == ELightType::LightType_Point)
		{
			if (SharedPointShadowResource == nullptr)
			{
				continue;
			}

			FShadowMap ShadowMap;
			ShadowMap.Resource = SharedPointShadowResource;
			ShadowMap.MapType =
				(ShadowRequest.bUseVSM && HasVSMResources(SharedPointVSMResource) && SharedPointVSMResource.MomentsCubeSRV != nullptr)
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
				}
				continue;
			}

			GShadowMaps.push_back(ShadowMap);
			GVSMResources.push_back(ShadowMap.MapType == EShadowMapType::VSMCube ? SharedPointVSMResource : FShadowVSMResource{});
			GLightToShadowIndices[ShadowRequest.LightId] = ShadowIndexCounter;

			GShadowCBData.ShadowDataArray[ShadowIndexCounter].UVOffset = FVector2(0, 0);
			GShadowCBData.ShadowDataArray[ShadowIndexCounter].UVScale = FVector2(1, 1);
			GShadowCBData.ShadowDataArray[ShadowIndexCounter].ShadowLightPosition = ShadowLight.Position;
			GShadowCBData.ShadowDataArray[ShadowIndexCounter].ShadowFar = std::max(ShadowLight.Radius, 0.1f);
			GShadowCBData.ShadowDataArray[ShadowIndexCounter].ShadowBias = ComputeShadowCompareBias(ShadowLight);
			GShadowCBData.ShadowDataArray[ShadowIndexCounter].ShadowSlopeBias = ShadowLight.ShadowSlopeBias;
			GShadowCBData.ShadowDataArray[ShadowIndexCounter].ShadowFilterScale = ComputeShadowFilterScale(ShadowLight);
			GShadowCBData.ShadowDataArray[ShadowIndexCounter].ShadowMapType = static_cast<uint32>(ShadowMap.MapType);
			GShadowCBData.ShadowDataArray[ShadowIndexCounter].SliceCount = 1;
			GShadowCBData.ShadowDataArray[ShadowIndexCounter].ShadowTextureIndex = PointShadowTextureIndexCounter;
			ApplyShadowFilterMode(GShadowCBData.ShadowDataArray[ShadowIndexCounter], RequestedShadowFilter, ShadowMap.MapType);
			GShadowCBData.ShadowDataArray[ShadowIndexCounter].PointShadowTexelSize =
				2.0f / std::max<float>(static_cast<float>(SharedPointShadowResource->Resolution), 1.0f);
			ApplyVSMParameters(GShadowCBData.ShadowDataArray[ShadowIndexCounter], ShadowLight, ShadowMap.MapType);

			SharedPointShadowFaceOffset += 6;
			++PointShadowTextureIndexCounter;
			++ShadowIndexCounter;
		}
		else if (ShadowRequest.Type == ELightType::LightType_Spot)
		{
			// 1. 공간 할당 가능?
			FAtlasAllocationResult AllocResult;
			if (!AtlasAllocator.Allocate(ShadowRequest.Resolution, AllocResult))
			{
				// 공간 부족 시 새 아틀라스 생성 후 재할당
				FShadowRequestDesc Desc;
				Desc.AllocationMode = EShadowAllocationMode::AtlasPacked;
				Desc.MapType = EShadowMapType::Depth2D;
				Desc.Resolution = ATLAS_SIZE;
				Desc.CascadeCount = static_cast<uint32>(ShadowRequest.Cascades.size());

				FShadowResource* NewAtlasRes = nullptr;
				if (AcquireResource(Context, Desc, &NewAtlasRes))
				{
					AtlasAllocator.AddNewAtlasResource(NewAtlasRes);

					FShadowMap NewAtlasMap;
					NewAtlasMap.Resource = NewAtlasRes;
					NewAtlasMap.MapType = EShadowMapType::Depth2D;
					NewAtlasMap.LightType = ELightType::LightType_Spot;
					GShadowMaps.push_back(NewAtlasMap);
					GVSMResources.emplace_back();

					uint32 NewAtlasIndex = static_cast<uint32>(GShadowMaps.size() - 1);
					AtlasAllocator.SetCurrentAtlasIndex(NewAtlasIndex);
					AtlasAllocator.Allocate(ShadowRequest.Resolution, AllocResult);
				}
			}
			uint32 AtlasIndex = AtlasAllocator.GetCurrentAtlasIndex();
			FShadowMap& CurrentAtlasMap = GShadowMaps[AtlasIndex];

			// 뷰 추가 (배열 맨 뒤에 push_back 됨)
			BuildViews(Context, ShadowRequest, CurrentAtlasMap.Views);

			// 아틀라스 전용 UV 슬라이스 추가
			FShadowSlice Slice;
			Slice.Index = 0;
			Slice.Type = EShadowSliceType::Atlas;
			Slice.UVOffset = AllocResult.UVOffset;
			Slice.UVScale = AllocResult.UVScale;
			Slice.LightId = ShadowRequest.LightId;
			Slice.SourceLightSlotIndex = ShadowLight.SourceLightSlotIndex;
			CurrentAtlasMap.Slices.push_back(Slice);

			// ★ 방금 추가된 View와 Slice의 실제 인덱스 추출 (맨 마지막 위치)
            uint32 CurrentViewIndex = static_cast<uint32>(CurrentAtlasMap.Views.size() - 1);
            uint32 CurrentSliceIndex = static_cast<uint32>(CurrentAtlasMap.Slices.size() - 1);

			// 매핑 테이블 기록
			FLightShadowMappingInfo& MappingInfo = ShadowLookupTable[ShadowRequest.LightId];
			MappingInfo.bHasShadow = true;
			MappingInfo.ShadowMapIndex = AtlasIndex;
			MappingInfo.SliceIndex = CurrentSliceIndex; // 계산된 슬라이스 인덱스 사용

			// 현재 LightId가 몇 번째 ShadowIndex를 쓰는지 기록
			GLightToShadowIndices[ShadowRequest.LightId] = ShadowIndexCounter;

			// ★ 2. GPU에 넘길 상수 버퍼 데이터를 여기서 싹 다 채워버림!
			// [수정됨] Views[0] 대신 방금 추가된 Views[CurrentViewIndex]를 참조합니다!
			GShadowCBData.ShadowDataArray[ShadowIndexCounter].ShadowLightView[0] = CurrentAtlasMap.Views[CurrentViewIndex].LightView;
			GShadowCBData.ShadowDataArray[ShadowIndexCounter].ShadowLightProjection[0] = CurrentAtlasMap.Views[CurrentViewIndex].LightProjection;
			GShadowCBData.ShadowDataArray[ShadowIndexCounter].UVOffset = AllocResult.UVOffset;
			GShadowCBData.ShadowDataArray[ShadowIndexCounter].UVScale = AllocResult.UVScale;
			GShadowCBData.ShadowDataArray[ShadowIndexCounter].ShadowBias = ComputeShadowCompareBias(ShadowLight);
			GShadowCBData.ShadowDataArray[ShadowIndexCounter].ShadowSlopeBias = ShadowLight.ShadowSlopeBias;
			GShadowCBData.ShadowDataArray[ShadowIndexCounter].ShadowFilterScale = ComputeShadowFilterScale(ShadowLight);
			GShadowCBData.ShadowDataArray[ShadowIndexCounter].ShadowMapType = static_cast<uint32>(CurrentAtlasMap.MapType);
			GShadowCBData.ShadowDataArray[ShadowIndexCounter].SliceCount = 1;
			GShadowCBData.ShadowDataArray[ShadowIndexCounter].ShadowTextureIndex = 1u;
			ApplyShadowFilterMode(GShadowCBData.ShadowDataArray[ShadowIndexCounter], RequestedShadowFilter, CurrentAtlasMap.MapType);
			GShadowCBData.ShadowDataArray[ShadowIndexCounter].PointShadowTexelSize = 0.0f;
			ApplyVSMParameters(GShadowCBData.ShadowDataArray[ShadowIndexCounter], ShadowLight, CurrentAtlasMap.MapType);

			ShadowIndexCounter++;
		}
		else
		{
			FShadowMap ShadowMap;
			if (MakeShadowMap(Context, ShadowRequest, ShadowMap))
			{
				FShadowVSMResource VSMResource;
				if (ShadowRequest.bUseVSM)
				{
					const uint32 SliceCount = static_cast<uint32>(ShadowMap.Views.size());
					if (!CreateVSMResources(
							Context->Device,
							ShadowMap.Resource ? ShadowMap.Resource->Resolution : 0u,
							SliceCount,
							VSMResource))
					{
						ShadowMap.MapType = EShadowMapType::Depth2D;
						VSMResource = FShadowVSMResource{};
					}
				}

				GShadowMaps.push_back(ShadowMap);
				GVSMResources.push_back(std::move(VSMResource));

				GLightToShadowIndices[ShadowRequest.LightId] = ShadowIndexCounter;

				for (size_t i = 0; i < ShadowRequest.Cascades.size(); i++)
				{
					GShadowCBData.ShadowDataArray[ShadowIndexCounter].ShadowLightView[i] = ShadowMap.Views[i].LightView;
					GShadowCBData.ShadowDataArray[ShadowIndexCounter].ShadowLightProjection[i] = ShadowMap.Views[i].LightProjection;
					GShadowCBData.ShadowDataArray[ShadowIndexCounter].CascadeSplits[i] = ShadowRequest.Cascades[i].Far;
				}

				GShadowCBData.ShadowDataArray[ShadowIndexCounter].UVOffset = FVector2(0, 0);
				GShadowCBData.ShadowDataArray[ShadowIndexCounter].UVScale = FVector2(1, 1);
				GShadowCBData.ShadowDataArray[ShadowIndexCounter].ShadowLightPosition = ShadowLight.Position;
				GShadowCBData.ShadowDataArray[ShadowIndexCounter].ShadowFar =
					std::max(ShadowLight.Radius, 0.1f);
				GShadowCBData.ShadowDataArray[ShadowIndexCounter].ShadowBias = ComputeShadowCompareBias(ShadowLight);
				GShadowCBData.ShadowDataArray[ShadowIndexCounter].ShadowSlopeBias = ShadowLight.ShadowSlopeBias;
				GShadowCBData.ShadowDataArray[ShadowIndexCounter].ShadowFilterScale = ComputeShadowFilterScale(ShadowLight);
				GShadowCBData.ShadowDataArray[ShadowIndexCounter].ShadowMapType = static_cast<uint32>(ShadowMap.MapType);
                GShadowCBData.ShadowDataArray[ShadowIndexCounter].SliceCount = static_cast<uint32>(ShadowRequest.Cascades.size());
				GShadowCBData.ShadowDataArray[ShadowIndexCounter].ShadowTextureIndex = 0u;
				ApplyShadowFilterMode(GShadowCBData.ShadowDataArray[ShadowIndexCounter], RequestedShadowFilter, ShadowMap.MapType);
				GShadowCBData.ShadowDataArray[ShadowIndexCounter].PointShadowTexelSize = 0.0f;
				ApplyVSMParameters(GShadowCBData.ShadowDataArray[ShadowIndexCounter], ShadowLight, ShadowMap.MapType);

				ShadowIndexCounter++;
			}
		}
	}

	if (SharedPointShadowResource != nullptr && PointShadowTextureIndexCounter == 0)
	{
		Context->ShadowResourcePool->Release(SharedPointShadowResource);
		SharedPointShadowResource = nullptr;
	}

	if (GShadowMaps.empty())
	{
		bSkip = true;
		return true;
	}

	OutSRV = GShadowMaps[0].Resource->SRV;
	OutRTV = nullptr;

	ShaderBinding->ApplyFrameParameters(*Context->RenderBus);

	return true;
}

bool FShadowPass::DrawCommand(const FRenderPassContext* Context)
{
	if (bSkip || !ShaderBinding)
		return true;

	const FRenderBus* RenderBus = Context->RenderBus;

	const TArray<FRenderCommand>& Commands = RenderBus->GetCommands(ERenderPass::Opaque);

	if (Commands.empty())
		return true;

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

	// Shadow rendering must not inherit read-only depth or translucent blend state from the previous frame.
	Context->DeviceContext->OMSetDepthStencilState(DefaultDepthStencilState, 0);
	Context->DeviceContext->OMSetBlendState(OpaqueBlendState, nullptr, 0xFFFFFFFF);

	auto DrawShadowCommands = [&](const FShadowViewInfo& ViewInfo, const FRenderLight* ShadowLight = nullptr) -> bool
	{
		ShaderBinding->SetMatrix4("View", ViewInfo.LightView);
		ShaderBinding->SetMatrix4("Projection", ViewInfo.LightProjection);

		for (const FRenderCommand& Cmd : Commands)
		{
			if (Cmd.Type == ERenderCommandType::PostProcessOutline)
			{
				continue;
			}

			if (ShadowLight != nullptr && Cmd.WorldBounds.IsValid())
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

			uint32 Offset = 0;
			ID3D11Buffer* VertexBuffer = Cmd.MeshBuffer->GetVertexBuffer().GetBuffer();
			if (VertexBuffer == nullptr)
			{
				return false;
			}

			const uint32 VertexCount = Cmd.MeshBuffer->GetVertexBuffer().GetVertexCount();
			const uint32 Stride = Cmd.MeshBuffer->GetVertexBuffer().GetStride();
			if (VertexCount == 0 || Stride == 0)
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

			Context->DeviceContext->IASetVertexBuffers(0, 1, &VertexBuffer, &Stride, &Offset);

			ID3D11Buffer* IndexBuffer = Cmd.MeshBuffer->GetIndexBuffer().GetBuffer();
			if (IndexBuffer != nullptr)
			{
				const uint32 IndexStart = Cmd.SectionIndexStart;
				const uint32 IndexCount = Cmd.SectionIndexCount;
				Context->DeviceContext->IASetIndexBuffer(IndexBuffer, DXGI_FORMAT_R32_UINT, 0);
				Context->DeviceContext->DrawIndexed(IndexCount, IndexStart, 0);
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
			HasVSMResources(GVSMResources[ShadowMapIndex]) &&
			PointVSMShaderBinding != nullptr;

		if (bPointVSM)
		{
			FShadowVSMResource& VSMResource = GVSMResources[ShadowMapIndex];
			const FRenderLight* DrawShadowLight =
				ShadowMap.LightId < static_cast<uint32>(Context->RenderBus->GetLights().size())
					? &Context->RenderBus->GetLights()[ShadowMap.LightId]
					: nullptr;
			if (DrawShadowLight == nullptr)
			{
				continue;
			}

			const uint32 DrawSliceCount = std::min<uint32>(
				static_cast<uint32>(ShadowMap.Views.size()),
				std::min<uint32>(
					static_cast<uint32>(ShadowMap.Resource->DSVs.size() > ShadowMap.ResourceSliceOffset ? ShadowMap.Resource->DSVs.size() - ShadowMap.ResourceSliceOffset : 0),
					static_cast<uint32>(VSMResource.MomentRTVs.size() > ShadowMap.ResourceSliceOffset ? VSMResource.MomentRTVs.size() - ShadowMap.ResourceSliceOffset : 0)));
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

				PointVSMShaderBinding->ApplyFrameParameters(*Context->RenderBus);
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

				if (!DrawShadowCommands(ShadowMap.Views[SliceIndex]))
				{
					Context->DeviceContext->RSSetViewports(OldVPCount, OldVP);
					return false;
				}
			}

			continue;
		}

		const uint32 DrawSliceCount = std::min<uint32>(
			static_cast<uint32>(ShadowMap.Resource->DSVs.size() > ShadowMap.ResourceSliceOffset ? ShadowMap.Resource->DSVs.size() - ShadowMap.ResourceSliceOffset : 0),
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

		for (uint32 ViewIndex = 0; ViewIndex < DrawSliceCount; ++ViewIndex)
		{
			const FRenderLight* DrawShadowLight = nullptr;
			if (ShadowMap.LightType == ELightType::LightType_Point &&
				ShadowMap.LightId < static_cast<uint32>(Context->RenderBus->GetLights().size()))
			{
				DrawShadowLight = &Context->RenderBus->GetLights()[ShadowMap.LightId];
			}

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
		return true;

	bool bHasVSMWork = false;
	for (size_t ShadowMapIndex = 0; ShadowMapIndex < GShadowMaps.size() && ShadowMapIndex < GVSMResources.size(); ++ShadowMapIndex)
	{
		if ((GShadowMaps[ShadowMapIndex].MapType == EShadowMapType::VSM2D ||
			 GShadowMaps[ShadowMapIndex].MapType == EShadowMapType::VSMCube) &&
			HasVSMResources(GVSMResources[ShadowMapIndex]))
		{
			bHasVSMWork = true;
			break;
		}
	}

	if (!bHasVSMWork)
	{
		return true;
	}

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
		FShadowVSMResource& VSMResource = GVSMResources[ShadowMapIndex];
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
			std::min<uint32>(
				AvailableMomentSlices,
				AvailableTempSlices));
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
				// The moments texture is filterable color data, so these fullscreen writes must be pure overwrites.
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
	FShadowRequestDesc Desc;
	Desc.AllocationMode = EShadowAllocationMode::ArrayBased; // CSM
	Desc.MapType =
		Req.Type == ELightType::LightType_Point
			? (Req.bUseVSM ? EShadowMapType::VSMCube : EShadowMapType::DepthCube)
			: (Req.bUseVSM ? EShadowMapType::VSM2D : EShadowMapType::Depth2D);
	Desc.Resolution = Req.Resolution;
    Desc.CascadeCount = static_cast<uint32>(Req.Cascades.size());
	Desc.CubeCount = (Req.Type == ELightType::LightType_Point) ? 1u : 0u;

	if (!AcquireResource(Context, Desc, &OutShadowMap.Resource))
		return false;
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
	OutShadowMap.MapType = Desc.MapType;
	OutShadowMap.LightId = Req.LightId;
	OutShadowMap.SourceLightSlotIndex = Context->RenderBus->GetLights()[Req.LightId].SourceLightSlotIndex;
	OutShadowMap.LightType = Req.Type;

	return true;
}

bool FShadowPass::BuildViews(const FRenderPassContext* Context, const FShadowRequest& Req, TArray<FShadowViewInfo>& OutViewInfoArray)
{
	switch (Req.Type)
	{
	case ELightType::LightType_Directional:
		for (uint32 i = 0; i < Req.Cascades.size(); ++i)
		{
			// gather camera frustum corners in world space
			const FCameraState& Cam = Context->RenderBus->GetCameraState();
			const FVector CamPos = Context->RenderBus->GetCameraPosition();
			const FVector CamForward = Context->RenderBus->GetCameraForward();
			const FVector CamRight = Context->RenderBus->GetCameraRight();
			const FVector CamUp = Context->RenderBus->GetCameraUp();

			const float Near = Req.Cascades[i].Near;
			const float Far = Req.Cascades[i].Far;
			const float HalfTan = tanf(Cam.FOV * 0.5f);

			const float NearH = 2.0f * Near * HalfTan;
			const float NearW = NearH * Cam.AspectRatio;
			const float FarH = 2.0f * Far * HalfTan;
			const float FarW = FarH * Cam.AspectRatio;

			const FVector NearCenter = CamPos + CamForward * Near;
			const FVector FarCenter = CamPos + CamForward * Far;

			FVector FrustumCorners[8] = {
				NearCenter + CamUp * (NearH * 0.5f) - CamRight * (NearW * 0.5f),
				NearCenter + CamUp * (NearH * 0.5f) + CamRight * (NearW * 0.5f),
				NearCenter - CamUp * (NearH * 0.5f) - CamRight * (NearW * 0.5f),
				NearCenter - CamUp * (NearH * 0.5f) + CamRight * (NearW * 0.5f),
				FarCenter + CamUp * (FarH * 0.5f) - CamRight * (FarW * 0.5f),
				FarCenter + CamUp * (FarH * 0.5f) + CamRight * (FarW * 0.5f),
				FarCenter - CamUp * (FarH * 0.5f) - CamRight * (FarW * 0.5f),
				FarCenter - CamUp * (FarH * 0.5f) + CamRight * (FarW * 0.5f)
			};

			// compute frustum center and radius
			FVector FrustumCenter = FVector::ZeroVector;
			for (int j = 0; j < 8; ++j) FrustumCenter += FrustumCorners[j];
			FrustumCenter *= (1.0f / 8.0f);

			float AABBRadius = 0.0f;
			for (int j = 0; j < 8; ++j) AABBRadius = std::max(AABBRadius, (FrustumCorners[j] - FrustumCenter).Size());

			// light basis
			FRenderLight Light = Context->RenderBus->GetLights()[Req.LightId];
			const FVector LightDir = Light.Direction.GetSafeNormal();
			FVector Up = FVector::UpVector;
			if (std::abs(FVector::DotProduct(LightDir, Up)) > 0.99f) Up = FVector::RightVector;

			// place light-eye and build view
			const FVector Eye = FrustumCenter + LightDir * AABBRadius;
			FMatrix LightView = FMatrix::MakeViewLookAtLH(Eye, FrustumCenter, Up);

			// transform frustum into light space and compute AABB
			FVector FrustumLS[8];
			for (int j = 0; j < 8; ++j) FrustumLS[j] = LightView.TransformPosition(FrustumCorners[j]);

			FVector Min = FrustumLS[0];
			FVector Max = FrustumLS[0];
			for (int j = 1; j < 8; ++j)
			{
				Min.X = std::min(Min.X, FrustumLS[j].X);
				Min.Y = std::min(Min.Y, FrustumLS[j].Y);
				Min.Z = std::min(Min.Z, FrustumLS[j].Z);
				Max.X = std::max(Max.X, FrustumLS[j].X);
				Max.Y = std::max(Max.Y, FrustumLS[j].Y);
				Max.Z = std::max(Max.Z, FrustumLS[j].Z);
			}

			// Near/Far from light-space Z extents with padding and safety fallback
			float NearZ = Min.Z;
			float FarZ = Max.Z;
			const float Padding = std::max(50.0f, (FarZ - NearZ) * 0.1f);
			NearZ -= Padding; FarZ += Padding;
			if (FarZ - NearZ < 1.0f)
			{
				float center = (NearZ + FarZ) * 0.5f;
				NearZ = center - 0.5f; FarZ = center + 0.5f;
			}
			const float FallbackFar = AABBRadius + 1000.0f;
			if (FarZ < FallbackFar) FarZ = FallbackFar;

			// build projection
			FShadowViewInfo ViewInfo;
			ViewInfo.LightView = LightView;
			ViewInfo.LightProjection = FMatrix::MakeOrthographicLH(Max.X - Min.X, Max.Y - Min.Y, NearZ, FarZ);
			ViewInfo.SplitDepth = Far;
			OutViewInfoArray.push_back(ViewInfo);
		}
		break;

	case ELightType::LightType_Spot:
		for (uint32 i = 0; i < Req.Cascades.size(); i++)
		{
			FRenderLight Light = Context->RenderBus->GetLights()[Req.LightId];
			FShadowViewInfo ViewInfo;

			FVector LightDir = Light.Direction; // normalize 되어 있어야 함

			FVector Eye = Light.Position;
			FVector Target = Eye + Light.Direction;
			FVector Up = FVector(0, 0, 1);

			if (abs(FVector::DotProduct(LightDir, Up)) > 0.99f)
			{
				Up = FVector(1, 0, 0); // X-Forward니까 X로 대체
			}

			ViewInfo.LightView = FMatrix::MakeViewLookAtLH(Eye, Target, Up);
			ViewInfo.SplitDepth = Context->RenderBus->GetCameraState().FarZ;

			float OuterAngleRad = acos(Light.SpotOuterCos); // 반각(half angle)
			float FovRad = OuterAngleRad * 2.0f;            // 전체 FOV

			float NearZ = 0.1f;
			float FarZ = std::max(Light.Radius, NearZ + 0.1f);

			ViewInfo.LightProjection = FMatrix::MakePerspectiveFovLH(
				FovRad,
				1.0f,        // 정사각형 섀도우 맵
				NearZ,        // Near
				FarZ // Far = 라이트 반경
			);

			OutViewInfoArray.push_back(ViewInfo);
		}
		break;

	case ELightType::LightType_Point:
	{
		// Engine basis: +X forward, +Y right, +Z up.
		// These up vectors must match D3D TextureCube face orientation.
		// Using world-up for the lateral faces mirrors the sampled cubemap shadow onto the wrong side.
		static const FVector CubeDirs[6] = {
			FVector::ForwardVector,
			-FVector::ForwardVector,
			FVector::RightVector,
			-FVector::RightVector,
			FVector::UpVector,
			-FVector::UpVector
		};
		static const FVector CubeUps[6] = {
			FVector::RightVector,
			FVector::RightVector,
			-FVector::UpVector,
			FVector::UpVector,
			FVector::RightVector,
			FVector::RightVector
		};

		for (uint32 i = 0; i < 6; i++)
		{
			FRenderLight Light = Context->RenderBus->GetLights()[Req.LightId];
			FShadowViewInfo ViewInfo;

			FVector LightDir = CubeDirs[i];

			FVector Eye = Light.Position;
			FVector Target = Eye + LightDir;
			FVector Up = CubeUps[i];

			ViewInfo.LightView = FMatrix::MakeViewLookAtLH(Eye, Target, Up);
			ViewInfo.SplitDepth = Context->RenderBus->GetCameraState().FarZ;

			float FovRad = (90.0f * (3.141592f / 180.0f)); // 전체 FOV

			float NearZ = 0.1f;
			float FarZ = std::max(Light.Radius, NearZ + 0.1f);

			ViewInfo.LightProjection = FMatrix::MakePerspectiveFovLH(
				FovRad,
				1.0f,        // 정사각형 섀도우 맵
				NearZ,        // Near
				FarZ // Far = 라이트 반경
			);

			OutViewInfoArray.push_back(ViewInfo);
		}
		break;
	}
		
	default:
		return false;
	}
	
	return true;
}

bool FShadowPass::BuildSlices(const FRenderPassContext* Context, const FShadowRequest& Req, TArray<FShadowSlice>& OutShadowSlices)
{
	switch (Req.Type)
	{
	case ELightType::LightType_Directional:
		for (uint32 i = 0; i < Req.Cascades.size(); i++)
		{
			FShadowSlice ShadowSlice;
			ShadowSlice.Index = i;
			ShadowSlice.Type = EShadowSliceType::CSM;
			ShadowSlice.UVOffset = FVector2(0, 0);
			ShadowSlice.UVScale = FVector2(1, 1);
			OutShadowSlices.push_back(ShadowSlice);
		}
		break;

	case ELightType::LightType_Spot:
		for (uint32 i = 0; i < Req.Cascades.size(); i++)
		{
			FShadowSlice ShadowSlice;
			ShadowSlice.Index = i;
			ShadowSlice.Type = EShadowSliceType::Atlas;
			ShadowSlice.UVOffset = FVector2(0, 0);
			ShadowSlice.UVScale = FVector2(1, 1);
			OutShadowSlices.push_back(ShadowSlice);
		}
		break;
	case ELightType::LightType_Point:
		// Point Light 는 CSM 고려 X
		for (uint32 i = 0; i < 6; i++)
		{
			FShadowSlice ShadowSlice;
			ShadowSlice.Index = i;
			ShadowSlice.Type = EShadowSliceType::CubeFace;
			ShadowSlice.UVOffset = FVector2(0, 0);
			ShadowSlice.UVScale = FVector2(1, 1);
			OutShadowSlices.push_back(ShadowSlice);
		}
		break;
	default:
		return false;
	}

	return true;
}

bool FShadowPass::AcquireResource(const FRenderPassContext* Context, const FShadowRequestDesc& Desc, FShadowResource** OutShadowResource)
{
	*OutShadowResource = Context->ShadowResourcePool->Acquire(Context->Device, Desc);
	return *OutShadowResource != nullptr;
}
