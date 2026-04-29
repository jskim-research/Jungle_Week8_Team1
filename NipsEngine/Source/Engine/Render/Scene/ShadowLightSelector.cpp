#include "ShadowLightSelector.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include "Component/Light/LightComponent.h"

namespace
{
	constexpr uint32 MaxAtlasShadowCount = 16;
	// Must stay within the shadow constant-buffer budget consumed by UberLit/OpaqueRenderPass.
	constexpr uint32 MaxPointShadowCount = 32;
	constexpr int32 PointBaseShadowResolution = 512;
	constexpr int32 AtlasBaseShadowResolution = 1024;

	struct FScoredShadowCandidate
	{
		float Score = 0.0f;
		uint32 LightIndex = 0;
	};

	float ComputePointShadowScore(const FRenderLight& Light, const FVector& CameraPosition)
	{
		const float Radius = std::max(Light.Radius, 0.001f);
		const float DistanceToCenter = FVector::Dist(CameraPosition, Light.Position);
		const float VolumeDist = std::max(0.0f, DistanceToCenter - Radius);
		const float Influence = std::max(Light.Intensity, 0.0f) * Radius * Radius;
		return Influence / (1.0f + VolumeDist * VolumeDist);
	}

	float ComputeSpotShadowScore(const FRenderLight& Light, const FVector& CameraPosition)
	{
		const float Height = std::max(Light.Radius, 0.001f);
		const FVector Axis = Light.Direction.GetSafeNormal();
		const float CosTheta = std::clamp(Light.SpotOuterCos, 0.001f, 0.9999f);
		const float SinTheta = std::sqrt(std::max(0.0f, 1.0f - CosTheta * CosTheta));
		const float BaseRadius = Height * (SinTheta / CosTheta);

		FVector BoundsCenter = Light.Position + Axis * Height;
		float BoundsRadius = BaseRadius;
		if (BaseRadius <= Height)
		{
			BoundsRadius = (Height * Height + BaseRadius * BaseRadius) / (2.0f * Height);
			BoundsCenter = Light.Position + Axis * BoundsRadius;
		}

		const float DistanceToCenter = FVector::Dist(CameraPosition, BoundsCenter);
		const float VolumeDist = std::max(0.0f, DistanceToCenter - BoundsRadius);
		const FVector ToCamera = (CameraPosition - Light.Position).GetSafeNormal();
		const float Facing = std::max(0.0f, FVector::DotProduct(Axis, ToCamera));
		const float FacingWeight = 0.25f + 0.75f * Facing * Facing;
		const float Influence = std::max(Light.Intensity, 0.0f) * BoundsRadius * BoundsRadius;
		return (Influence * FacingWeight) / (1.0f + VolumeDist * VolumeDist);
	}

	float ComputeShadowScore(const FRenderLight& Light, const FVector& CameraPosition)
	{
		switch (static_cast<ELightType>(Light.Type))
		{
		case ELightType::LightType_Directional:
			return std::numeric_limits<float>::max();
		case ELightType::LightType_Spot:
			return ComputeSpotShadowScore(Light, CameraPosition);
		case ELightType::LightType_Point:
			return ComputePointShadowScore(Light, CameraPosition);
		default:
			return 0.0f;
		}
	}

	int32 ComputeShadowResolution(const FRenderLight& Light)
	{
		const ELightType Type = static_cast<ELightType>(Light.Type);
		const int32 BaseResolution = (Type == ELightType::LightType_Point) ? PointBaseShadowResolution : AtlasBaseShadowResolution;
		const float SafeScale = std::clamp(Light.ShadowResolutionScale, 0.125f, 4.0f);
		const int32 AlignedResolution = static_cast<int32>(std::lround((BaseResolution * SafeScale) / 128.0f)) * 128;
		const int32 MaxResolution = (Type == ELightType::LightType_Point) ? 1024 : 2048;
		return std::clamp(AlignedResolution, 256, MaxResolution);
	}
}

TArray<FShadowRequest> FShadowLightSelector::SelectShadowLights(const TArray<FRenderLight>& SceneLights, const FVector& CameraPosition, const FCameraState& CameraState)
{
	TArray<FShadowRequest> SelectedLights;

	if (SceneLights.empty())
		return SelectedLights;

	TArray<FScoredShadowCandidate> AtlasCandidates;
	TArray<FScoredShadowCandidate> PointCandidates;

	for (uint32 LightIndex = 0; LightIndex < static_cast<uint32>(SceneLights.size()); ++LightIndex)
	{
		const FRenderLight& Light = SceneLights[LightIndex];
		const ELightType Type = static_cast<ELightType>(Light.Type);
		if (Type == ELightType::LightType_AmbientLight || Type == ELightType::Max || !Light.bCastShadows)
		{
			continue;
		}

		FScoredShadowCandidate Candidate = {};
		Candidate.Score = ComputeShadowScore(Light, CameraPosition);
		Candidate.LightIndex = LightIndex;

		if (Type == ELightType::LightType_Point)
		{
			PointCandidates.push_back(Candidate);
		}
		else
		{
			AtlasCandidates.push_back(Candidate);
		}
	}

	auto AppendBestCandidates = [&](const TArray<FScoredShadowCandidate>& Candidates, uint32 MaxCount)
	{
		TArray<FScoredShadowCandidate> SortedCandidates = Candidates;
		const uint32 SelectedCount = std::min<uint32>(static_cast<uint32>(SortedCandidates.size()), MaxCount);
		if (SelectedCount == 0)
		{
			return;
		}

		std::partial_sort(
			SortedCandidates.begin(),
			SortedCandidates.begin() + SelectedCount,
			SortedCandidates.end(),
			[](const FScoredShadowCandidate& Lhs, const FScoredShadowCandidate& Rhs)
			{
				return Lhs.Score > Rhs.Score;
			});

		for (uint32 CandidateIndex = 0; CandidateIndex < SelectedCount; ++CandidateIndex)
		{
			const uint32 LightIndex = SortedCandidates[CandidateIndex].LightIndex;
			const FRenderLight& Light = SceneLights[LightIndex];

			FShadowRequest Req;
			Req.LightId = LightIndex;
			Req.Type = static_cast<ELightType>(Light.Type);
			Req.Resolution = ComputeShadowResolution(Light);
			Req.ProjectionMode = Light.bPSM ? EShadowProjectionMode::PSM : EShadowProjectionMode::Standard;
			Req.bUseVSM = false;
			Req.bPSM = Light.bPSM;
			FCascadeInfo CascadeInfo;

			if (Req.Type == ELightType::LightType_Directional)
			{
                CascadeInfo.Near = CameraState.NearZ;
                CascadeInfo.Far = 50;
                Req.Cascades.push_back(CascadeInfo);

                CascadeInfo.Near = 50;
                CascadeInfo.Far = 200;
                Req.Cascades.push_back(CascadeInfo);

                CascadeInfo.Near = 200;
                CascadeInfo.Far = 400;
                Req.Cascades.push_back(CascadeInfo);
			}
			else
            {
                CascadeInfo.Near = CameraState.NearZ;
                CascadeInfo.Far = CameraState.FarZ;
                Req.Cascades.push_back(CascadeInfo);
			}

			SelectedLights.push_back(Req);
		}
	};

	AppendBestCandidates(PointCandidates, MaxPointShadowCount);
	AppendBestCandidates(AtlasCandidates, MaxAtlasShadowCount);
	return SelectedLights;
}
