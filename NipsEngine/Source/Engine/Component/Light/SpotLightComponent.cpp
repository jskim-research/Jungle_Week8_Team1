#include "SpotLightComponent.h"
#include "Object/ObjectFactory.h"
#include "Render/Renderer/RenderFlow/RenderPassContext.h"

DEFINE_CLASS(USpotLightComponent, UPointLightComponent)
REGISTER_FACTORY(USpotLightComponent)

USpotLightComponent::USpotLightComponent()
{
	SetLightType(ELightType::LightType_Spot);
	bPSM = true;
}

// Spot light shadow view generation (single cascade index)
bool USpotLightComponent::BuildShadowView(uint32 CascadeIndex, FMatrix& OutView, FMatrix& OutProjection) const
{
    (void)CascadeIndex; // spots typically use single projection

    FVector Eye = GetWorldLocation();
    FVector LightDir = GetUpVector() * -1.0f;
    FVector Target = Eye + LightDir;
    FVector Up = FVector(0, 0, 1);
    if (abs(FVector::DotProduct(LightDir, Up)) > 0.99f)
    {
        Up = FVector(1, 0, 0);
    }

    OutView = FMatrix::MakeViewLookAtLH(Eye, Target, Up);

    float OuterAngleRad = (OuterConeAngle * (3.141592f / 180.0f)) * 0.5f; // half-angle
    float FovRad = OuterAngleRad * 2.0f;

    float NearZ = 0.1f;
    float FarZ = std::max(GetAttenuationRadius(), NearZ + 0.1f);

    OutProjection = FMatrix::MakePerspectiveFovLH(FovRad, 1.0f, NearZ, FarZ);

    return true;
}

void USpotLightComponent::GetEditableProperties(TArray<FPropertyDescriptor>& OutProps)
{
	UPointLightComponent::GetEditableProperties(OutProps);

	OutProps.push_back({ "Inner Cone Angle", EPropertyType::Float, &InnerConeAngle, 0.0f, 80.0f, 0.1f });
	OutProps.push_back({ "Outer Cone Angle", EPropertyType::Float, &OuterConeAngle, 0.0f, 80.0f, 0.1f });
    OutProps.push_back({ "Apply PSM", EPropertyType::Bool, &bPSM });
    if (bPSM) OutProps.push_back({ "Camera Slider Back", EPropertyType::Float, &CameraSliderBack });
}

void USpotLightComponent::PostEditProperty(const char* PropertyName)
{
	UPointLightComponent::PostEditProperty(PropertyName);

	if (strcmp(PropertyName, "Inner Cone Angle") == 0)
	{
		if (InnerConeAngle > OuterConeAngle)
		{
			OuterConeAngle = InnerConeAngle;
		}
	}
	else if (strcmp(PropertyName, "Outer Cone Angle") == 0)
	{
		if (OuterConeAngle < InnerConeAngle)
		{
			InnerConeAngle = OuterConeAngle;
		}
	}
}

void USpotLightComponent::Serialize(FArchive& Ar)
{
	UPointLightComponent::Serialize(Ar);

	Ar << "InnerConeAngle" << InnerConeAngle;
	Ar << "OuterConeAngle" << OuterConeAngle;
}

void USpotLightComponent::PostDuplicate(UObject* Original)
{
	UPointLightComponent::PostDuplicate(Original);

	const USpotLightComponent* Orig = Cast<USpotLightComponent>(Original);
	if (!Orig)
	{
		return;
	}

	InnerConeAngle = Orig->InnerConeAngle;
	OuterConeAngle = Orig->OuterConeAngle;
}

