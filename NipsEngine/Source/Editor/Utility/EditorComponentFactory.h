#pragma once

#include "Core/CoreMinimal.h"
#include "Engine/GameFramework/AActor.h"
#include <functional>

class UActorComponent;
// class AActor; (Included via AActor.h)

struct FComponentMenuEntry
{
    const char* DisplayName;
    const char* Category;
    std::function<UActorComponent*(AActor*)> Register;
};

// 컴포넌트 생성을 전담하는 헬퍼 구조체이고, EditorPropertyWidget에 저장된 배열을 던져주는 역할만 합니다.
struct FComponentFactory
{
    template<typename ComponentType>
    static UActorComponent* CreateDefault(AActor* Actor) { return Actor->AddComponent<ComponentType>(); }

    static UActorComponent* CreateSubUV(AActor* Actor);
    static UActorComponent* CreateTextRender(AActor* Actor);
    static UActorComponent* CreateBillboard(AActor* Actor);
    static UActorComponent* CreateHeightFog(AActor* Actor);

    static const TArray<FComponentMenuEntry>& GetMenuRegistry();
};