#include "LightComponent.h"
#include "Object/ObjectFactory.h"
#include "GameFramework/AActor.h"

DEFINE_CLASS(ULightComponentBase, USceneComponent)
REGISTER_FACTORY(ULightComponentBase)

void ULightComponentBase::GetEditableProperties(TArray<FPropertyDescriptor>& OutProps)
{
    USceneComponent::GetEditableProperties(OutProps);

    OutProps.push_back({ "LightColor", EPropertyType::Color, &LightColor });
    OutProps.push_back({ "Intensity", EPropertyType::Float, &Intensity, 0.0f, 20.0f, 0.1f });
    OutProps.push_back({ "Visible", EPropertyType::Bool, &bVisible });
	OutProps.push_back({ "Cast Shadows", EPropertyType::Bool, &bCastShadows });
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
	Ar << "CastShadows" << bCastShadows;
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

    LightHandle = FLightHandle();
    VisualizationComponent = nullptr;
}

DEFINE_CLASS(ULightComponent, ULightComponentBase)
REGISTER_FACTORY(ULightComponent)

ULightComponent::ULightComponent() = default;

void ULightComponent::GetEditableProperties(TArray<FPropertyDescriptor>& OutProps)
{
    ULightComponentBase::GetEditableProperties(OutProps);

	OutProps.push_back({ "Shadow Resolution Scale", EPropertyType::Float, &ShadowResolutionScale, 0.125f, 4.0f, 0.125f });
    OutProps.push_back({ "Shadow Bias", EPropertyType::Float, &ShadowBias, 0.0f, 1.0f, 0.001f });
    OutProps.push_back({ "Shadow Slope Bias", EPropertyType::Float, &ShadowSlopeBias, 0.0f, 5.0f, 0.01f });
    OutProps.push_back({ "Shadow Sharpen", EPropertyType::Float, &ShadowSharpen, 0.0f, 1.0f, 0.05f });

	// 참고: LightType은 사용자가 수정하지 못하도록 UI 노출에서 제외합니다.
}

void ULightComponent::Serialize(FArchive& Ar)
{
    ULightComponentBase::Serialize(Ar);

    uint32 LightTypeValue = static_cast<uint32>(LightType);
    Ar << "LightType" << LightTypeValue;
    LightType = static_cast<ELightType>(LightTypeValue); // 불러올 때만 사용되는 코드

	Ar << "ShadowResolutionScale" << ShadowResolutionScale;
	Ar << "ShadowBias" << ShadowBias;
	Ar << "ShadowSlopeBias" << ShadowSlopeBias;
	Ar << "ShadowSharpen" << ShadowSharpen;
}

void ULightComponent::PostDuplicate(UObject* Original)
{
    ULightComponentBase::PostDuplicate(Original);

    const ULightComponent* Orig = Cast<ULightComponent>(Original);
    if (!Orig)
        return;

    LightType = Orig->LightType;
}
