#include "PointLightComponent.h"
#include "Object/ObjectFactory.h"
#include "Render/Renderer/RenderFlow/RenderPassContext.h"

DEFINE_CLASS(UPointLightComponent, ULightComponent)
REGISTER_FACTORY(UPointLightComponent)

UPointLightComponent::UPointLightComponent()
{
	SetLightType(ELightType::LightType_Point);
}

void UPointLightComponent::GetEditableProperties(TArray<FPropertyDescriptor>& OutProps)
{
	ULightComponent::GetEditableProperties(OutProps);

	OutProps.push_back({ "Attenuation Radius",     EPropertyType::Float, &AttenuationRadius,    0.0f,  10000.0f, 1.0f });
	OutProps.push_back({ "Light Falloff Exponent", EPropertyType::Float, &LightFalloffExponent, 0.01f, 16.0f,    0.01f });
}

void UPointLightComponent::Serialize(FArchive& Ar)
{
	ULightComponent::Serialize(Ar);

	Ar << "AttenuationRadius"    << AttenuationRadius;
	Ar << "LightFalloffExponent" << LightFalloffExponent;
}

void UPointLightComponent::PostDuplicate(UObject* Original)
{
	ULightComponent::PostDuplicate(Original);

	const UPointLightComponent* Orig = Cast<UPointLightComponent>(Original);
	if (!Orig) return;

	AttenuationRadius    = Orig->AttenuationRadius;
	LightFalloffExponent = Orig->LightFalloffExponent;
}

// Point light cube-face shadow view (single face by index)
bool UPointLightComponent::BuildShadowView(uint32 CascadeIndex, FMatrix& OutView, FMatrix& OutProjection) const
{
    if (CascadeIndex >= 6)
        return false;

    static const FVector CubeDirs[6] = {
        FVector::ForwardVector,
        -FVector::ForwardVector,
        FVector::RightVector,
        -FVector::RightVector,
        FVector::UpVector,
        -FVector::UpVector
    };
    static const FVector CubeUps[6] = {
        FVector::RightVector,
        FVector::RightVector,
        -FVector::UpVector,
        FVector::UpVector,
        FVector::RightVector,
        FVector::RightVector
    };

    FVector Eye = GetWorldLocation();
    FVector Target = Eye + CubeDirs[CascadeIndex];
    FVector Up = CubeUps[CascadeIndex];

    OutView = FMatrix::MakeViewLookAtLH(Eye, Target, Up);

    float FovRad = (90.0f * (3.141592f / 180.0f));
    float NearZ = 0.1f;
    float FarZ = std::max(AttenuationRadius, NearZ + 0.1f);

    OutProjection = FMatrix::MakePerspectiveFovLH(FovRad, 1.0f, NearZ, FarZ);

    return true;
}

