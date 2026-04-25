#pragma once
#include "LightComponent.h"

class UDirectionalLightComponent : public ULightComponent
{
public:
    DECLARE_CLASS(UDirectionalLightComponent, ULightComponent)

    UDirectionalLightComponent();
    ~UDirectionalLightComponent() override = default;

    void GetEditableProperties(TArray<FPropertyDescriptor>& OutProps) override;
    void Serialize(FArchive& Ar) override;
    void PostDuplicate(UObject* Original) override;

	// ──────────── Cascade Shadow Map ────────────
	int32 GetCascadeCount() const { return CascadeCount; }
	float GetShadowDistance() const { return ShadowDistance; }
	FVector4 GetCascadeSplits() const { return CascadeSplits;  }

protected:
    FString GetVisualizationTexturePath() const override { return "Asset/Texture/Icons/S_LightDirectional.PNG"; }

private:
	int32 CascadeCount = 4;
	float ShadowDistance = 3000.0f;
	// 빛마다 Cascade Split을 다르게 조정할 있으므로 static constexpr로 선언하지 않는다.
	FVector4 CascadeSplits = { 0.067f, 0.133f, 0.267f, 1.0f };
};
