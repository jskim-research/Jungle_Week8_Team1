#include "DirectionalLightComponent.h"
#include "Object/ObjectFactory.h"

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

