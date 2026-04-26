#pragma once
#include "RenderPass.h"
#include "Render/Common/ComPtr.h"

class FDecalRenderPass : public FBaseRenderPass
{
public:
	bool Initialize() override;
	bool Release() override;

private:
	bool Begin(const FRenderPassContext* Context) override;
	bool DrawCommand(const FRenderPassContext* Context) override;
	bool End(const FRenderPassContext* Context) override;

private:
	bool bSkipDecalDraw = false;
	TComPtr<ID3D11Buffer> VisibleLightConstantBuffer;
};
