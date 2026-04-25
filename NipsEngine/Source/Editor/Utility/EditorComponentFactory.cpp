#include "EditorComponentFactory.h"

#include "Engine/GameFramework/AActor.h"
#include "Component/StaticMeshComponent.h"
#include "Component/BillboardComponent.h"
#include "Component/TextRenderComponent.h"
#include "Component/SubUVComponent.h"
#include "Component/GizmoComponent.h"
#include "Component/Movement/RotatingMovementComponent.h"
#include "Component/Movement/ProjectileMovementComponent.h"
#include "Component/Movement/InterpToMovementComponent.h"
#include "Component/Movement/PursuitMovementComponent.h"
#include "Component/SkyAtmosphereComponent.h"
#include "Selection/SelectionManager.h"
#include "Component/HeightFogComponent.h"
#include "Component/Light/AmbientLightComponent.h"
#include "Component/Light/DirectionalLightComponent.h"
#include "Component/Light/PointLightComponent.h"
#include "Component/Light/SpotLightComponent.h"

// 새로운 컴포넌트를 레지스트리에 등록합니다. 특수한 설정이 필요한 컴포넌트는 직접 설정합니다.
template<typename ComponentType>
UActorComponent* FEditorComponentFactory::Register(AActor* Actor)
{
    return Actor->AddComponent<ComponentType>();
}

template <>
UActorComponent* FEditorComponentFactory::Register<USubUVComponent>(AActor* Actor)
{
    auto* Comp = Actor->AddComponent<USubUVComponent>();
    Comp->SetParticle(FName("Explosion"));
    Comp->SetSpriteSize(2.0f, 2.0f);
    Comp->SetFrameRate(30.f);
    return Comp;
}

template <>
UActorComponent* FEditorComponentFactory::Register<UTextRenderComponent>(AActor* Actor)
{
    auto* Comp = Actor->AddComponent<UTextRenderComponent>();
    Comp->SetFont(FName("Default"));
    Comp->SetText("TextRender");
    return Comp;
}

template <>
UActorComponent* FEditorComponentFactory::Register<UBillboardComponent>(AActor* Actor)
{
    auto* Comp = Actor->AddComponent<UBillboardComponent>();
    Comp->SetTexturePath("Asset/Texture/Pawn_64x.png");
    return Comp;
}

template <>
UActorComponent* FEditorComponentFactory::Register<UHeightFogComponent>(AActor* Actor)
{
    auto* Comp = Actor->AddComponent<UHeightFogComponent>();
    Comp->SetFogDensity(0);
    Comp->SetFogInscatteringColor(FVector4(0.72f, 0.8f, 0.9f, 1.0f));
    return Comp;
}

// EditorPropertyWidget에 출력될 데이터를 담고 있는 배열입니다. 이 리스트만 관리하면 됩니다.
const TArray<FComponentMenuEntry>& FEditorComponentFactory::GetMenuRegistry()
{
    static const TArray<FComponentMenuEntry> Registry = {
        { "Scene Component", "Common", Register<USceneComponent> },
        { "StaticMesh Component", "Common", Register<UStaticMeshComponent> },
        { "SubUV Component", "Common", Register<USubUVComponent> },
        { "TextRender Component", "Common", Register<UTextRenderComponent> },
        { "Billboard Component", "Common", Register<UBillboardComponent> },
        { "HeightFog Component", "Common", Register<UHeightFogComponent> },
        { "SkyAtmosphere Component", "Common", Register<USkyAtmosphereComponent> },

        { "RotatingMovement Component", "Movement", Register<URotatingMovementComponent> },
        { "InterpToMovement Component", "Movement", Register<UInterpToMovementComponent> },
        { "PursuitMovement Component", "Movement", Register<UPursuitMovementComponent> },
        { "ProjectileMovement Component", "Movement", Register<UProjectileMovementComponent> },

        { "AmbientLight Component", "Light", Register<UAmbientLightComponent> },
        { "DirectionalLight Component", "Light", Register<UDirectionalLightComponent> },
        { "PointLight Component", "Light", Register<UPointLightComponent> },
        { "SpotLight Component", "Light", Register<USpotLightComponent> },
    };

    return Registry;
}