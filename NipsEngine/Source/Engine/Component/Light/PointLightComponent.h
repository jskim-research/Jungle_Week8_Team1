#pragma once
#include "LightComponent.h"

class UPointLightComponent : public ULightComponent
{
public:
	DECLARE_CLASS(UPointLightComponent, ULightComponent)

	UPointLightComponent();
	~UPointLightComponent() override = default;

	void GetEditableProperties(TArray<FPropertyDescriptor>& OutProps) override;
	void Serialize(FArchive& Ar) override;
	void PostDuplicate(UObject* Original) override;
    // Shadow view builder override (캐스케이드/face 인덱스로 View/Projection 반환)
    bool BuildShadowView(uint32 CascadeIndex, FMatrix& OutView, FMatrix& OutProjection) const override;
public:
	float GetAttenuationRadius()    const { return AttenuationRadius; }
	float GetLightFalloffExponent() const { return LightFalloffExponent; }

	void SetAttenuationRadius(float InRadius)       { AttenuationRadius    = InRadius; }
	void SetLightFalloffExponent(float InExponent)  { LightFalloffExponent = InExponent; }

protected:
	FString GetVisualizationTexturePath() const override { return "Asset/Texture/Icons/S_LightPoint.PNG"; }

private:
	float AttenuationRadius    = 10.0f;
	float LightFalloffExponent = 1.0f;
};
