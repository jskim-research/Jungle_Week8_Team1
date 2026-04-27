#pragma once

class UPrimitiveComponent;

struct FHitResult 
{
	UPrimitiveComponent* HitComponent = nullptr;

	float Distance = FLT_MAX;
	
	//	World 기준
	FVector Location = { 0, 0, 0 };
	FVector Normal = { 0, 0, 0 };
	
	int32 FaceIndex = -1;

	bool bHit = false;
	
	void Reset()
	{
		HitComponent = nullptr;
		Distance = FLT_MAX;
		Location = { 0, 0, 0 };
		Normal = { 0, 0, 0 };
		FaceIndex = -1;
		bHit = false;
	}
	
	bool IsValid() const
	{
		return bHit && (HitComponent != nullptr);
	}
};
