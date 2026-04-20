#include "LightComponent.h"
#include "Object/ObjectFactory.h"
#include "GameFramework/AActor.h"

DEFINE_CLASS(ULightComponentBase, USceneComponent)
REGISTER_FACTORY(ULightComponentBase)

DEFINE_CLASS(ULightComponent, ULightComponentBase)
REGISTER_FACTORY(ULightComponent)

ULightComponentBase::ULightComponentBase() = default;

void ULightComponentBase::GetEditableProperties(TArray<FPropertyDescriptor>& OutProps)
{
    USceneComponent::GetEditableProperties(OutProps);

    OutProps.push_back({ "LightColor", EPropertyType::Color, &LightColor });
    OutProps.push_back({ "Intensity", EPropertyType::Float, &Intensity });
    OutProps.push_back({ "Visible", EPropertyType::Bool, &bVisible });
}

void ULightComponentBase::PostEditProperty(const char* PropertyName)
{
    USceneComponent::PostEditProperty(PropertyName);
}

void ULightComponentBase::Serialize(FArchive& Ar)
{
    USceneComponent::Serialize(Ar);

    Ar << "LightColor" << LightColor;
    Ar << "Intensity" << Intensity;
    Ar << "Visible" << bVisible;
}

void ULightComponentBase::BeginPlay()
{
    USceneComponent::BeginPlay();
}

void ULightComponentBase::EndPlay()
{
    USceneComponent::EndPlay();
}

void ULightComponentBase::OnRegister()
{
    if (!Owner) { return; }
    UWorld* World = Owner->GetFocusedWorld();
    if (!World) { return; }

    World->RegisterLight(this);

    if (!VisualizationComponent)
    {
        FString TexturePath = GetVisualizationTexturePath();
        if (!TexturePath.empty())
        {
            VisualizationComponent = Owner->AddComponent<UBillboardComponent>();
            VisualizationComponent->SetIsVisualizationComponent(true);
            VisualizationComponent->SetTexturePath(TexturePath);
            VisualizationComponent->AttachToComponent(this);
        }
    }
}

void ULightComponentBase::OnUnregister()
{
    if (VisualizationComponent)
    {
        if (Owner)
        {
            Owner->RemoveComponent(VisualizationComponent);
        }
        VisualizationComponent = nullptr;
    }

    if (!Owner) { return; }
    UWorld* World = Owner->GetFocusedWorld();
    if (!World) { return; }

    World->UnregisterLight(this);
}

void ULightComponentBase::PostDuplicate(UObject* Original)
{
    USceneComponent::PostDuplicate(Original);

    const ULightComponentBase* Orig = Cast<ULightComponentBase>(Original);
    if (!Orig)
        return;

    LightColor = Orig->LightColor;
    Intensity = Orig->Intensity;
    bVisible = Orig->bVisible;
    LightHandle = Orig->LightHandle;
}

ULightComponent::ULightComponent() = default;

void ULightComponent::Serialize(FArchive& Ar)
{
    ULightComponentBase::Serialize(Ar);

    uint32 LightTypeValue = static_cast<uint32>(LightType);
    Ar << "LightType" << LightTypeValue;
    LightType = static_cast<ELightType>(LightTypeValue);
}

void ULightComponent::PostDuplicate(UObject* Original)
{
    ULightComponentBase::PostDuplicate(Original);

    const ULightComponent* Orig = Cast<ULightComponent>(Original);
    if (!Orig)
        return;

    LightType = Orig->LightType;
}
