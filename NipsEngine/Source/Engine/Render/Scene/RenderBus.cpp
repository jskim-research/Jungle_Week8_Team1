#include "RenderBus.h"
#include <UI/EditorConsoleWidget.h>

void FRenderBus::Clear()
{
	for (uint32 i = 0; i < (uint32)ERenderPass::MAX; ++i)
	{
		PassQueues[i].clear();
	}

	Lights.clear();
	bHasDirectionalLight = false;
	DirectionalLightDirection = FVector::ZeroVector;
	DirectionalLightColor = FVector::ZeroVector;
	DirectionalLightIntensity = 0.0f;
	bHasAmbientLight = false;
	GlobalAmbientColor = FVector(0.02f, 0.02f, 0.02f);
}

void FRenderBus::AddCommand(ERenderPass Pass, const FRenderCommand& InCommand)
{
	PassQueues[(uint32)Pass].push_back(InCommand);
}

void FRenderBus::AddCommand(ERenderPass Pass, FRenderCommand&& InCommand)
{
	PassQueues[(uint32)Pass].push_back(std::move(InCommand));
}

const TArray<FRenderCommand>& FRenderBus::GetCommands(ERenderPass Pass) const
{
	return PassQueues[(uint32)Pass];
}

void FRenderBus::SetViewProjection(const FMatrix& InView, const FMatrix& InProj)
{
	View = InView;
	Proj = InProj;

	const FMatrix CameraWorldMatrix = InView.GetInverse();
	CameraPosition = CameraWorldMatrix.GetOrigin();
	CameraForward = CameraWorldMatrix.GetForwardVector().GetSafeNormal();
	CameraRight = CameraWorldMatrix.GetRightVector().GetSafeNormal();
	CameraUp = CameraWorldMatrix.GetUpVector().GetSafeNormal();
}

void FRenderBus::SetRenderSettings(const EViewMode NewViewMode, const FShowFlags NewShowFlags)
{
	ViewMode = NewViewMode;
	ShowFlags = NewShowFlags;
}

void FRenderBus::SetDirectionalLight(const FVector& InDirectionToLight, const FVector& InLightColor, float InIntensity)
{
	bHasDirectionalLight = true;
	DirectionalLightDirection = InDirectionToLight;
	DirectionalLightColor = InLightColor;
	DirectionalLightIntensity = InIntensity;
}

void FRenderBus::SetAmbientLight(const FVector& InAmbientColor)
{
	bHasAmbientLight = true;
	GlobalAmbientColor = InAmbientColor;
}
