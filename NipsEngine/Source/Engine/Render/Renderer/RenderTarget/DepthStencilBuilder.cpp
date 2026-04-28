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
    TextureMode = ETextureMode::Cubemap;
    TextureArraySize = 6;
    CubeCount = 1;
	return *this;
}

FDepthStencilBuilder& FDepthStencilBuilder::AsCubemapArray(uint32 InCubeCount)
{
    assert(InCubeCount > 0 && InCubeCount * 6 <= MAX_TEXTURE_ARRAY_NUM);
    if (InCubeCount == 0)
        return *this;

    TextureMode = ETextureMode::CubemapArray;
    CubeCount = InCubeCount;
    TextureArraySize = InCubeCount * 6;
    return *this;
}

FDepthStencilBuilder& FDepthStencilBuilder::AsArray(uint32 InArraySize)
{
    assert(InArraySize > 0 && InArraySize <= MAX_TEXTURE_ARRAY_NUM);
    if (InArraySize == 0)
        return *this;

    TextureMode = ETextureMode::Array;
    CubeCount = 1;
    TextureArraySize = InArraySize;
    return *this;
}

FDepthStencilResource FDepthStencilBuilder::Build(ID3D11Device* Device)
{
	FDepthStencilResource DSR;
	D3D11_TEXTURE2D_DESC DepthStencilDesc = {};
	DepthStencilDesc.Width = Width;
	DepthStencilDesc.Height = Height;
	DepthStencilDesc.MipLevels = 1;
    DepthStencilDesc.ArraySize = TextureArraySize;
	DepthStencilDesc.Format = DXGI_FORMAT_R24G8_TYPELESS;
	DepthStencilDesc.SampleDesc.Count = 1;
	DepthStencilDesc.SampleDesc.Quality = 0;
	DepthStencilDesc.Usage = D3D11_USAGE_DEFAULT;
	DepthStencilDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL;

	DepthStencilDesc.MiscFlags = 0;
	if (TextureMode == ETextureMode::Cubemap || TextureMode == ETextureMode::CubemapArray)
		DepthStencilDesc.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE; // ← 큐브 플래그

	if (bCreateSRV)
		DepthStencilDesc.BindFlags |= D3D11_BIND_SHADER_RESOURCE;

	Device->CreateTexture2D(&DepthStencilDesc, nullptr, &DSR.Texture);

	// DSV: face 별로 6개
	if (TextureMode == ETextureMode::Cubemap || TextureMode == ETextureMode::CubemapArray)
	{
		D3D11_DEPTH_STENCIL_VIEW_DESC DsvDesc = {};
		DsvDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
		DsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
		DsvDesc.Texture2DArray.MipSlice = 0;
		DsvDesc.Texture2DArray.ArraySize = 1; // face 1장씩

		DSR.DSVs.resize(TextureArraySize);
		DSR.DST = (TextureMode == ETextureMode::CubemapArray) ? EDepthStencilResourceType::CubemapArray : EDepthStencilResourceType::Cubemap;
		for (uint32 i = 0; i < TextureArraySize; i++)
		{
			DsvDesc.Texture2DArray.FirstArraySlice = i;
			Device->CreateDepthStencilView(DSR.Texture.Get(), &DsvDesc, &DSR.DSVs[i]);
		}
	}
	else if (TextureMode == ETextureMode::Array)
	{
        DSR.DST = EDepthStencilResourceType::Array;

        D3D11_DEPTH_STENCIL_VIEW_DESC DsvDesc = {};
        DsvDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        DsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
        DsvDesc.Texture2DArray.MipSlice = 0;
        DsvDesc.Texture2DArray.ArraySize = 1; // slice 하나씩

        DSR.DSVs.resize(TextureArraySize);

        for (uint32 i = 0; i < TextureArraySize; i++)
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

		if (TextureMode == ETextureMode::Cubemap)
        {
            SrvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
            SrvDesc.TextureCube.MostDetailedMip = 0;
            SrvDesc.TextureCube.MipLevels = 1;
        }
        else if (TextureMode == ETextureMode::CubemapArray)
        {
            SrvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBEARRAY;
            SrvDesc.TextureCubeArray.MostDetailedMip = 0;
            SrvDesc.TextureCubeArray.MipLevels = 1;
            SrvDesc.TextureCubeArray.First2DArrayFace = 0;
            SrvDesc.TextureCubeArray.NumCubes = CubeCount;
        }
        else if (TextureMode == ETextureMode::Array)
        {
            SrvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
            SrvDesc.Texture2DArray.MostDetailedMip = 0;
            SrvDesc.Texture2DArray.MipLevels = 1;
            SrvDesc.Texture2DArray.FirstArraySlice = 0;
            SrvDesc.Texture2DArray.ArraySize = TextureArraySize;
        }
        else
        {
            SrvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            SrvDesc.Texture2D.MostDetailedMip = 0;
            SrvDesc.Texture2D.MipLevels = 1;
        }
		Device->CreateShaderResourceView(DSR.Texture.Get(), &SrvDesc, &DSR.SRV);
	}

	return DSR;
}
