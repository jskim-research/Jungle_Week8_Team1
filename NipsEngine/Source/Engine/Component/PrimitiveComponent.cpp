#include "PrimitiveComponent.h"
#include "Engine/Geometry/Ray.h"
#include "Core/CollisionTypes.h"
#include "GameFramework/World.h"
#include "Math/Utils.h"

DEFINE_CLASS(UPrimitiveComponent, USceneComponent)

void UPrimitiveComponent::GetEditableProperties(TArray<FPropertyDescriptor>& OutProps)
{
	USceneComponent::GetEditableProperties(OutProps);

	OutProps.push_back({"Visible", EPropertyType::Bool, &bIsVisible});
	OutProps.push_back({"Enable Cull", EPropertyType::Bool, &bEnableCull});
}

void UPrimitiveComponent::PostEditProperty(const char* PropertyName)
{
	USceneComponent::PostEditProperty(PropertyName);
	NotifySpatialIndexDirty();
}

void UPrimitiveComponent::Serialize(FArchive& Ar)
{
	USceneComponent::Serialize(Ar);
	Ar << "Visible" << bIsVisible;
	Ar << "Enable Cull" << bEnableCull;
}

void UPrimitiveComponent::SetVisibility(bool bVisible)
{
	if (bIsVisible == bVisible)
	{
		return;
	}

	bIsVisible = bVisible;
	NotifySpatialIndexDirty();
}

bool UPrimitiveComponent::Raycast(const FRay& Ray, FHitResult& OutHitResult)
{
	if (!bIsVisible)
	{
		return false;
	}

	UpdateWorldAABB();

	float BoxT = 0.0f;
	if (!WorldAABB.IntersectRay(Ray, BoxT))
	{
		return false;
	}

	return RaycastMesh(Ray, OutHitResult);
}

bool UPrimitiveComponent::IntersectTriangle(const FVector& RayOrigin, const FVector& RayDir, const FVector& V0,
	const FVector& V1, const FVector& V2, float& OutT)
{
	const FVector Edge1 = V1 - V0;
	const FVector Edge2 = V2 - V0;

	const FVector PVec = FVector::CrossProduct(RayDir, Edge2);
	const float Det = FVector::DotProduct(Edge1, PVec);

	if (std::fabs(Det) < MathUtil::Epsilon)
	{
		return false;
	}

	const float InvDet = 1.0f / Det;
	const FVector TVec = RayOrigin - V0;

	const float U = FVector::DotProduct(TVec, PVec) * InvDet;
	if (U < 0.0f || U > 1.0f)
	{
		return false;
	}

	const FVector QVec = FVector::CrossProduct(TVec, Edge1);
	const float V = FVector::DotProduct(RayDir, QVec) * InvDet;
	if (V < 0.0f || (U + V) > 1.0f)
	{
		return false;
	}

	const float T = FVector::DotProduct(Edge2, QVec) * InvDet;
	if (T < 0.0f)
	{
		return false;
	}

	OutT = T;
	return true;
}

void UPrimitiveComponent::UpdateWorldMatrix() const
{
	USceneComponent::UpdateWorldMatrix();
	UpdateWorldAABB();
}

void UPrimitiveComponent::AddWorldOffset(const FVector& WorldDelta)
{
	USceneComponent::AddWorldOffset(WorldDelta);
	UpdateWorldAABB();
}

void UPrimitiveComponent::OnTransformDirty()
{
	NotifySpatialIndexDirty();
}

void UPrimitiveComponent::NotifySpatialIndexDirty() const
{
	AActor* Owner = GetOwner();
	if (Owner == nullptr)
	{
		return;
	}

	UWorld* World = Owner->GetFocusedWorld();
	if (World == nullptr)
	{
		return;
	}

	World->GetSpatialIndex().MarkPrimitiveDirty(const_cast<UPrimitiveComponent*>(this));
}
