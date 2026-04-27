#pragma once

#include "LightCullingPass.h"
#include "Render/Common/ComPtr.h"
#include <cstring>

namespace SceneLightBinding
{
	struct FVisibleLightConstants
	{
		uint32 TileCountX = 0;
		uint32 TileCountY = 0;
		uint32 TileSize = 0;
		uint32 MaxPointLightsPerTile = 0;
		uint32 MaxSpotLightsPerTile = 0;
		uint32 VisiblePointLightCount = 0;
		uint32 VisibleSpotLightCount = 0;
		uint32 Padding0 = 0;
	};

	inline bool EnsureVisibleLightConstantBuffer(ID3D11Device* Device, TComPtr<ID3D11Buffer>& VisibleLightConstantBuffer)
	{
		if (VisibleLightConstantBuffer)
		{
			return true;
		}

		D3D11_BUFFER_DESC Desc = {};
		Desc.ByteWidth = sizeof(FVisibleLightConstants);
		Desc.Usage = D3D11_USAGE_DYNAMIC;
		Desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		Desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

		return SUCCEEDED(Device->CreateBuffer(&Desc, nullptr, VisibleLightConstantBuffer.GetAddressOf()));
	}

	inline void BindResources(const FRenderPassContext* Context, TComPtr<ID3D11Buffer>& VisibleLightConstantBuffer)
	{
		if (Context == nullptr || Context->Device == nullptr || Context->DeviceContext == nullptr)
		{
			return;
		}

		const FLightCullingOutputs& Outputs = FLightCullingPass::GetOutputs();
		if (!EnsureVisibleLightConstantBuffer(Context->Device, VisibleLightConstantBuffer))
		{
			return;
		}

		FVisibleLightConstants Constants = {};
		Constants.TileCountX = Outputs.TileCountX;
		Constants.TileCountY = Outputs.TileCountY;
		Constants.TileSize = Outputs.TileSize;
		Constants.MaxPointLightsPerTile = Outputs.MaxPointLightsPerTile;
		Constants.MaxSpotLightsPerTile = Outputs.MaxSpotLightsPerTile;
		Constants.VisiblePointLightCount = Outputs.PointLightCount;
		Constants.VisibleSpotLightCount = Outputs.SpotLightCount;

		D3D11_MAPPED_SUBRESOURCE Mapped = {};
		if (SUCCEEDED(Context->DeviceContext->Map(VisibleLightConstantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &Mapped)))
		{
			std::memcpy(Mapped.pData, &Constants, sizeof(Constants));
			Context->DeviceContext->Unmap(VisibleLightConstantBuffer.Get(), 0);
		}

		ID3D11ShaderResourceView* SRVs[6] =
		{
			Outputs.PointLightBufferSRV,
			Outputs.SpotLightBufferSRV,
			Outputs.TilePointLightGridSRV,
			Outputs.TilePointLightIndexSRV,
			Outputs.TileSpotLightGridSRV,
			Outputs.TileSpotLightIndexSRV
		};
		
		Context->DeviceContext->PSSetShaderResources(8, 6, SRVs);
		ID3D11Buffer* CBuffer = VisibleLightConstantBuffer.Get();
		Context->DeviceContext->PSSetConstantBuffers(4, 1, &CBuffer);

		Context->DeviceContext->VSSetShaderResources(8, 6, SRVs);
        Context->DeviceContext->VSSetConstantBuffers(4, 1, &CBuffer);
	}

	inline void UnbindResources(ID3D11DeviceContext* DeviceContext)
	{
		if (DeviceContext == nullptr)
		{
			return;
		}

		ID3D11ShaderResourceView* NullSRVs[6] = { nullptr, nullptr, nullptr, nullptr, nullptr, nullptr };
		DeviceContext->PSSetShaderResources(8, 6, NullSRVs);
		DeviceContext->VSSetShaderResources(8, 6, NullSRVs);

		ID3D11Buffer* NullCB = nullptr;
		DeviceContext->PSSetConstantBuffers(4, 1, &NullCB);
		DeviceContext->VSSetConstantBuffers(4, 1, &NullCB);
	}
}
