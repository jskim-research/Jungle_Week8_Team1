#pragma once
#include "Component/SceneComponent.h"
#include "Component/BillboardComponent.h"
#include "Render/Common/RenderTypes.h"
#include "GameFramework/World.h"

class ULightComponentBase : public USceneComponent
{
public:
    DECLARE_CLASS(ULightComponentBase, USceneComponent)

    ULightComponentBase();
    ~ULightComponentBase() override = default;

    void GetEditableProperties(TArray<FPropertyDescriptor>& OutProps) override;
    void PostEditProperty(const char* PropertyName) override;

    void PostDuplicate(UObject* Original) override;

    void Serialize(FArchive& Ar) override;

    void BeginPlay() override;
    void EndPlay() override;

    void OnRegister() override;
    void OnUnregister() override;

public:
    const FColor& GetLightColor() const { return LightColor; }
    float GetIntensity() const { return Intensity; }
    bool IsVisible() const { return bVisible; }

    void SetLightColor(const FColor& InColor) { LightColor = InColor; }
    void SetIntensity(float InIntensity) { Intensity = InIntensity; }
    void SetVisible(bool bInVisible) { bVisible = bInVisible; }
	
	const FLightHandle& GetLightHandle() const { return LightHandle; }
    void SetLightHandle(const FLightHandle& InLightHandle) { LightHandle = InLightHandle; }

private:
    FColor LightColor = FColor(1.0f, 1.0f, 1.0f, 1.0f);
    float Intensity = 1.0f;
    bool bVisible = true;

	FLightHandle LightHandle;
    UBillboardComponent* VisualizationComponent = nullptr;

protected:
    virtual FString GetVisualizationTexturePath() const { return {}; }
};

class ULightComponent : public ULightComponentBase
{
public:
    DECLARE_CLASS(ULightComponent, ULightComponentBase)

    ULightComponent();
    ~ULightComponent() override = default;

    void Serialize(FArchive& Ar) override;
    void PostDuplicate(UObject* Original) override;

public:
    ELightType GetLightType() const { return LightType; }

protected:
    void SetLightType(ELightType InLightType) { LightType = InLightType; }

private:
    ELightType LightType = ELightType::Max;
};
