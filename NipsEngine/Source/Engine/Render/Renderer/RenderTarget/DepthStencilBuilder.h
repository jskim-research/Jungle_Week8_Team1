#pragma once
#include "Core/CoreTypes.h"
#include "DepthStencilResource.h"

#define MAX_TEXTURE_ARRAY_NUM 3

struct ID3D11Device;

enum class ETextureMode
{
	Single,  // 단일 텍스쳐
	Cubemap,
	Array
};

class FDepthStencilBuilder
{
private:
	uint32 Width = 0;
	uint32 Height = 0;

	/** Stencil 을 따로 안 쓰면 Depth 쪽 데이터 bit 를 늘리는 방식이 적절 */
	bool bUseStencil = false;
	bool bCreateSRV = false;

    ETextureMode TextureMode;
    uint32 TextureArraySize = 1;

public:
	FDepthStencilBuilder& SetSize(uint32 InWidth, uint32 InHeight);
	FDepthStencilBuilder& WithStencil();
	FDepthStencilBuilder& WithSRV();
	FDepthStencilBuilder& AsCubemap();
    FDepthStencilBuilder& AsArray(uint32 InArraySize);
	FDepthStencilResource Build(ID3D11Device* Device);
};

