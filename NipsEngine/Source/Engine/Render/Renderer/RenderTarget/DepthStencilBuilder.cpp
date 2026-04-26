#include "DepthStencilBuilder.h"

FDepthStencilBuilder& FDepthStencilBuilder::SetSize(uint32 InWidth, uint32 InHeight)
{
	Width = InWidth;
	Height = InHeight;

	return *this;
}

FDepthStencilBuilder& FDepthStencilBuilder::WithStencil()
{
	bUseStencil = true;

	return *this;
}

FDepthStencilBuilder& FDepthStencilBuilder::WithSRV()
{
	bCreateSRV = true;

	return *this;
}

FDepthStencilBuilder& FDepthStencilBuilder::AsCubemap()
{
	bCubemap = true;
	return *this;
}

FDepthStencilResource FDepthStencilBuilder::Build(ID3D11Device* Device)
{
	FDepthStencilResource DSR;
	D3D11_TEXTURE2D_DESC DepthStencilDesc = {};
	DepthStencilDesc.Width = Width;
	DepthStencilDesc.Height = Height;
	DepthStencilDesc.MipLevels = 1;
	DepthStencilDesc.ArraySize = bCubemap ? 6 : 1; // ← 6장
	DepthStencilDesc.Format = DXGI_FORMAT_R24G8_TYPELESS;
	DepthStencilDesc.SampleDesc.Count = 1;
	DepthStencilDesc.SampleDesc.Quality = 0;
	DepthStencilDesc.Usage = D3D11_USAGE_DEFAULT;
	DepthStencilDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
	if (bCubemap)
		DepthStencilDesc.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE; // ← 큐브 플래그
	if (bCreateSRV)
		DepthStencilDesc.BindFlags |= D3D11_BIND_SHADER_RESOURCE;

	Device->CreateTexture2D(&DepthStencilDesc, nullptr, &DSR.Texture);

	// DSV: face 별로 6개
	if (bCubemap)
	{
		D3D11_DEPTH_STENCIL_VIEW_DESC DsvDesc = {};
		DsvDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
		DsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
		DsvDesc.Texture2DArray.MipSlice = 0;
		DsvDesc.Texture2DArray.ArraySize = 1; // face 1장씩

		DSR.DSVs.resize(6);
		DSR.DST = EDepthStencilResourceType::Cubemap;
		for (int i = 0; i < 6; i++)
		{
			DsvDesc.Texture2DArray.FirstArraySlice = i;
			Device->CreateDepthStencilView(DSR.Texture.Get(), &DsvDesc, &DSR.DSVs[i]);
		}
	}
	else
	{
		DSR.DST = EDepthStencilResourceType::Default;
		D3D11_DEPTH_STENCIL_VIEW_DESC DsvDesc = {};
		DsvDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
		DsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
		DsvDesc.Texture2D.MipSlice = 0;
		DSR.DSVs.resize(1);
		Device->CreateDepthStencilView(DSR.Texture.Get(), &DsvDesc, &DSR.DSVs[0]);
	}

	// SRV: TextureCube로
	if (bCreateSRV)
	{
		D3D11_SHADER_RESOURCE_VIEW_DESC SrvDesc = {};
		SrvDesc.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
		SrvDesc.ViewDimension = bCubemap
									? D3D11_SRV_DIMENSION_TEXTURECUBE // ← 큐브맵 SRV
									: D3D11_SRV_DIMENSION_TEXTURE2D;
		SrvDesc.TextureCube.MostDetailedMip = 0;
		SrvDesc.TextureCube.MipLevels = 1;
		Device->CreateShaderResourceView(DSR.Texture.Get(), &SrvDesc, &DSR.SRV);
	}

	return DSR;
}
