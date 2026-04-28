#include "ShadowResourcePool.h"
#include "Render/Renderer/RenderTarget/DepthStencilFactory.h"

FShadowResource* FShadowResourcePool::Acquire(ID3D11Device* Device, const FShadowRequestDesc& Desc)
{
	FShadowResource* ShadowResource = new FShadowResource;
	ShadowResource->Resolution = Desc.Resolution;

	if (Desc.MapType == EShadowMapType::DepthCube)
	{
		ShadowResource->BackingResource =
			FDepthStencilFactory::CreateDepthStencilViewCubemap(Device, Desc.Resolution, Desc.Resolution);
	}
	else
	{
		if (Desc.AllocationMode == EShadowAllocationMode::ArrayBased)
		{
            ShadowResource->BackingResource = FDepthStencilFactory::CreateDepthStencilViewCSMArray(Device, Desc.Resolution, Desc.Resolution, Desc.CascadeCount);
		}
		else
        {
            ShadowResource->BackingResource =
                FDepthStencilFactory::CreateDepthStencilView(Device, Desc.Resolution, Desc.Resolution);
		}
	}

	ShadowResource->SRV = ShadowResource->BackingResource.SRV.Get();
	for (const TComPtr<ID3D11DepthStencilView>& DepthStencilView : ShadowResource->BackingResource.DSVs)
	{
		ShadowResource->DSVs.push_back(DepthStencilView.Get());
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
