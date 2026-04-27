#pragma once
#include "Core/Containers/Array.h"
#include "Render/Common/ComPtr.h"

struct ID3D11DepthStencilView;
struct ID3D11ShaderResourceView;
struct ID3D11Texture2D;

enum class EDepthStencilResourceType
{
	Default,	// DSV 1장
	Cubemap,	// DSV 6장
	Array		//
};

struct FDepthStencilResource
{
	TComPtr<ID3D11Texture2D> Texture;
	TArray<TComPtr<ID3D11DepthStencilView>> DSVs;
	TComPtr<ID3D11ShaderResourceView> SRV;
	EDepthStencilResourceType DST;
};

