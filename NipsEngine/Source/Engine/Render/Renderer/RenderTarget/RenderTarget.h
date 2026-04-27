#pragma once
#include "Core/CoreTypes.h"
#include "Render/Common/ComPtr.h"

#include <d3d11.h>
#include <dxgi1_5.h>

struct FRenderTarget
{
	TComPtr<ID3D11Texture2D> Texture;
	TComPtr<ID3D11RenderTargetView> RTV;
	TComPtr<ID3D11ShaderResourceView> SRV;

	uint32 Width = 0;
	uint32 Height = 0;
	DXGI_FORMAT Format = DXGI_FORMAT_UNKNOWN;
};

