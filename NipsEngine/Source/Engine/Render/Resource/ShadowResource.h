#pragma once
#include "Core/Common.h"
#include <d3d11.h>

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
	std::vector<ID3D11DepthStencilView*> DSVs;
	uint32 Resolution;
};