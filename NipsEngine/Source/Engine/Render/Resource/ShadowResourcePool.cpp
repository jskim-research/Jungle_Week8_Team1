#include "ShadowResourcePool.h"
#include "Render/Renderer/RenderTarget/DepthStencilFactory.h"
#include <algorithm>

FShadowResource* FShadowResourcePool::Acquire(ID3D11Device* Device, const FShadowRequestDesc& Desc)
{
	FShadowResource* ShadowResource = new FShadowResource;
	ShadowResource->Resolution = Desc.Resolution;

	if (Desc.MapType == EShadowMapType::DepthCube || Desc.MapType == EShadowMapType::VSMCube)
	{
		if (Desc.AllocationMode == EShadowAllocationMode::ArrayBased)
		{
			ShadowResource->BackingResource =
				FDepthStencilFactory::CreateDepthStencilViewCubemapArray(Device, Desc.Resolution, Desc.Resolution, std::max(Desc.CubeCount, 1u));
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
            ShadowResource->BackingResource = FDepthStencilFactory::CreateDepthStencilViewArray(Device, Desc.Resolution, Desc.Resolution, Desc.CascadeCount);
		}
        else if (Desc.AllocationMode == EShadowAllocationMode::AtlasPacked)
        {
            ShadowResource->BackingResource =
                FDepthStencilFactory::CreateDepthStencilViewArray(Device, Desc.Resolution, Desc.Resolution, 1);
		}
		else
        {
			// Texture Array 만 현재 고려중
            assert(false && "현재는 지원하지 않는 모드");
		}
	}

	ShadowResource->SRV = ShadowResource->BackingResource.SRV.Get();
	for (const TComPtr<ID3D11DepthStencilView>& DepthStencilView : ShadowResource->BackingResource.DSVs)
	{
		ShadowResource->DSVs.push_back(DepthStencilView.Get());
	}

	if (ShadowResource->BackingResource.Texture == nullptr ||
		ShadowResource->SRV == nullptr ||
		ShadowResource->DSVs.empty())
	{
		delete ShadowResource;
		return nullptr;
	}

	return ShadowResource;
}

void FShadowResourcePool::Release(FShadowResource* Resource)
{
	if (Resource)
	{
		delete Resource;
	}
}

void FShadowResourcePool::BeginFrame()
{
}
