#include "ShadowPass.h"
#include "Core/Logging/GPUProfiler.h"
#include "Core/Logging/Stats.h"
#include "Render/Scene/ShadowLightSelector.h"
#include "Core/ResourceManager.h"
#include "Editor/UI/EditorConsoleWidget.h"
#include "GameFramework/AActor.h"
#include "GameFramework/World.h"
#include "Render/Common/PSMCalculator.h"
#include "Component/Light/LightComponent.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <unordered_set>
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

	struct FShadowFrameCapture
	{
		TArray<FShadowMap> ShadowMaps;
		TArray<FShadowVSMResource*> VSMResources;
		TArray<int32> LightToShadowIndices;
		FOpaqueRenderPass::FShadowArrayCB ShadowCBData = {};
		bool bValid = false;
	};

	TArray<FShadowFrameCapture> GShadowFrameCaptures;

	struct FResolvedShadowProjectionState
	{
		EShadowProjectionMode Mode = EShadowProjectionMode::Standard;
		FMatrix Matrix = FMatrix::Identity;
	};

	TArray<FResolvedShadowProjectionState> GResolvedProjectionStates;

	enum class EPSMInvalidReason : uint8
	{
		None,
		MissingContext,
		NoReceivers,
		InvalidCamera,
		InvalidVirtualCamera,
		DirectionalPostPerspectiveFitFailed,
		InvalidSpotBasis,
		SpotReceiverFitFailed,
		UnsupportedLightType,
		NonFiniteMatrix
	};

	struct FPSMBuildDiagnostics
	{
		EPSMInvalidReason Reason = EPSMInvalidReason::None;
		float Near = 0.0f;
		float Far = 0.0f;
		float Aux0 = 0.0f;
		float Aux1 = 0.0f;
		int32 PointCount = 0;
	};

	struct FPSMFallbackLogState
	{
		bool bWasFallback = false;
		EPSMInvalidReason Reason = EPSMInvalidReason::None;
	};

	TArray<FPSMFallbackLogState> GPSMFallbackLogStates;

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

	const TArray<FRenderCommand>& GetShadowCasterCommands(const FRenderBus& RenderBus)
	{
		const TArray<FRenderCommand>& ShadowCommands = RenderBus.GetCommands(ERenderPass::Shadow);
		return !ShadowCommands.empty() ? ShadowCommands : RenderBus.GetCommands(ERenderPass::Opaque);
	}

	const TArray<FRenderCommand>& GetPSMReceiverCommands(const FRenderBus& RenderBus)
	{
		const TArray<FRenderCommand>& OpaqueCommands = RenderBus.GetCommands(ERenderPass::Opaque);
		return !OpaqueCommands.empty() ? OpaqueCommands : GetShadowCasterCommands(RenderBus);
	}

	const char* GetLightTypeName(ELightType LightType)
	{
		switch (LightType)
		{
		case ELightType::LightType_Directional:
			return "Directional";
		case ELightType::LightType_Spot:
			return "Spot";
		case ELightType::LightType_Point:
			return "Point";
		default:
			return "Unknown";
		}
	}

		const char* GetPSMInvalidReasonName(EPSMInvalidReason Reason)
		{
			switch (Reason)
			{
		case EPSMInvalidReason::None:
			return "None";
		case EPSMInvalidReason::MissingContext:
			return "MissingContext";
		case EPSMInvalidReason::NoReceivers:
			return "NoReceivers";
		case EPSMInvalidReason::InvalidCamera:
			return "InvalidCamera";
		case EPSMInvalidReason::InvalidVirtualCamera:
			return "InvalidVirtualCamera";
		case EPSMInvalidReason::DirectionalPostPerspectiveFitFailed:
			return "DirectionalPostPerspectiveFitFailed";
		case EPSMInvalidReason::InvalidSpotBasis:
			return "InvalidSpotBasis";
		case EPSMInvalidReason::SpotReceiverFitFailed:
			return "SpotReceiverFitFailed";
		case EPSMInvalidReason::UnsupportedLightType:
			return "UnsupportedLightType";
		case EPSMInvalidReason::NonFiniteMatrix:
			return "NonFiniteMatrix";
			default:
				return "Unknown";
			}
		}

		bool IsAtlasShadowMap(const FShadowMap& ShadowMap)
		{
			return ShadowMap.MapType == EShadowMapType::Depth2D &&
				   !ShadowMap.Slices.empty() &&
				   ShadowMap.Slices[0].Type == EShadowSliceType::Atlas;
		}

		void CacheShadowFrameCapture(const FRenderPassContext* Context)
		{
			if (Context == nullptr || Context->ViewportIndex < 0)
			{
				return;
			}

			const size_t CaptureIndex = static_cast<size_t>(Context->ViewportIndex);
			if (GShadowFrameCaptures.size() <= CaptureIndex)
			{
				GShadowFrameCaptures.resize(CaptureIndex + 1);
			}

			FShadowFrameCapture& Capture = GShadowFrameCaptures[CaptureIndex];
			Capture.ShadowMaps = GShadowMaps;
			Capture.VSMResources = GVSMResources;
			Capture.LightToShadowIndices = GLightToShadowIndices;
			Capture.ShadowCBData = GShadowCBData;
			Capture.bValid = true;
		}

		const FShadowFrameCapture* GetShadowFrameCapture(int32 ViewportIndex)
		{
			if (ViewportIndex < 0)
			{
				return nullptr;
			}

			const size_t CaptureIndex = static_cast<size_t>(ViewportIndex);
			if (CaptureIndex >= GShadowFrameCaptures.size())
			{
				return nullptr;
			}

			return GShadowFrameCaptures[CaptureIndex].bValid ? &GShadowFrameCaptures[CaptureIndex] : nullptr;
		}

		uint32 GetEffectiveMipCount(const D3D11_TEXTURE2D_DESC& Desc)
		{
			if (Desc.MipLevels != 0)
			{
				return Desc.MipLevels;
			}

			uint32 Width = std::max(Desc.Width, 1u);
			uint32 Height = std::max(Desc.Height, 1u);
			uint32 MipCount = 1;
			while (Width > 1u || Height > 1u)
			{
				Width = std::max(Width >> 1u, 1u);
				Height = std::max(Height >> 1u, 1u);
				++MipCount;
			}

			return MipCount;
		}

		bool GetBlockCompressionInfo(DXGI_FORMAT Format, uint32& OutBlockWidth, uint32& OutBlockHeight, uint32& OutBytesPerBlock)
		{
			OutBlockWidth = 4u;
			OutBlockHeight = 4u;

			switch (Format)
			{
			case DXGI_FORMAT_BC1_TYPELESS:
			case DXGI_FORMAT_BC1_UNORM:
			case DXGI_FORMAT_BC1_UNORM_SRGB:
			case DXGI_FORMAT_BC4_TYPELESS:
			case DXGI_FORMAT_BC4_UNORM:
			case DXGI_FORMAT_BC4_SNORM:
				OutBytesPerBlock = 8u;
				return true;

			case DXGI_FORMAT_BC2_TYPELESS:
			case DXGI_FORMAT_BC2_UNORM:
			case DXGI_FORMAT_BC2_UNORM_SRGB:
			case DXGI_FORMAT_BC3_TYPELESS:
			case DXGI_FORMAT_BC3_UNORM:
			case DXGI_FORMAT_BC3_UNORM_SRGB:
			case DXGI_FORMAT_BC5_TYPELESS:
			case DXGI_FORMAT_BC5_UNORM:
			case DXGI_FORMAT_BC5_SNORM:
			case DXGI_FORMAT_BC6H_TYPELESS:
			case DXGI_FORMAT_BC6H_UF16:
			case DXGI_FORMAT_BC6H_SF16:
			case DXGI_FORMAT_BC7_TYPELESS:
			case DXGI_FORMAT_BC7_UNORM:
			case DXGI_FORMAT_BC7_UNORM_SRGB:
				OutBytesPerBlock = 16u;
				return true;

			default:
				OutBytesPerBlock = 0u;
				return false;
			}
		}

		DXGI_FORMAT ResolveShadowUsageFormat(DXGI_FORMAT Format, bool bTreatAsDepthResource)
		{
			switch (Format)
			{
			case DXGI_FORMAT_R32_TYPELESS:
			case DXGI_FORMAT_R32_FLOAT:
			case DXGI_FORMAT_D32_FLOAT:
				return bTreatAsDepthResource ? DXGI_FORMAT_D32_FLOAT : DXGI_FORMAT_R32_FLOAT;

			case DXGI_FORMAT_R24G8_TYPELESS:
			case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
			case DXGI_FORMAT_D24_UNORM_S8_UINT:
				return bTreatAsDepthResource ? DXGI_FORMAT_D24_UNORM_S8_UINT : DXGI_FORMAT_R24_UNORM_X8_TYPELESS;

			case DXGI_FORMAT_R16_TYPELESS:
			case DXGI_FORMAT_R16_FLOAT:
			case DXGI_FORMAT_D16_UNORM:
				return bTreatAsDepthResource ? DXGI_FORMAT_D16_UNORM : DXGI_FORMAT_R16_FLOAT;

			default:
				return Format;
			}
		}

		FString FormatBytesInternal(uint64 Bytes)
		{
			std::ostringstream Stream;
			Stream << std::fixed << std::setprecision(2);

			const double ByteCount = static_cast<double>(Bytes);
			const double Kilobyte = 1024.0;
			const double Megabyte = Kilobyte * 1024.0;
			const double Gigabyte = Megabyte * 1024.0;

			if (ByteCount >= Gigabyte)
			{
				Stream << (ByteCount / Gigabyte) << " GB";
			}
			else if (ByteCount >= Megabyte)
			{
				Stream << (ByteCount / Megabyte) << " MB";
			}
			else if (ByteCount >= Kilobyte)
			{
				Stream << (ByteCount / Kilobyte) << " KB";
			}
			else
			{
				Stream << Bytes << " B";
			}

			return Stream.str();
		}

		const char* GetDXGIFormatDisplayName(DXGI_FORMAT Format)
		{
			switch (Format)
			{
			case DXGI_FORMAT_R32_TYPELESS:
				return "R32_TYPELESS";
			case DXGI_FORMAT_D32_FLOAT:
				return "D32_FLOAT";
			case DXGI_FORMAT_R32_FLOAT:
				return "R32_FLOAT";
			case DXGI_FORMAT_R24G8_TYPELESS:
				return "R24G8_TYPELESS";
			case DXGI_FORMAT_D24_UNORM_S8_UINT:
				return "D24_UNORM_S8_UINT";
			case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
				return "R24_UNORM_X8_TYPELESS";
			case DXGI_FORMAT_R16_TYPELESS:
				return "R16_TYPELESS";
			case DXGI_FORMAT_D16_UNORM:
				return "D16_UNORM";
			case DXGI_FORMAT_R16_FLOAT:
				return "R16_FLOAT";
			case DXGI_FORMAT_R16G16_FLOAT:
				return "R16G16_FLOAT";
			case DXGI_FORMAT_R32G32_FLOAT:
				return "R32G32_FLOAT";
			default:
				return "UNKNOWN";
			}
		}

		const char* GetCubeFaceName(int32 FaceIndex)
		{
			switch (FaceIndex)
			{
			case 0:
				return "+X";
			case 1:
				return "-X";
			case 2:
				return "+Y";
			case 3:
				return "-Y";
			case 4:
				return "+Z";
			case 5:
				return "-Z";
			default:
				return "?";
			}
		}

		const FOpaqueRenderPass::FShadowCB* GetShadowConstantForLight(const FShadowFrameCapture& Capture, uint32 LightIndex)
		{
			if (LightIndex >= static_cast<uint32>(Capture.LightToShadowIndices.size()))
			{
				return nullptr;
			}

			const int32 ShadowIndex = Capture.LightToShadowIndices[LightIndex];
			if (ShadowIndex < 0 || ShadowIndex >= MAX_SHADOW_LIGHTS)
			{
				return nullptr;
			}

			return &Capture.ShadowCBData.ShadowDataArray[ShadowIndex];
		}

		FString ResolveLightDisplayName(const UWorld* World, uint32 SourceLightSlotIndex)
		{
			if (World == nullptr || SourceLightSlotIndex == 0xFFFFFFFF)
			{
				return {};
			}

			const TArray<FLightSlot>& LightSlots = World->GetWorldLightSlots();
			if (SourceLightSlotIndex >= static_cast<uint32>(LightSlots.size()))
			{
				return {};
			}

			const FLightSlot& Slot = LightSlots[SourceLightSlotIndex];
			const ULightComponentBase* LightComponent = Slot.bAlive ? Slot.LightData : nullptr;
			if (LightComponent == nullptr)
			{
				return {};
			}

			const AActor* Owner = LightComponent->GetOwner();
			return (Owner != nullptr)
				? Owner->GetFName().ToString()
				: LightComponent->GetFName().ToString();
		}

		bool ResolveLightCastShadowEnabled(const UWorld* World, uint32 SourceLightSlotIndex, bool bDefaultValue)
		{
			if (World == nullptr || SourceLightSlotIndex == 0xFFFFFFFF)
			{
				return bDefaultValue;
			}

			const TArray<FLightSlot>& LightSlots = World->GetWorldLightSlots();
			if (SourceLightSlotIndex >= static_cast<uint32>(LightSlots.size()))
			{
				return bDefaultValue;
			}

			const FLightSlot& Slot = LightSlots[SourceLightSlotIndex];
			return (Slot.bAlive && Slot.LightData != nullptr)
				? Slot.LightData->IsCastShadows()
				: bDefaultValue;
		}

		void UpdatePSMFallbackLogging(
			const FShowFlags& ShowFlags,
			const FRenderLight& Light,
		uint32 LightId,
		EShadowProjectionMode RequestedMode,
		bool bUsingFallback,
		const FPSMBuildDiagnostics& Diagnostics);

	float ComputeShadowCompareBias(const FRenderLight& Light)
	{
		const float UserBias = std::clamp(Light.ShadowBias, 0.0f, 1.0f);
		return 0.005f * std::max(UserBias * 2.0f, 0.1f);
	}

	float ComputeShadowCompareBias(
		const FRenderLight& Light,
		EShadowMapType MapType,
		const FResolvedShadowProjectionState& ProjectionState,
		uint32 EffectiveResolution)
	{
		float Bias = ComputeShadowCompareBias(Light);
		if (MapType == EShadowMapType::Depth2D || MapType == EShadowMapType::VSM2D)
		{
			const float Resolution = std::max(static_cast<float>(EffectiveResolution), 1.0f);
			const float TexelDepthBias =
				(ProjectionState.Mode == EShadowProjectionMode::PSM ? 4.0f : 2.0f) / Resolution;
			Bias = std::max(Bias, TexelDepthBias);
		}

		return Bias;
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

	bool TryComputePSMMatrix(
		const FRenderPassContext* Context,
		const FRenderLight& Light,
		FMatrix& OutMatrix,
		FPSMBuildDiagnostics* OutDiagnostics = nullptr)
	{
		FRAME_SPIKE_SCOPE("PSM shadow matrix/build step");
		OutMatrix = FMatrix::Identity;
		FPSMBuildDiagnostics Diagnostics = {};

		auto Fail = [&](EPSMInvalidReason Reason) -> bool
		{
			Diagnostics.Reason = Reason;
			if (OutDiagnostics != nullptr)
			{
				*OutDiagnostics = Diagnostics;
			}
			return false;
		};

		if (Context == nullptr || Context->RenderBus == nullptr)
		{
			return Fail(EPSMInvalidReason::MissingContext);
		}

		const FRenderBus& RenderBus = *Context->RenderBus;
		const TArray<FRenderCommand>& ReceiverCommands = GetPSMReceiverCommands(RenderBus);
		const TArray<FRenderCommand>& CasterCommands = GetShadowCasterCommands(RenderBus);
		const ELightType LightType = static_cast<ELightType>(Light.Type);

		FCamera Camera = {};
		Camera.Forward = RenderBus.GetCameraForward();
		Camera.Up = RenderBus.GetCameraUp();
		Camera.Right = RenderBus.GetCameraRight();
		Camera.Position = RenderBus.GetCameraPosition();
		Camera.CameraState = RenderBus.GetCameraState();

		if (!PSM::IsFiniteVector(Camera.Forward) ||
			!PSM::IsFiniteVector(Camera.Up) ||
			!PSM::IsFiniteVector(Camera.Position))
		{
			return Fail(EPSMInvalidReason::InvalidCamera);
		}

		if (LightType == ELightType::LightType_Directional)
		{
			if (ReceiverCommands.empty())
			{
				return Fail(EPSMInvalidReason::NoReceivers);
			}

			FCamera FitCamera = Camera;
			PSM::GetCameraFitNearZ(!CasterCommands.empty() ? CasterCommands : ReceiverCommands, FitCamera);

			const float SliderBack = (Light.CameraSliderBack > 0.0f) ? Light.CameraSliderBack : kDefaultPsmSliderBack;
			FMatrix VirtualCameraView;
			FMatrix VirtualCameraProjection;
			if (!PSM::GenerateVirtualCameraViewProjection(SliderBack, FitCamera, VirtualCameraProjection, VirtualCameraView))
			{
				return Fail(EPSMInvalidReason::InvalidVirtualCamera);
			}

			FMatrix PostPerspectiveView;
			FMatrix PostPerspectiveProjection;
			if (!PSM::GeneratePostPerspectiveViewProjection(
				Light.Direction.GetSafeNormal(),
				PostPerspectiveProjection,
				PostPerspectiveView,
				VirtualCameraView,
				VirtualCameraProjection))
			{
				return Fail(EPSMInvalidReason::DirectionalPostPerspectiveFitFailed);
			}

			OutMatrix = VirtualCameraView * VirtualCameraProjection * PostPerspectiveView * PostPerspectiveProjection;
		}
		else if (LightType == ELightType::LightType_Spot)
		{
			if (!PSM::IsFiniteVector(Light.Position) || Light.Direction.IsNearlyZero())
			{
				return Fail(EPSMInvalidReason::InvalidSpotBasis);
			}

			PSM::FSpotLightPerspectiveFitStats SpotFitStats = {};
			FMatrix SpotView;
			FMatrix SpotProjection;
			if (!PSM::GenerateSpotLightPerspectiveFitViewProjection(
				Camera,
				ReceiverCommands,
				CasterCommands,
				Light.Position,
				Light.Direction.GetSafeNormal(),
				Light.SpotOuterCos,
				std::max(Light.Radius, 1.0f),
				SpotView,
				SpotProjection,
				&SpotFitStats))
			{
				Diagnostics.Near = SpotFitStats.Near;
				Diagnostics.Far = SpotFitStats.Far;
				Diagnostics.Aux0 = SpotFitStats.MinClipX;
				Diagnostics.Aux1 = SpotFitStats.MaxClipX;
				Diagnostics.PointCount = SpotFitStats.ValidPointCount;
				return Fail(EPSMInvalidReason::SpotReceiverFitFailed);
			}

			Diagnostics.Near = SpotFitStats.Near;
			Diagnostics.Far = SpotFitStats.Far;
			Diagnostics.Aux0 = SpotFitStats.MinClipX;
			Diagnostics.Aux1 = SpotFitStats.MaxClipX;
			Diagnostics.PointCount = SpotFitStats.ValidPointCount;
			OutMatrix = SpotView * SpotProjection;
		}
		else
		{
			return Fail(EPSMInvalidReason::UnsupportedLightType);
		}

		if (!PSM::IsFiniteMatrix(OutMatrix))
		{
			return Fail(EPSMInvalidReason::NonFiniteMatrix);
		}

		Diagnostics.Reason = EPSMInvalidReason::None;
		if (OutDiagnostics != nullptr)
		{
			*OutDiagnostics = Diagnostics;
		}

		return true;
	}

	FResolvedShadowProjectionState ResolveShadowProjectionState(
		const FRenderPassContext* Context,
		const FShowFlags& ShowFlags,
		const FRenderLight& Light,
		uint32 LightId)
	{
		FResolvedShadowProjectionState State = {};
		const EShadowProjectionMode RequestedMode = ResolveShadowProjectionMode(ShowFlags, Light);
		State.Mode = RequestedMode;
		FPSMBuildDiagnostics Diagnostics = {};
		if (RequestedMode != EShadowProjectionMode::PSM)
		{
			UpdatePSMFallbackLogging(ShowFlags, Light, LightId, RequestedMode, false, Diagnostics);
			return State;
		}

		if (!TryComputePSMMatrix(Context, Light, State.Matrix, &Diagnostics))
		{
			State.Mode = EShadowProjectionMode::Standard;
			State.Matrix = FMatrix::Identity;
			UpdatePSMFallbackLogging(ShowFlags, Light, LightId, RequestedMode, true, Diagnostics);
			return State;
		}

		UpdatePSMFallbackLogging(ShowFlags, Light, LightId, RequestedMode, false, Diagnostics);
		return State;
	}

	const FResolvedShadowProjectionState& GetResolvedShadowProjectionState(uint32 LightId)
	{
		static const FResolvedShadowProjectionState DefaultState = {};
		return (LightId < static_cast<uint32>(GResolvedProjectionStates.size()))
			? GResolvedProjectionStates[LightId]
			: DefaultState;
	}

	void UpdatePSMFallbackLogging(
		const FShowFlags& ShowFlags,
		const FRenderLight& Light,
		uint32 LightId,
		EShadowProjectionMode RequestedMode,
		bool bUsingFallback,
		const FPSMBuildDiagnostics& Diagnostics)
	{
		if (LightId >= static_cast<uint32>(GPSMFallbackLogStates.size()))
		{
			GPSMFallbackLogStates.resize(LightId + 1);
		}

		FPSMFallbackLogState& PreviousState = GPSMFallbackLogStates[LightId];
		if (RequestedMode != EShadowProjectionMode::PSM)
		{
			PreviousState = {};
			return;
		}

		const ELightType LightType = static_cast<ELightType>(Light.Type);
		const bool bVerbose =
			(LightType == ELightType::LightType_Spot && ShowFlags.bSpotLightDebug) ||
			(LightType == ELightType::LightType_Directional && ShowFlags.bDirectionalLightDebug);

		if (!bUsingFallback)
		{
			if (PreviousState.bWasFallback)
			{
				UE_LOG(
					"[Shadow][PSM] %s Light %u recovered from Standard fallback.",
					GetLightTypeName(LightType),
					LightId);
			}

			PreviousState = {};
			return;
		}

		if (bVerbose || !PreviousState.bWasFallback || PreviousState.Reason != Diagnostics.Reason)
		{
			UE_LOG(
				"[Shadow][PSM] Falling back to Standard for %s Light %u: reason=%s near=%.3f far=%.3f points=%d aux0=%.3f aux1=%.3f",
				GetLightTypeName(LightType),
				LightId,
				GetPSMInvalidReasonName(Diagnostics.Reason),
				Diagnostics.Near,
				Diagnostics.Far,
				Diagnostics.PointCount,
				Diagnostics.Aux0,
				Diagnostics.Aux1);
		}

		PreviousState.bWasFallback = true;
		PreviousState.Reason = Diagnostics.Reason;
	}

	void ApplyShadowProjectionMode(
		FOpaqueRenderPass::FShadowCB& ShadowData,
		const FResolvedShadowProjectionState& ProjectionState)
	{
		const bool bUsePSM = ProjectionState.Mode == EShadowProjectionMode::PSM;
		ShadowData.isPSM = bUsePSM ? 1u : 0u;
		ShadowData.PSM = bUsePSM ? ProjectionState.Matrix : FMatrix::Identity;
	}

	float ComputeVSMDepthBias(
		const FRenderLight& Light,
		EShadowMapType MapType,
		const FResolvedShadowProjectionState& ProjectionState,
		uint32 EffectiveResolution)
	{
		const float UserBias = std::clamp(Light.ShadowBias, 0.0f, 1.0f);
		if (MapType == EShadowMapType::VSMCube)
		{
			return std::max(0.02f, std::max(Light.Radius, 1.0f) * 1.0e-4f * std::max(UserBias * 2.0f, 0.1f));
		}

		float Bias = 5.0e-4f * std::max(UserBias * 2.0f, 0.1f);
		const float Resolution = std::max(static_cast<float>(EffectiveResolution), 1.0f);
		const float TexelDepthBias =
			(ProjectionState.Mode == EShadowProjectionMode::PSM ? 2.0f : 1.0f) / Resolution;
		return std::max(Bias, TexelDepthBias);
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

	void ApplyVSMParameters(
		FOpaqueRenderPass::FShadowCB& ShadowData,
		const FRenderLight& Light,
		EShadowMapType MapType,
		const FResolvedShadowProjectionState& ProjectionState,
		uint32 EffectiveResolution)
	{
		ShadowData.VSMDepthBias = ComputeVSMDepthBias(Light, MapType, ProjectionState, EffectiveResolution);
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

uint32 FShadowPass::GetBytesPerPixel(DXGI_FORMAT Format)
{
	switch (Format)
	{
	case DXGI_FORMAT_R32_TYPELESS:
	case DXGI_FORMAT_D32_FLOAT:
	case DXGI_FORMAT_R32_FLOAT:
	case DXGI_FORMAT_R24G8_TYPELESS:
	case DXGI_FORMAT_D24_UNORM_S8_UINT:
	case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
		return 4u;

	case DXGI_FORMAT_R16_TYPELESS:
	case DXGI_FORMAT_D16_UNORM:
	case DXGI_FORMAT_R16_FLOAT:
		return 2u;

	case DXGI_FORMAT_R16G16_FLOAT:
		return 4u;

	case DXGI_FORMAT_R32G32_FLOAT:
		return 8u;

	default:
		return 0u;
	}
}

uint64 FShadowPass::EstimateTexture2DMemoryBytes(const D3D11_TEXTURE2D_DESC& Desc, DXGI_FORMAT FormatOverride)
{
	const DXGI_FORMAT Format = (FormatOverride != DXGI_FORMAT_UNKNOWN) ? FormatOverride : Desc.Format;
	const uint32 ArraySize = std::max(Desc.ArraySize, 1u);
	const uint32 SampleCount = std::max(Desc.SampleDesc.Count, 1u);
	const uint32 MipCount = GetEffectiveMipCount(Desc);

	uint32 Width = std::max(Desc.Width, 1u);
	uint32 Height = std::max(Desc.Height, 1u);
	uint64 TotalBytes = 0u;

	uint32 BlockWidth = 0u;
	uint32 BlockHeight = 0u;
	uint32 BytesPerBlock = 0u;
	if (GetBlockCompressionInfo(Format, BlockWidth, BlockHeight, BytesPerBlock))
	{
		for (uint32 MipIndex = 0; MipIndex < MipCount; ++MipIndex)
		{
			const uint64 BlocksX = std::max<uint64>((Width + BlockWidth - 1u) / BlockWidth, 1u);
			const uint64 BlocksY = std::max<uint64>((Height + BlockHeight - 1u) / BlockHeight, 1u);
			TotalBytes += BlocksX * BlocksY * BytesPerBlock * ArraySize * SampleCount;
			Width = std::max(Width >> 1u, 1u);
			Height = std::max(Height >> 1u, 1u);
		}

		return TotalBytes;
	}

	const uint32 BytesPerPixel = GetBytesPerPixel(Format);
	if (BytesPerPixel == 0u)
	{
		return 0u;
	}

	for (uint32 MipIndex = 0; MipIndex < MipCount; ++MipIndex)
	{
		TotalBytes += static_cast<uint64>(Width) * Height * BytesPerPixel * ArraySize * SampleCount;
		Width = std::max(Width >> 1u, 1u);
		Height = std::max(Height >> 1u, 1u);
	}

	return TotalBytes;
}

FString FShadowPass::FormatBytes(uint64 Bytes)
{
	return FormatBytesInternal(Bytes);
}

FShadowStats FShadowPass::GetShadowStats(int32 ViewportIndex, const UWorld* World)
{
	FShadowStats Stats;
	const FShadowFrameCapture* Capture = GetShadowFrameCapture(ViewportIndex);
	if (Capture == nullptr)
	{
		return Stats;
	}

	std::unordered_set<const void*> CountedTextures;
	TMap<uint32, size_t> LightLookup;

	auto CountUniqueTexture = [&](ID3D11Texture2D* Texture, DXGI_FORMAT Format, uint64& CategoryBytes, bool* bOutWasUnique = nullptr) -> uint64
	{
		if (bOutWasUnique != nullptr)
		{
			*bOutWasUnique = false;
		}

		if (Texture == nullptr)
		{
			return 0u;
		}

		D3D11_TEXTURE2D_DESC Desc = {};
		Texture->GetDesc(&Desc);
		const uint64 Bytes = EstimateTexture2DMemoryBytes(Desc, Format);
		if (CountedTextures.insert(Texture).second)
		{
			if (bOutWasUnique != nullptr)
			{
				*bOutWasUnique = true;
			}
			Stats.TotalMemoryBytes += Bytes;
			CategoryBytes += Bytes;
		}

		return Bytes;
	};

	auto GetOrAddLightStat = [&](uint32 LightIndex, uint32 SourceLightSlotIndex, ELightType LightType, EShadowMapType MapType, uint32 LogicalSliceCount) -> FLightShadowStat&
	{
		const auto Existing = LightLookup.find(LightIndex);
		if (Existing != LightLookup.end())
		{
			return Stats.Lights[Existing->second];
		}

		FLightShadowStat NewStat;
		NewStat.LightIndex = LightIndex;
		NewStat.SourceLightSlotIndex = SourceLightSlotIndex;
		NewStat.LightName = ResolveLightDisplayName(World, SourceLightSlotIndex);
		NewStat.LightType = LightType;
		NewStat.bCastShadow = ResolveLightCastShadowEnabled(World, SourceLightSlotIndex, true);

		const FOpaqueRenderPass::FShadowCB* ShadowCB = GetShadowConstantForLight(*Capture, LightIndex);
		NewStat.ProjectionMode =
			(ShadowCB != nullptr && ShadowCB->isPSM != 0u)
				? EShadowProjectionMode::PSM
				: EShadowProjectionMode::Standard;
		NewStat.bUsesCSM =
			LightType == ELightType::LightType_Directional &&
			LogicalSliceCount > 1u &&
			NewStat.ProjectionMode != EShadowProjectionMode::PSM;
		NewStat.FilterMode =
			(ShadowCB != nullptr)
				? SanitizeShadowFilterMode(static_cast<int32>(ShadowCB->ShadowFilterMode))
				: ResolveShadowFilterMode(GetDefaultShadowFilterMode(), MapType);

		Stats.Lights.push_back(std::move(NewStat));
		const size_t NewIndex = Stats.Lights.size() - 1u;
		LightLookup[LightIndex] = NewIndex;
		return Stats.Lights[NewIndex];
	};

		auto AppendMapStat = [&](FLightShadowStat& LightStat, const FShadowMapStat& MapStat, bool bCountsAsLogicalShadowMap)
		{
			LightStat.ShadowMaps.push_back(MapStat);
			LightStat.TotalMemoryBytes += MapStat.MemoryBytes;
		++LightStat.ResourceViewCount;
		++Stats.TotalResourceViewCount;

		if (bCountsAsLogicalShadowMap)
		{
				++LightStat.LogicalShadowMapCount;
				++Stats.TotalShadowMapCount;
			}
		};

		auto MakeSingleSliceDesc = [](const D3D11_TEXTURE2D_DESC& Desc) -> D3D11_TEXTURE2D_DESC
		{
			D3D11_TEXTURE2D_DESC SingleSliceDesc = Desc;
			SingleSliceDesc.ArraySize = 1u;
			return SingleSliceDesc;
		};

	for (size_t ShadowMapIndex = 0; ShadowMapIndex < Capture->ShadowMaps.size(); ++ShadowMapIndex)
	{
		const FShadowMap& ShadowMap = Capture->ShadowMaps[ShadowMapIndex];
		if (ShadowMap.Resource == nullptr || ShadowMap.Resource->BackingResource.Texture == nullptr)
		{
			continue;
		}

		D3D11_TEXTURE2D_DESC DepthDesc = {};
		ShadowMap.Resource->BackingResource.Texture->GetDesc(&DepthDesc);
		const DXGI_FORMAT DepthFormat = ResolveShadowUsageFormat(DepthDesc.Format, true);
		const bool bAtlasMap = IsAtlasShadowMap(ShadowMap);
		const bool bPointLight = ShadowMap.LightType == ELightType::LightType_Point;
		const bool bSharedPointResource = bPointLight && DepthDesc.ArraySize > 6u;

		bool bDepthTextureWasUnique = false;
		const uint64 DepthTextureBytes =
			CountUniqueTexture(
				ShadowMap.Resource->BackingResource.Texture.Get(),
				DepthFormat,
				Stats.TotalDepthMemoryBytes,
				&bDepthTextureWasUnique);
		if (bAtlasMap && bDepthTextureWasUnique)
		{
			Stats.TotalAtlasMemoryBytes += DepthTextureBytes;
		}

		FShadowVSMResource* VSMResource =
			(ShadowMapIndex < Capture->VSMResources.size()) ? Capture->VSMResources[ShadowMapIndex] : nullptr;

		D3D11_TEXTURE2D_DESC MomentsDesc = {};
		D3D11_TEXTURE2D_DESC TempDesc = {};
		bool bHasMoments = false;
		bool bHasTemp = false;

		if (VSMResource != nullptr && VSMResource->MomentsTexture != nullptr)
		{
			VSMResource->MomentsTexture->GetDesc(&MomentsDesc);
			bHasMoments = true;
			CountUniqueTexture(VSMResource->MomentsTexture.Get(), MomentsDesc.Format, Stats.TotalVSMMomentMemoryBytes);
		}

		if (VSMResource != nullptr && VSMResource->TempTexture != nullptr)
		{
			VSMResource->TempTexture->GetDesc(&TempDesc);
			bHasTemp = true;
			CountUniqueTexture(VSMResource->TempTexture.Get(), TempDesc.Format, Stats.TotalVSMTempMemoryBytes);
		}

		if (bAtlasMap)
		{
			const uint32 SliceCount = std::min<uint32>(
				static_cast<uint32>(ShadowMap.Views.size()),
				static_cast<uint32>(ShadowMap.Slices.size()));

			for (uint32 SliceIndex = 0; SliceIndex < SliceCount; ++SliceIndex)
			{
				const FShadowSlice& Slice = ShadowMap.Slices[SliceIndex];
				const uint32 SliceWidth = std::max<uint32>(
					static_cast<uint32>(std::lround(DepthDesc.Width * Slice.UVScale.X)),
					1u);
				const uint32 SliceHeight = std::max<uint32>(
					static_cast<uint32>(std::lround(DepthDesc.Height * Slice.UVScale.Y)),
					1u);

				D3D11_TEXTURE2D_DESC SliceDesc = DepthDesc;
				SliceDesc.Width = SliceWidth;
				SliceDesc.Height = SliceHeight;
				SliceDesc.ArraySize = 1u;
				const uint64 SliceBytes = EstimateTexture2DMemoryBytes(SliceDesc, DepthFormat);

				FLightShadowStat& LightStat =
					GetOrAddLightStat(Slice.LightId, Slice.SourceLightSlotIndex, ELightType::LightType_Spot, ShadowMap.MapType, 1u);

				FShadowMapStat DepthStat;
				DepthStat.Name = "Spot Shadow Depth";
				DepthStat.Width = SliceWidth;
				DepthStat.Height = SliceHeight;
				DepthStat.ArraySlice = 0u;
				DepthStat.MipLevel = 0u;
				DepthStat.Format = DepthFormat;
				DepthStat.MemoryBytes = SliceBytes;
				DepthStat.ProjectionMode = LightStat.ProjectionMode;
				DepthStat.FilterMode = LightStat.FilterMode;
				DepthStat.bUsesCSM = LightStat.bUsesCSM;
				DepthStat.bSharedResource = true;
				DepthStat.bHasAtlasRegion = true;
				DepthStat.AtlasX = static_cast<uint32>(std::lround(Slice.UVOffset.X * DepthDesc.Width));
				DepthStat.AtlasY = static_cast<uint32>(std::lround(Slice.UVOffset.Y * DepthDesc.Height));
				AppendMapStat(LightStat, DepthStat, true);
			}

			continue;
		}

		const uint32 AvailableDepthSlices =
			static_cast<uint32>(DepthDesc.ArraySize > ShadowMap.ResourceSliceOffset
				? DepthDesc.ArraySize - ShadowMap.ResourceSliceOffset
				: 0u);
		uint32 SliceCount = std::min<uint32>(static_cast<uint32>(ShadowMap.Views.size()), AvailableDepthSlices);

		if (bHasMoments)
		{
			const uint32 AvailableMomentSlices =
				static_cast<uint32>(MomentsDesc.ArraySize > ShadowMap.ResourceSliceOffset
					? MomentsDesc.ArraySize - ShadowMap.ResourceSliceOffset
					: 0u);
			SliceCount = std::min<uint32>(SliceCount, AvailableMomentSlices);
		}

		if (bHasTemp)
		{
			const uint32 AvailableTempSlices =
				static_cast<uint32>(TempDesc.ArraySize > ShadowMap.ResourceSliceOffset
					? TempDesc.ArraySize - ShadowMap.ResourceSliceOffset
					: 0u);
			SliceCount = std::min<uint32>(SliceCount, AvailableTempSlices);
		}

		if (SliceCount == 0u)
		{
			continue;
		}

			const uint64 DepthSliceBytes =
				EstimateTexture2DMemoryBytes(MakeSingleSliceDesc(DepthDesc), DepthFormat);

			const uint64 MomentSliceBytes = bHasMoments
				? EstimateTexture2DMemoryBytes(MakeSingleSliceDesc(MomentsDesc), MomentsDesc.Format)
				: 0u;

			const uint64 TempSliceBytes = bHasTemp
				? EstimateTexture2DMemoryBytes(MakeSingleSliceDesc(TempDesc), TempDesc.Format)
				: 0u;

		FLightShadowStat& LightStat =
			GetOrAddLightStat(ShadowMap.LightId, ShadowMap.SourceLightSlotIndex, ShadowMap.LightType, ShadowMap.MapType, SliceCount);

		for (uint32 SliceIndex = 0; SliceIndex < SliceCount; ++SliceIndex)
		{
			const uint32 ArraySlice = ShadowMap.ResourceSliceOffset + SliceIndex;
			const bool bDirectionalCascade =
				ShadowMap.LightType == ELightType::LightType_Directional && SliceCount > 1u;

			FString SliceLabel;
			if (ShadowMap.LightType == ELightType::LightType_Point)
			{
				SliceLabel = FString("Face ") + GetCubeFaceName(static_cast<int32>(SliceIndex));
			}
			else if (bDirectionalCascade)
			{
				SliceLabel = "Cascade " + std::to_string(SliceIndex);
			}
			else
			{
				SliceLabel = "Shadow 0";
			}

			FShadowMapStat DepthStat;
			DepthStat.Name = SliceLabel + " Depth";
			DepthStat.Width = DepthDesc.Width;
			DepthStat.Height = DepthDesc.Height;
			DepthStat.ArraySlice = ArraySlice;
			DepthStat.MipLevel = 0u;
			DepthStat.CascadeIndex = bDirectionalCascade ? static_cast<int32>(SliceIndex) : -1;
			DepthStat.FaceIndex =
				ShadowMap.LightType == ELightType::LightType_Point ? static_cast<int32>(SliceIndex) : -1;
			DepthStat.Format = DepthFormat;
			DepthStat.MemoryBytes = DepthSliceBytes;
			DepthStat.ProjectionMode = LightStat.ProjectionMode;
			DepthStat.FilterMode = LightStat.FilterMode;
			DepthStat.bUsesCSM = LightStat.bUsesCSM;
			DepthStat.bSharedResource = bSharedPointResource;
			AppendMapStat(LightStat, DepthStat, true);

			if (bHasMoments)
			{
				FShadowMapStat MomentsStat = DepthStat;
				MomentsStat.Name = SliceLabel + " VSM Moments";
				MomentsStat.Format = MomentsDesc.Format;
				MomentsStat.MemoryBytes = MomentSliceBytes;
				MomentsStat.bSharedResource = bSharedPointResource;
				AppendMapStat(LightStat, MomentsStat, false);
			}

			if (bHasTemp)
			{
				FShadowMapStat TempStat = DepthStat;
				TempStat.Name = SliceLabel + " VSM Temp";
				TempStat.Format = TempDesc.Format;
				TempStat.MemoryBytes = TempSliceBytes;
				TempStat.bSharedResource = bSharedPointResource;
				AppendMapStat(LightStat, TempStat, false);
			}
		}
	}

	std::sort(
		Stats.Lights.begin(),
		Stats.Lights.end(),
		[](const FLightShadowStat& Lhs, const FLightShadowStat& Rhs)
		{
			return Lhs.LightIndex < Rhs.LightIndex;
		});

	Stats.ShadowCastingLightCount = static_cast<uint32>(Stats.Lights.size());
	return Stats;
}

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
	GShadowFrameCaptures.clear();
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
	GResolvedProjectionStates.clear();
	GShadowCBData = FOpaqueRenderPass::FShadowArrayCB{};
	OutSRV = nullptr;
	OutRTV = nullptr;
	bSkip = false;

	const TArray<FRenderLight>& Lights = Context->RenderBus->GetLights();
	GLightToShadowIndices.assign(Lights.size(), -1);
	GResolvedProjectionStates.assign(Lights.size(), FResolvedShadowProjectionState{});
	GPSMFallbackLogStates.resize(Lights.size());

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
		const FResolvedShadowProjectionState ProjectionState =
			ResolveShadowProjectionState(Context, ShowFlags, ShadowLight, ShadowRequest.LightId);
		GResolvedProjectionStates[ShadowRequest.LightId] = ProjectionState;
		ShadowRequest.ProjectionMode = ProjectionState.Mode;
		ShadowRequest.bPSM = ProjectionState.Mode == EShadowProjectionMode::PSM;
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
			const FResolvedShadowProjectionState& ProjectionState = GetResolvedShadowProjectionState(ShadowRequest.LightId);

			FOpaqueRenderPass::FShadowCB& CB = GShadowCBData.ShadowDataArray[ShadowIndexCounter];
			CB.UVOffset = FVector2(0.0f, 0.0f);
			CB.UVScale = FVector2(1.0f, 1.0f);
			CB.ShadowLightPosition = ShadowLight.Position;
			CB.ShadowFar = std::max(ShadowLight.Radius, 0.1f);
			CB.ShadowBias = ComputeShadowCompareBias(
				ShadowLight,
				ShadowMap.MapType,
				ProjectionState,
				SharedPointShadowResource->Resolution);
			CB.ShadowSlopeBias = ShadowLight.ShadowSlopeBias;
			CB.ShadowFilterScale = ComputeShadowFilterScale(ShadowLight);
			CB.ShadowMapType = static_cast<uint32>(ShadowMap.MapType);
			CB.SliceCount = 1;
			CB.ShadowTextureIndex = PointShadowTextureIndexCounter;
			ApplyShadowFilterMode(CB, RequestedShadowFilter, ShadowMap.MapType);
			CB.PointShadowTexelSize =
				2.0f / std::max<float>(static_cast<float>(SharedPointShadowResource->Resolution), 1.0f);
			ApplyVSMParameters(CB, ShadowLight, ShadowMap.MapType, ProjectionState, SharedPointShadowResource->Resolution);
			ApplyShadowProjectionMode(CB, ProjectionState);

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
			const FResolvedShadowProjectionState& ProjectionState = GetResolvedShadowProjectionState(ShadowRequest.LightId);

			FOpaqueRenderPass::FShadowCB& CB = GShadowCBData.ShadowDataArray[ShadowIndexCounter];
			CB.ShadowLightView[0] = CurrentAtlasMap.Views[ViewIndex].LightView;
			CB.ShadowLightProjection[0] = CurrentAtlasMap.Views[ViewIndex].LightProjection;
			CB.UVOffset = AllocResult.UVOffset;
			CB.UVScale = AllocResult.UVScale;
			CB.ShadowBias = ComputeShadowCompareBias(
				ShadowLight,
				CurrentAtlasMap.MapType,
				ProjectionState,
				ShadowRequest.Resolution);
			CB.ShadowSlopeBias = ShadowLight.ShadowSlopeBias;
			CB.ShadowFilterScale = ComputeShadowFilterScale(ShadowLight);
			CB.ShadowMapType = static_cast<uint32>(CurrentAtlasMap.MapType);
			CB.SliceCount = 1;
			CB.ShadowTextureIndex = 1u;
			ApplyShadowFilterMode(CB, RequestedShadowFilter, CurrentAtlasMap.MapType);
			CB.PointShadowTexelSize = 0.0f;
			ApplyVSMParameters(CB, ShadowLight, CurrentAtlasMap.MapType, ProjectionState, ShadowRequest.Resolution);
			ApplyShadowProjectionMode(CB, ProjectionState);

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
		const FResolvedShadowProjectionState& ProjectionState = GetResolvedShadowProjectionState(ShadowRequest.LightId);

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
		CB.ShadowBias = ComputeShadowCompareBias(
			ShadowLight,
			ShadowMap.MapType,
			ProjectionState,
			ShadowMap.Resource ? ShadowMap.Resource->Resolution : 0u);
		CB.ShadowSlopeBias = ShadowLight.ShadowSlopeBias;
		CB.ShadowFilterScale = ComputeShadowFilterScale(ShadowLight);
		CB.ShadowMapType = static_cast<uint32>(ShadowMap.MapType);
		CB.SliceCount = static_cast<uint32>(ShadowRequest.Cascades.size());
		CB.ShadowTextureIndex = 0u;
		ApplyShadowFilterMode(CB, RequestedShadowFilter, ShadowMap.MapType);
		CB.PointShadowTexelSize = 0.0f;
		ApplyVSMParameters(
			CB,
			ShadowLight,
			ShadowMap.MapType,
			ProjectionState,
			ShadowMap.Resource ? ShadowMap.Resource->Resolution : 0u);
		ApplyShadowProjectionMode(CB, ProjectionState);

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
	const TArray<FRenderCommand>& Commands = GetShadowCasterCommands(*RenderBus);
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

	auto GetLight = [&](uint32 LightId) -> const FRenderLight*
	{
		return (LightId < static_cast<uint32>(Lights.size())) ? &Lights[LightId] : nullptr;
	};

	auto DrawShadowCommands = [&](const FShadowViewInfo& ViewInfo, uint32 LightId, const FRenderLight* ShadowLight) -> bool
	{
		if (ShadowLight == nullptr)
		{
			return true;
		}

		ShaderBinding->SetMatrix4("View", ViewInfo.LightView);
		ShaderBinding->SetMatrix4("Projection", ViewInfo.LightProjection);

		const FResolvedShadowProjectionState& ProjectionState = GetResolvedShadowProjectionState(LightId);
		const bool bUsePSM = ProjectionState.Mode == EShadowProjectionMode::PSM;
		ShaderBinding->SetMatrix4("PSM", bUsePSM ? ProjectionState.Matrix : FMatrix::Identity);
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

				if (!DrawShadowCommands(ShadowMap.Views[SliceIndex], Slice.LightId, GetLight(Slice.LightId)))
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

			if (!DrawShadowCommands(ShadowMap.Views[ViewIndex], ShadowMap.LightId, DrawShadowLight))
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
	CacheShadowFrameCapture(Context);

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
