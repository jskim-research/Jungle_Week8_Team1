#include "ShadowResourcePool.h"

#include "Core/Logging/Stats.h"
#include "Render/Renderer/RenderTarget/DepthStencilFactory.h"

#include <algorithm>
#include <cassert>

namespace
{
	EShadowMapType NormalizeDepthResourceMapType(EShadowMapType MapType)
	{
		switch (MapType)
		{
		case EShadowMapType::VSM2D:
			return EShadowMapType::Depth2D;
		case EShadowMapType::VSMCube:
			return EShadowMapType::DepthCube;
		default:
			return MapType;
		}
	}

	bool MatchesShadowResourceDesc(const FShadowRequestDesc& Lhs, const FShadowRequestDesc& Rhs)
	{
		return NormalizeDepthResourceMapType(Lhs.MapType) == NormalizeDepthResourceMapType(Rhs.MapType) &&
			   Lhs.AllocationMode == Rhs.AllocationMode &&
			   Lhs.Resolution == Rhs.Resolution &&
			   Lhs.CascadeCount == Rhs.CascadeCount &&
			   Lhs.CubeCount == Rhs.CubeCount;
	}

	std::unique_ptr<FShadowResource> CreateShadowResource(ID3D11Device* Device, const FShadowRequestDesc& Desc)
	{
		std::unique_ptr<FShadowResource> ShadowResource = std::make_unique<FShadowResource>();
		ShadowResource->Resolution = Desc.Resolution;

		if (Desc.MapType == EShadowMapType::DepthCube || Desc.MapType == EShadowMapType::VSMCube)
		{
			if (Desc.AllocationMode == EShadowAllocationMode::ArrayBased)
			{
				ShadowResource->BackingResource =
					FDepthStencilFactory::CreateDepthStencilViewCubemapArray(
						Device,
						Desc.Resolution,
						Desc.Resolution,
						std::max(Desc.CubeCount, 1u));
			}
			else
			{
				ShadowResource->BackingResource =
					FDepthStencilFactory::CreateDepthStencilViewCubemap(Device, Desc.Resolution, Desc.Resolution);
			}
		}
		else
		{
			if (Desc.AllocationMode == EShadowAllocationMode::ArrayBased)
			{
				ShadowResource->BackingResource =
					FDepthStencilFactory::CreateDepthStencilViewArray(Device, Desc.Resolution, Desc.Resolution, Desc.CascadeCount);
			}
			else if (Desc.AllocationMode == EShadowAllocationMode::AtlasPacked)
			{
				ShadowResource->BackingResource =
					FDepthStencilFactory::CreateDepthStencilViewArray(Device, Desc.Resolution, Desc.Resolution, 1);
			}
			else
			{
				assert(false && "Unsupported shadow allocation mode.");
				return nullptr;
			}
		}

		ShadowResource->SRV = ShadowResource->BackingResource.SRV.Get();
		ShadowResource->DSVs.clear();
		ShadowResource->DSVs.reserve(ShadowResource->BackingResource.DSVs.size());
		for (const TComPtr<ID3D11DepthStencilView>& DepthStencilView : ShadowResource->BackingResource.DSVs)
		{
			ShadowResource->DSVs.push_back(DepthStencilView.Get());
		}

		if (ShadowResource->BackingResource.Texture == nullptr ||
			ShadowResource->SRV == nullptr ||
			ShadowResource->DSVs.empty())
		{
			return nullptr;
		}

		return ShadowResource;
	}
}

FShadowResource* FShadowResourcePool::Acquire(ID3D11Device* Device, const FShadowRequestDesc& Desc)
{
	for (FPooledShadowResourceEntry& Entry : ResourcePool)
	{
		if (!Entry.bInUse && Entry.Resource && MatchesShadowResourceDesc(Entry.Desc, Desc))
		{
			Entry.bInUse = true;
			FFrameSpikeProfiler::Get().AddCounter("Shadow depth resource reuses");
			return Entry.Resource.get();
		}
	}

	std::unique_ptr<FShadowResource> NewResource;
	{
		FRAME_SPIKE_SCOPE("Shadow depth resource create");
		NewResource = CreateShadowResource(Device, Desc);
	}

	if (!NewResource)
	{
		return nullptr;
	}

	FFrameSpikeProfiler::Get().AddCounter("Shadow depth resource creates");

	FPooledShadowResourceEntry Entry;
	Entry.Desc = Desc;
	Entry.Resource = std::move(NewResource);
	Entry.bInUse = true;

	FShadowResource* ResourcePtr = Entry.Resource.get();
	ResourcePool.push_back(std::move(Entry));
	ResourceLookup[ResourcePtr] = static_cast<uint32>(ResourcePool.size() - 1);
	return ResourcePtr;
}

void FShadowResourcePool::Release(FShadowResource* Resource)
{
	if (Resource == nullptr)
	{
		return;
	}

	const auto It = ResourceLookup.find(Resource);
	if (It == ResourceLookup.end())
	{
		assert(false && "Attempted to release a shadow resource that is not owned by the pool.");
		return;
	}

	ResourcePool[It->second].bInUse = false;
}

void FShadowResourcePool::BeginFrame()
{
}
