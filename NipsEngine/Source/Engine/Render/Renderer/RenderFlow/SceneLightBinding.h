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
		uint32 MaxLightsPerTile = 0;
		uint32 LightCount = 0;
		float Padding[3] = { 0.0f, 0.0f, 0.0f };
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
		Constants.MaxLightsPerTile = Outputs.MaxLightsPerTile;
		Constants.LightCount = Outputs.LightCount;

		D3D11_MAPPED_SUBRESOURCE Mapped = {};
		if (SUCCEEDED(Context->DeviceContext->Map(VisibleLightConstantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &Mapped)))
		{
			std::memcpy(Mapped.pData, &Constants, sizeof(Constants));
			Context->DeviceContext->Unmap(VisibleLightConstantBuffer.Get(), 0);
		}

		ID3D11ShaderResourceView* SRVs[3] =
		{
			Outputs.LightBufferSRV,
			Outputs.TileLightCountSRV,
			Outputs.TileLightIndexSRV
		};
		
		Context->DeviceContext->PSSetShaderResources(8, 3, SRVs);
		ID3D11Buffer* CBuffer = VisibleLightConstantBuffer.Get();
		Context->DeviceContext->PSSetConstantBuffers(4, 1, &CBuffer);

		Context->DeviceContext->VSSetShaderResources(8, 3, SRVs);
        Context->DeviceContext->VSSetConstantBuffers(4, 1, &CBuffer);
	}

	inline void UnbindResources(ID3D11DeviceContext* DeviceContext)
	{
		if (DeviceContext == nullptr)
		{
			return;
		}

		ID3D11ShaderResourceView* NullSRVs[3] = { nullptr, nullptr, nullptr };
        DeviceContext->PSSetShaderResources(8, 3, NullSRVs);
        DeviceContext->VSSetShaderResources(8, 3, NullSRVs);

		ID3D11Buffer* NullCB = nullptr;
		DeviceContext->PSSetConstantBuffers(4, 1, &NullCB);
	}
}
