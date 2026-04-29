#pragma once
#include "Render/Common/ShadowTypes.h"

// Allocator는 딱 이런 데이터만 뱉어주도록 수정합니다.
struct FAtlasAllocationResult
{
    FShadowResource* AtlasResource; // 할당된 4096 거대 텍스처
    FVector2 UVOffset;              // 아틀라스 내 시작 위치 (예: 0.5, 0.0)
    FVector2 UVScale;               // 아틀라스 내 차지 비율 (예: 0.25, 0.25)
    uint32 ViewportX = 0u, ViewportY = 0u;    // 렌더링 시 필요한 Viewport 위치
    uint32 ViewportSize = 0u;
};

enum class ENodeState
{
    Free,
    Allocated,
    Split
};

struct FBuddyNode
{
    ENodeState State = ENodeState::Free;

    // 이 노드가 아틀라스 내에서 차지하는 픽셀 좌표와 크기
    uint32 X, Y;
    uint32 Size;

    // 자식 노드 (Split 상태일 때만 유효)
    // 0: TopLeft, 1: TopRight, 2: BottomLeft, 3: BottomRight
    FBuddyNode* Children[4];
};

class FShadowAtlasAllocator
{

public:
    bool Allocate(int inResoulution, FAtlasAllocationResult& outResult);
    void AddNewAtlasResource(FShadowResource* NewAtlasRes);
    void SetCurrentAtlasIndex(int Index) { CurrentShadowMapIndex = Index; }
    void Reset();
    FShadowMap& GetCurrentShadowMap() { return CurrentShadowMap; }
    int GetCurrentAtlasIndex() { return CurrentShadowMapIndex; }

private:
    void ClearTree(FBuddyNode* Node);
    FBuddyNode* AllocateRecursive(int inResoulution, FBuddyNode* CurrentNode);
    TArray<FShadowMap> ShadowMaps;
	FShadowMap CurrentShadowMap;
    FBuddyNode* CurrentRootBuddy = nullptr;
    int CurrentShadowMapIndex;
};
