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

// 특수한 설정이 필요한 컴포넌트라면 아래와 같이 직접 설정해줍니다.
UActorComponent* FComponentFactory::CreateSubUV(AActor* Actor)
{
    auto* Comp = Actor->AddComponent<USubUVComponent>();
    Comp->SetParticle(FName("Explosion"));
    Comp->SetSpriteSize(2.0f, 2.0f);
    Comp->SetFrameRate(30.f);
    return Comp;
}

UActorComponent* FComponentFactory::CreateTextRender(AActor* Actor)
{
    auto* Comp = Actor->AddComponent<UTextRenderComponent>();
    Comp->SetFont(FName("Default"));
    Comp->SetText("TextRender");
    return Comp;
}

UActorComponent* FComponentFactory::CreateBillboard(AActor* Actor)
{
    auto* Comp = Actor->AddComponent<UBillboardComponent>();
    Comp->SetTexturePath("Asset/Texture/Pawn_64x.png");
    return Comp;
}

UActorComponent* FComponentFactory::CreateHeightFog(AActor* Actor)
{
    auto* Comp = Actor->AddComponent<UHeightFogComponent>();
    Comp->SetFogDensity(0);
    Comp->SetFogInscatteringColor(FVector4(0.72f, 0.8f, 0.9f, 1.0f));
    return Comp;
}

// EditorPropertyWidget에 출력될 데이터를 담고 있는 배열입니다. 이 리스트만 관리하면 됩니다.
const TArray<FComponentMenuEntry>& FComponentFactory::GetMenuRegistry()
{
    static const TArray<FComponentMenuEntry> Registry = {
        { "Scene Component", "Common", CreateDefault<USceneComponent> },
        { "StaticMesh Component", "Common", CreateDefault<UStaticMeshComponent> },
        { "SubUV Component", "Common", CreateSubUV },
        { "TextRender Component", "Common", CreateTextRender },
        { "Billboard Component", "Common", CreateBillboard },
        { "HeightFog Component", "Common", CreateHeightFog },
        { "SkyAtmosphere Component", "Common", CreateDefault<USkyAtmosphereComponent> },

        { "RotatingMovement Component", "Movement", CreateDefault<URotatingMovementComponent> },
        { "InterpToMovement Component", "Movement", CreateDefault<UInterpToMovementComponent> },
        { "PursuitMovement Component", "Movement", CreateDefault<UPursuitMovementComponent> },
        { "ProjectileMovement Component", "Movement", CreateDefault<UProjectileMovementComponent> },

        { "AmbientLight Component", "Light", CreateDefault<UAmbientLightComponent> },
        { "DirectionalLight Component", "Light", CreateDefault<UDirectionalLightComponent> },
        { "PointLight Component", "Light", CreateDefault<UPointLightComponent> },
        { "SpotLight Component", "Light", CreateDefault<USpotLightComponent> },
    };

    return Registry;
}