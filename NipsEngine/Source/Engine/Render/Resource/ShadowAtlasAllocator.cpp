#include "ShadowAtlasAllocator.h"

bool FShadowAtlasAllocator::Allocate(int inResolution, FAtlasAllocationResult& outResult)
{
    outResult.AtlasResource = CurrentShadowMap.Resource;

	FBuddyNode* AllocatedNode = nullptr;

	AllocatedNode = AllocateRecursive(inResolution, CurrentRootBuddy);

	if (!AllocatedNode)
        return false;

	outResult.AtlasResource = CurrentShadowMap.Resource;
    outResult.ViewportX = AllocatedNode->X;
    outResult.ViewportY = AllocatedNode->Y;
    outResult.ViewportSize = AllocatedNode->Size;

	float AtlasTotalSize = (float)CurrentRootBuddy->Size;
    outResult.UVScale = FVector2(AllocatedNode->Size / AtlasTotalSize, AllocatedNode->Size / AtlasTotalSize);
    outResult.UVOffset = FVector2(AllocatedNode->X / AtlasTotalSize, AllocatedNode->Y / AtlasTotalSize);

    return true;
}

void FShadowAtlasAllocator::AddNewAtlasResource(FShadowResource* NewAtlasRes)
{
    Reset();

    CurrentRootBuddy = nullptr;

    CurrentRootBuddy = new FBuddyNode();
    CurrentRootBuddy->Size = NewAtlasRes->Resolution;
    CurrentRootBuddy->X = 0;
    CurrentRootBuddy->Y = 0;
    CurrentRootBuddy->State = ENodeState::Free;
}

void FShadowAtlasAllocator::Reset()
{
    if (CurrentRootBuddy)
    {
        ClearTree(CurrentRootBuddy);
        CurrentRootBuddy = nullptr;
    }
}

void FShadowAtlasAllocator::ClearTree(FBuddyNode* Node)
{
    if (!Node)
        return;

    // 자식들이 있으면 재귀적으로 먼저 다 지움 (후위 순회)
    for (int i = 0; i < 4; i++)
    {
        if (Node->Children[i])
        {
            ClearTree(Node->Children[i]);
            Node->Children[i] = nullptr;
        }
    }

    // 내 자신을 삭제
    delete Node;
}

FBuddyNode* FShadowAtlasAllocator::AllocateRecursive(int inResolution, FBuddyNode* CurrentNode)
{
    if (!CurrentNode)
        return nullptr;

	if (CurrentNode->State == ENodeState::Allocated)
    {
        return nullptr;
    }

    else if (CurrentNode->State == ENodeState::Split)
    {
        for (int i = 0; i < 4; i++)
        {
            FBuddyNode* Node = AllocateRecursive(inResolution, CurrentNode->Children[i]);
            if (Node)
                return Node;
        }
        return nullptr;
	}

    else if(CurrentNode->State == ENodeState::Free)
    {
        if (CurrentNode->Size < inResolution)
            return nullptr;

        if (CurrentNode->Size == inResolution)
        {
			CurrentNode->State = ENodeState::Allocated;
			return CurrentNode;
        }
        else if (CurrentNode->Size >= inResolution)
        {
            CurrentNode->State = ENodeState::Split;

            for (int i = 0; i < 4; i++)
            {
                CurrentNode->Children[i] = new FBuddyNode();
                CurrentNode->Children[i]->Size = CurrentNode->Size / 2;
                CurrentNode->Children[i]->X = CurrentNode->X + (i % 2) * CurrentNode->Size / 2;
                CurrentNode->Children[i]->Y = CurrentNode->Y + (i / 2) * CurrentNode->Size / 2;
                CurrentNode->Children[i]->State = ENodeState::Free;

            }
            return AllocateRecursive(inResolution, CurrentNode->Children[0]);
        }
	}
}
