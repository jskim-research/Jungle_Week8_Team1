#include "DirectionalLightComponent.h"
#include "Object/ObjectFactory.h"
#include "Render/Renderer/RenderFlow/RenderPassContext.h"

DEFINE_CLASS(UDirectionalLightComponent, ULightComponent)
REGISTER_FACTORY(UDirectionalLightComponent)

UDirectionalLightComponent::UDirectionalLightComponent()
{
	SetLightType(ELightType::LightType_Directional);
}

void UDirectionalLightComponent::GetEditableProperties(TArray<FPropertyDescriptor>& OutProps)
{
	ULightComponent::GetEditableProperties(OutProps);
	OutProps.push_back({ "Cascade Count", EPropertyType::Int, &CascadeCount });
	OutProps.push_back({ "Shadow Distance", EPropertyType::Float, &CascadeCount, 1000.0f, 30000.0f, 100.0f });
	OutProps.push_back({ "Cascade Splits", EPropertyType::Vec4, &CascadeCount });
}

void UDirectionalLightComponent::Serialize(FArchive& Ar)
{
	ULightComponent::Serialize(Ar);
	Ar << "CascadeCount" << CascadeCount;
	Ar << "ShadowDistance" << ShadowDistance;
	Ar << "CascadeSplits" << CascadeSplits;
}

void UDirectionalLightComponent::PostDuplicate(UObject* Original)
{
	ULightComponent::PostDuplicate(Original);
}

// Directional light shadow view generation (CSM-like)
bool UDirectionalLightComponent::BuildShadowView(uint32 CascadeIndex, FMatrix& OutView, FMatrix& OutProjection) const
{
    if (CascadeIndex >= static_cast<uint32>(CascadeCount))
        return false;

    // compute near/far for this cascade using CascadeSplits (fractions)
    auto GetSplitAt = [&](int idx) -> float {
        switch (idx)
        {
        case 0: return CascadeSplits.X;
        case 1: return CascadeSplits.Y;
        case 2: return CascadeSplits.Z;
        case 3: return CascadeSplits.W;
        default: return CascadeSplits.W;
        }
    };

    float PrevSplit = (CascadeIndex == 0) ? 0.0f : GetSplitAt((int)CascadeIndex - 1);
    float ThisSplit = GetSplitAt((int)CascadeIndex);

    float Near = PrevSplit * ShadowDistance;
    float Far = ThisSplit * ShadowDistance;
    if (Near <= 0.0f) Near = 1.0f;

    // Use light direction and world location to build a simple orthographic projection
    FVector LightDir = GetForwardVector().GetSafeNormal();
    FVector Center = GetWorldLocation();

    FVector Up = FVector::UpVector;
    if (std::abs(FVector::DotProduct(LightDir, Up)) > 0.99f) Up = FVector::RightVector;

    // Place eye a bit back along light direction
    FVector Eye = Center - LightDir * (ShadowDistance * 0.5f);
    OutView = FMatrix::MakeViewLookAtLH(Eye, Center, Up);

    // Ortho size - base it on cascade range
    float Range = std::max(1.0f, Far - Near);
    float OrthoSize = Range * 0.5f + ShadowDistance * 0.25f;
    OutProjection = FMatrix::MakeOrthographicLH(OrthoSize * 2.0f, OrthoSize * 2.0f, -ShadowDistance, ShadowDistance);

    return true;
}

