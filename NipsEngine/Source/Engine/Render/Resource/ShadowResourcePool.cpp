#include "ShadowResourcePool.h"
#include "Render/Renderer/RenderTarget/DepthStencilFactory.h"

FShadowResource* FShadowResourcePool::Acquire(ID3D11Device* Device, const FShadowRequestDesc& Desc)
{
	// 원래 따로 Pool 에서 가져오는게 맞는데, 테스트용으로 다음과 같이 설정
	// 테스트 용도긴 한데 자원 누수 신경써야함
    FShadowResource* ShadowResource = new FShadowResource;
	DSR = FDepthStencilFactory::CreateDepthStencilView(Device, Desc.Resolution, Desc.Resolution);

	ShadowResource->SRV = DSR.SRV.Get();
    ShadowResource->DSVs.push_back(DSR.DSV.Get());

    return ShadowResource;
}

void FShadowResourcePool::Release(FShadowResource* Resource)
{
	// 역시 테스트 용도 Release
	if (Resource)
	{
        delete Resource;
	}
}

void FShadowResourcePool::BeginFrame()
{
}
