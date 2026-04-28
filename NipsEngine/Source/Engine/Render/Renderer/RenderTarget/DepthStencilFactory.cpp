#include "DepthStencilFactory.h"

FDepthStencilResource FDepthStencilFactory::CreateDepthStencilView(ID3D11Device* Device, uint32 InWidth, uint32 InHeight)
{
	return FDepthStencilBuilder().SetSize(InWidth, InHeight).WithStencil().WithSRV().Build(Device);
}

FDepthStencilResource FDepthStencilFactory::CreateDepthStencilViewCubemap(ID3D11Device* Device, uint32 InWidth, uint32 InHeight)
{
	return FDepthStencilBuilder().SetSize(InWidth, InHeight).WithStencil().WithSRV().AsCubemap().Build(Device);
}

FDepthStencilResource FDepthStencilFactory::CreateDepthStencilViewArray(ID3D11Device* Device, uint32 InWidth, uint32 InHeight, uint32 CascadeNum)
{
    return FDepthStencilBuilder().SetSize(InWidth, InHeight).WithStencil().WithSRV().AsArray(CascadeNum).Build(Device);
}
