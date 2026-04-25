#include "GameFramework/World.h"
#include "Component/Light/LightComponent.h"

DEFINE_CLASS(UWorld, UObject)
REGISTER_FACTORY(UWorld)

// FName, UUID 발급, 메모리 추적 등을 위해 UObjectManager를 통해 생성, 삭제한다.
UWorld::UWorld()
{
    PersistentLevel = UObjectManager::Get().CreateObject<ULevel>();
}

// 소멸 역시 UObjectManager를 통해 처리한다.
UWorld::~UWorld()
{
    SpatialIndex.Clear();
    UObjectManager::Get().DestroyObject(PersistentLevel);
}

/* @brief 비노출 필드를 복사하고, Level을 깊은 복사한 뒤, 복제된 액터들의 소속을 자기 자신으로 재설정합니다. */
void UWorld::PostDuplicate(UObject* Original)
{
    // UWorld 생성자가 기본 PersistentLevel을 생성하므로,
    // 원본의 레벨로 교체하기 전에 먼저 해제합니다.
    if (PersistentLevel)
    {
        UObjectManager::Get().DestroyObject(PersistentLevel);
        PersistentLevel = nullptr;
    }

    const UWorld* OrigWorld = Cast<UWorld>(Original);

    // 프로퍼티 시스템에 노출되지 않은 필드를 직접 복사합니다.
    WorldType      = OrigWorld->WorldType;
    ActiveCamera   = OrigWorld->ActiveCamera;
    bHasBegunPlay  = false; // 항상 미시작 상태로 시작

    // PersistentLevel 을 깊은 복사한 뒤, 복제된 액터들의 소속을 새 월드로 재설정합니다.
    if (OrigWorld->PersistentLevel)
    {
        PersistentLevel = Cast<ULevel>(OrigWorld->PersistentLevel->Duplicate());
        for (AActor* DuplicatedActor : PersistentLevel->GetActors())
        {
            if (DuplicatedActor)
            {
                DuplicatedActor->SetWorld(this);
            }
        }
    }

    RebuildSpatialIndex();
}

void UWorld::BeginPlay()
{
    bHasBegunPlay = true;
    PersistentLevel->BeginPlay();
    RebuildSpatialIndex();
}

void UWorld::Tick(float DeltaTime)
{
    if (!PersistentLevel)
        return;

	if (WorldType == EWorldType::Editor)
		PersistentLevel->TickEditor(DeltaTime);
	else
		PersistentLevel->TickGame(DeltaTime);

    SyncSpatialIndex();
}

void UWorld::EndPlay(EEndPlayReason::Type EndPlayReason)
{
    if (bHasBegunPlay)
    {
        bHasBegunPlay = false;
        PersistentLevel->EndPlay(EndPlayReason);
    }
}

void UWorld::RebuildSpatialIndex()
{
    SpatialIndex.Rebuild(this);
}

void UWorld::SyncSpatialIndex()
{
    SpatialIndex.FlushDirtyBounds();
}

FLightHandle UWorld::RegisterLight(ULightComponentBase* Comp)
{
    FLightHandle LightHandle;
    FLightSlot LightSlot;

	if (FreeLightSlotList.empty())
	{
		// 새로 생성
        uint32 Index = static_cast<uint32>(WorldLightSlots.size());
		LightSlot.LightData = Comp;
        LightSlot.Generation = 0;
        LightSlot.bAlive = true;

		WorldLightSlots.push_back(LightSlot);

        LightHandle.Index = Index;
        LightHandle.Generation = WorldLightSlots[Index].Generation;
	}
	else
	{
		// Free Slot 사용
        uint32 Index = FreeLightSlotList.back();
        FreeLightSlotList.pop_back();
        WorldLightSlots[Index].Generation += 1;
        WorldLightSlots[Index].LightData = Comp;
        WorldLightSlots[Index].bAlive = true;

        LightHandle.Index = Index;
        LightHandle.Generation = WorldLightSlots[Index].Generation;
	}

	Comp->SetLightHandle(LightHandle);

	return LightHandle;
}

void UWorld::UnregisterLight(ULightComponentBase* Comp)
{
    FLightHandle LightHandle = Comp->GetLightHandle();
	// LightHandle이 없거나, 해당 Slot에 다른 데이터가 들어가 있으면 등록 해제 취소
    if (!LightHandle.IsValid() || WorldLightSlots[LightHandle.Index].Generation != LightHandle.Generation)
    {
        return;
    }

	WorldLightSlots[LightHandle.Index].bAlive = false;
    WorldLightSlots[LightHandle.Index].LightData = nullptr;
    FreeLightSlotList.push_back(LightHandle.Index);
}
