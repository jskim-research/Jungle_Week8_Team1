#pragma once
struct ID3D11DepthStencilView;
struct ID3D11ShaderResourceView;

/*
Atlas
SRV 1개 (Texture2D)
DSV 여러 개 (subregion)

CSM
SRV 1개 (Texture2DArray)
DSV N개

Cube
SRV 1개 (TextureCube)
DSV 6개 face view
*/
struct FShadowResource
{
	ID3D11ShaderResourceView* SRV;
	TArray<ID3D11DepthStencilView*> DSVs;
	uint32 Resolution;
};
