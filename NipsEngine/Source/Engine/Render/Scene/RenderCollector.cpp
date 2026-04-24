#include "RenderCollector.h"

#include "Render/LineBatcher.h"
#include "GameFramework/World.h"
#include "GameFramework/AActor.h"
#include "Object/ActorIterator.h"
#include "Component/BillboardComponent.h"
#include "Component/PrimitiveComponent.h"
#include "Component/StaticMeshComponent.h"
#include "Component/GizmoComponent.h"
#include "Component/TextRenderComponent.h"
#include "Component/SubUVComponent.h"
#include "Component/DecalComponent.h"
#include "Component/HeightFogComponent.h"
#include "Component/SkyAtmosphereComponent.h"
#include "Component/Light/AmbientLightComponent.h"
#include "Component/Light/DirectionalLightComponent.h"
#include "Component/Light/PointLightComponent.h"
#include "Component/Light/SpotLightComponent.h"
#include "Core/ResourceManager.h"
#include "Engine/Geometry/Frustum.h"
#include "Engine/Asset/StaticMesh.h"
#include "Engine/GameFramework/PrimitiveActors.h"
#include "Render/Resource/Material.h"
#include "Math/Utils.h"
#include "Object/ObjectIterator.h"
#include "Runtime/Stats/ScopeCycleCounter.h"
#include <algorithm>
#include <unordered_set>

namespace
{
	FVector MakeLightColorVector(const ULightComponentBase* LightComponent)
	{
		if (LightComponent == nullptr)
		{
			return FVector::ZeroVector;
		}

		const FColor& LightColor = LightComponent->GetLightColor();
		return FVector(LightColor.r, LightColor.g, LightColor.b);
	}

	FColor MakeBVHInternalNodeColor(int32 PathIndexFromLeaf, int32 PathLength)
	{
		if (PathLength <= 1)
		{
			return FColor::Yellow();
		}

		const float T = static_cast<float>(PathIndexFromLeaf) / static_cast<float>(PathLength - 1);
		return FColor::Lerp(FColor::Cyan(), FColor::Yellow(), T);
	}

	bool UsesCameraDependentRenderBounds(const UPrimitiveComponent* PrimitiveComponent)
	{
		if (PrimitiveComponent == nullptr)
		{
			return false;
		}

		switch (PrimitiveComponent->GetPrimitiveType())
		{
		case EPrimitiveType::EPT_Billboard:
		case EPrimitiveType::EPT_Text:
		case EPrimitiveType::EPT_SubUV:
			return true;
		default:
			return false;
		}
	}

	FMatrix MakeViewBillboardMatrix(const UPrimitiveComponent* Primitive, const FRenderBus& RenderBus)
	{
		const FMatrix WorldMatrix = Primitive->GetWorldMatrix();
		return UBillboardComponent::MakeBillboardWorldMatrix(
			WorldMatrix.GetOrigin(),
			WorldMatrix.GetScaleVector(),
			RenderBus.GetCameraForward(),
			RenderBus.GetCameraRight(),
			RenderBus.GetCameraUp());
	}

	FMatrix MakeViewSubUVSelectionMatrix(const USubUVComponent* SubUVComp, const FRenderBus& RenderBus)
	{
		const FVector WorldScale = SubUVComp->GetWorldScale();
		return USubUVComponent::MakeBillboardWorldMatrix(
			SubUVComp->GetWorldLocation(),
			FVector(
				WorldScale.X > 0.01f ? WorldScale.X : 0.01f,
				SubUVComp->GetWidth() * WorldScale.Y,
				SubUVComp->GetHeight() * WorldScale.Z),
			RenderBus.GetCameraForward(),
			RenderBus.GetCameraRight(),
			RenderBus.GetCameraUp());
	}
	/*
	* BillBoardComponent를 상속받은 text, SubUV가 사용하는 AABB 계산함수(의존성 분리)
	*/
	FAABB BuildQuadAABB(const FMatrix& WorldMatrix)
	{
		static constexpr FVector LocalQuadCorners[4] =
		{
			FVector(0.0f, -0.5f,  0.5f),
			FVector(0.0f,  0.5f,  0.5f),
			FVector(0.0f,  0.5f, -0.5f),
			FVector(0.0f, -0.5f, -0.5f)
		};

		FAABB Box;
		Box.Reset();

		for (const FVector& Corner : LocalQuadCorners)
		{
			Box.Expand(WorldMatrix.TransformPosition(Corner));
		}

		return Box;
	}

	FAABB BuildRenderAABB(const UPrimitiveComponent* PrimitiveComponent, const FRenderBus& RenderBus)
	{
		switch (PrimitiveComponent->GetPrimitiveType())
		{
		case EPrimitiveType::EPT_Billboard:
        {
            const UBillboardComponent* BillboardComponent = static_cast<const UBillboardComponent*>(PrimitiveComponent);
            return BuildQuadAABB(UBillboardComponent::MakeBillboardWorldMatrix(
                BillboardComponent->GetWorldLocation(),
                FVector(0.00f, BillboardComponent->GetWidth(), BillboardComponent->GetHeight()),
                RenderBus.GetCameraForward(),
                RenderBus.GetCameraRight(),
                RenderBus.GetCameraUp()));
        }
		case EPrimitiveType::EPT_Text:
		{
			const UTextRenderComponent* TextComp = static_cast<const UTextRenderComponent*>(PrimitiveComponent);
			return BuildQuadAABB(TextComp->GetTextMatrix());
		}
		case EPrimitiveType::EPT_SubUV:
		{
			const USubUVComponent* SubUVComp = static_cast<const USubUVComponent*>(PrimitiveComponent);
			return BuildQuadAABB(MakeViewSubUVSelectionMatrix(SubUVComp, RenderBus));
		}

		default:
			return PrimitiveComponent->GetWorldAABB();
		}
	}

	int32 SelectLODLevel(const FVector& CameraPos, const FAABB& Bounds, const FMatrix& ProjMatrix, int32 ValidLODCount)
	{
		bool IsOrthoGraphic = (std::abs(ProjMatrix.M[3][3] - 1.0f) < 1e-4f);
		if (ValidLODCount <= 1 || IsOrthoGraphic) return 0;

		// 1. 바운딩 박스를 통해 바운딩 스피어 반지름 및 카메라와의 거리 계산
		const FVector Center = (Bounds.Min + Bounds.Max) * 0.5f;
		const FVector Extent = (Bounds.Max - Bounds.Min) * 0.5f;
		const float SphereRadius = std::sqrt(Extent.X * Extent.X + Extent.Y * Extent.Y + Extent.Z * Extent.Z);

		const FVector Diff = Center - CameraPos;
		const float Dist = std::sqrt(Diff.X * Diff.X + Diff.Y * Diff.Y + Diff.Z * Diff.Z);

		if (Dist <= 1e-4f) return 0;

		const float ProjectedRadius = (SphereRadius / Dist) * ProjMatrix.M[2][1];
		const float ScreenCoverage = ProjectedRadius; 

		static constexpr float Thresholds[] = { 0.15f, 0.08f, 0.05f, 0.02f };
		static constexpr int32 ThresholdCount = static_cast<int32>(sizeof(Thresholds) / sizeof(Thresholds[0]));

		const int32 MaxLOD = ValidLODCount - 1;
		for (int32 LOD = 0; LOD < MaxLOD; ++LOD)
		{
			float Threshold = (LOD < ThresholdCount) ? Thresholds[LOD] : 0.0f;
			if (ScreenCoverage >= Threshold)
				return LOD;
		}

		// 화면에 차지하는 비율이 가장 낮을 경우 최하위 LOD 반환
		return MaxLOD;
	}
}

void FRenderCollector::CollectWorld(UWorld* World, const FShowFlags& ShowFlags, EViewMode ViewMode, FRenderBus& RenderBus,
	const FFrustum* ViewFrustum)
{
	ResetCullingStats();
	ResetDecalStats();

	if (!World) return;
	
	CollectLight(World, RenderBus, ViewFrustum);

	if (ViewFrustum)
	{
		VisiblePrimitiveScratch.clear();
		World->GetSpatialIndex().FrustumQueryPrimitives(*ViewFrustum, VisiblePrimitiveScratch, FrustumQueryScratch);

		for (UPrimitiveComponent* Primitive : VisiblePrimitiveScratch)
		{
			if (Primitive == nullptr || UsesCameraDependentRenderBounds(Primitive) || !Primitive->IsEnableCull()) continue;
			++LastCullingStats.BVHPassedPrimitiveCount;
			CollectFromComponent(Primitive, ShowFlags, ViewMode, RenderBus, World->GetWorldType());
		}
	}

	std::unordered_set<UPrimitiveComponent*> CollectCameraDependentPrimitives;
	if (ViewFrustum)
	{
		CollectCameraDependentPrimitives.reserve(32);
	}
	
	// Frustum이 없다면 액터 단위로 통째로 수집하고, 그렇지 않다면 BVH에서 누락된 컴포넌트들을 개별 수집
	for (TActorIterator<AActor> Iter(World); Iter; ++Iter)
	{
		AActor* Actor = *Iter;
		if (!Actor || !Actor->IsVisible()) continue;

		if (!ViewFrustum)
		{
			for (UPrimitiveComponent* Primitive : Actor->GetPrimitiveComponents())
			{	
				if (Primitive != nullptr && Primitive->IsVisible())
				{
					++LastCullingStats.TotalVisiblePrimitiveCount;
				}
			}
			CollectFromActor(Actor, ShowFlags, ViewMode, RenderBus, World->GetWorldType());
			continue; // early-continue
		}

		// 이미 처리된 컴포넌트, 중복된 컴포넌트는 제외하고 Frustum Culling 수행
		for (UPrimitiveComponent* Primitive : Actor->GetPrimitiveComponents())
		{
			if (!Primitive || Primitive->IsVisible()) continue;

			++LastCullingStats.TotalVisiblePrimitiveCount;

			const bool bIsCameraDependent = UsesCameraDependentRenderBounds(Primitive);
			if (!bIsCameraDependent && Primitive->IsEnableCull()) continue;
			if (!CollectCameraDependentPrimitives.insert(Primitive).second) continue;

			if (bIsCameraDependent && Primitive->IsEnableCull())
			{
				if (ViewFrustum->Intersects(BuildRenderAABB(Primitive, RenderBus)) == FFrustum::EFrustumIntersectResult::Outside)
					continue;
			}

			++LastCullingStats.FallbackPassedPrimitiveCount;
            CollectFromComponent(Primitive, ShowFlags, ViewMode, RenderBus, World->GetWorldType());
		}
	}
}

void FRenderCollector::ResetCullingStats()
{
	LastCullingStats = {};
}

void FRenderCollector::ResetDecalStats()
{
	LastDecalStats = {};
}

// 조명을 Frustum Culling을 통해 수집한다.
void FRenderCollector::CollectLight(UWorld* World, FRenderBus& RenderBus, const FFrustum* ViewFrustum)
{
    const TArray<FLightSlot>& LightSlots = World->GetWorldLightSlots();

	for (const FLightSlot& Slot : LightSlots)
	{
        if (!Slot.bAlive || !Slot.LightData)
            continue;

		const ULightComponent* LightComponent = Cast<ULightComponent>(Slot.LightData);
		if (LightComponent == nullptr || !LightComponent->IsVisible())
		{
			continue;
		}

		FRenderLight RenderLight = {};
		RenderLight.Type = static_cast<uint32>(LightComponent->GetLightType());
		RenderLight.Color = MakeLightColorVector(LightComponent);
		RenderLight.Intensity = LightComponent->GetIntensity();

		switch (LightComponent->GetLightType())
		{
		case ELightType::LightType_AmbientLight:
		{
			RenderBus.AddLight(RenderLight);
			break;
		}

		case ELightType::LightType_Directional:
		{
			FVector DirectionToLight = (LightComponent->GetForwardVector() * -1.0f);
			DirectionToLight.Normalize();
			RenderLight.Direction = DirectionToLight;
			RenderBus.AddLight(RenderLight);

			// TODO: PIE에서도 화살표를 보여주고 있음.. PIE 월드를 감지할 필요가 있다.
            LineBatcher->AddDirectionalLight(
                LightComponent->GetWorldLocation(),
                RenderLight.Direction * -1.0f,
                LightComponent->GetRightVector(),
				LightComponent->GetLightColor().ToVector4()
			);
            break;
		}

		case ELightType::LightType_Point:
		{
			const UPointLightComponent* PointLight = Cast<UPointLightComponent>(LightComponent);
			if (PointLight == nullptr)
			{
				continue;
			}
			
			// View Frustum에 대한 Bounding Sphere 교차 검사
			if (ViewFrustum)
			{
				FVector Center = PointLight->GetWorldLocation();
				float Radius = PointLight->GetAttenuationRadius();
				
				if (!ViewFrustum->IntersectsBoundingSphere(Center, Radius))
					continue;
			}

			RenderLight.Position = PointLight->GetWorldLocation();
			RenderLight.Radius = PointLight->GetAttenuationRadius();
			RenderLight.FalloffExponent = PointLight->GetLightFalloffExponent();
			RenderBus.AddLight(RenderLight);
			break;
		}

		case ELightType::LightType_Spot:
		{
			const USpotLightComponent* SpotLight = Cast<USpotLightComponent>(LightComponent);
			if (SpotLight == nullptr)
			{
				continue;
			}
			
			// View Frustum에 대한 Bounding Sphere 교차 검사
			if (ViewFrustum)
			{
				FVector Center = SpotLight->GetWorldLocation();
				float Radius = SpotLight->GetAttenuationRadius();

				if (!ViewFrustum->IntersectsBoundingSphere(Center, Radius))
					continue;
			}

			const float InnerAngleDegrees = MathUtil::Clamp(SpotLight->GetInnerConeAngle(), 0.0f, 89.0f);
			const float OuterAngleDegrees = MathUtil::Clamp((std::max)(SpotLight->GetOuterConeAngle(), InnerAngleDegrees), 0.0f, 89.0f);

			// -z 축을 forward로 사용
			FVector LightDirection = SpotLight->GetUpVector() * -1.0f;
			LightDirection.Normalize();

			RenderLight.Position = SpotLight->GetWorldLocation();
			RenderLight.Direction = LightDirection;
			RenderLight.Radius = SpotLight->GetAttenuationRadius();
			RenderLight.FalloffExponent = SpotLight->GetLightFalloffExponent();
			RenderLight.SpotInnerCos = std::cos(MathUtil::DegreesToRadians(InnerAngleDegrees));
			RenderLight.SpotOuterCos = std::cos(MathUtil::DegreesToRadians(OuterAngleDegrees));
			RenderBus.AddLight(RenderLight);
			break;
		}

		default:
			break;
		}
	}
}

void FRenderCollector::CollectSelection(const TArray<AActor*>& SelectedActors, const FShowFlags& ShowFlags, EViewMode ViewMode, FRenderBus& RenderBus)
{
	bool bHasSelectionMask = false;
	for (AActor* Actor : SelectedActors)
	{
		bHasSelectionMask |= CollectFromSelectedActor(Actor, ShowFlags, ViewMode, RenderBus);
	}

	if (bHasSelectionMask)
	{
		FRenderCommand PostProcessCmd = {};
		PostProcessCmd.Type = ERenderCommandType::PostProcessOutline;
		PostProcessCmd.Material = FResourceManager::Get().GetMaterial("OutlineMaterial");

		UMaterial* Material = Cast<UMaterial>(PostProcessCmd.Material);
		Material->SetVector2("OutlineViewportSize", RenderBus.GetViewportSize());
        Material->SetVector2("OutlineViewportOrigin", RenderBus.GetViewportOrigin());
		Material->DepthStencilType = EDepthStencilType::Default;
		Material->RasterizerType = ERasterizerType::SolidBackCull;
		Material->BlendType = EBlendType::AlphaBlend;

		RenderBus.AddCommand(ERenderPass::PostProcessOutline, PostProcessCmd);
	}
}

void FRenderCollector::CollectGrid(float GridSpacing, int32 GridHalfLineCount, FRenderBus& RenderBus, bool bOrthographic)
{
	FRenderCommand Cmd = {};
	Cmd.Type = ERenderCommandType::Grid;
	Cmd.Constants.Grid.GridSpacing = GridSpacing;
	Cmd.Constants.Grid.GridHalfLineCount = GridHalfLineCount;
	Cmd.Constants.Grid.bOrthographic = bOrthographic;
	RenderBus.AddCommand(ERenderPass::Grid, Cmd);
}

void FRenderCollector::CollectGizmo(UGizmoComponent* Gizmo, const FShowFlags& ShowFlags, FRenderBus& RenderBus, bool bIsActiveOperation)
{
	if (ShowFlags.bGizmo == false) return;
	if (!Gizmo || !Gizmo->IsVisible()) return;

	FMeshBuffer* GizmoMesh = &MeshBufferManager.GetMeshBuffer(Gizmo->GetPrimitiveType());
	FMatrix WorldMatrix = Gizmo->GetWorldMatrix();
	bool bHolding = Gizmo->IsHolding();
	int32 SelectedAxis = Gizmo->GetSelectedAxis();

	auto CreateGizmoCmd = [&](bool bInner) {
		FRenderCommand Cmd = {};
		Cmd.Type = ERenderCommandType::Gizmo;
		Cmd.MeshBuffer = GizmoMesh;

		Cmd.SectionIndexStart = 0;
		Cmd.SectionIndexCount = GizmoMesh->GetIndexBuffer().GetIndexCount();

		Cmd.PerObjectConstants = FPerObjectConstants{ WorldMatrix };

		UMaterial* Material = Cast<UMaterial>(Gizmo->GetMaterial());
		Cmd.Material = Material;

		if (bInner)
		{
			Material->DepthStencilType = EDepthStencilType::GizmoInside;
			Material->BlendType = EBlendType::AlphaBlend;
		}
		else
		{
			Material->DepthStencilType = EDepthStencilType::GizmoOutside;
			Material->BlendType = EBlendType::Opaque;
		}

		Material->SetVector4("GizmoColorTint", FVector4(1.0f, 1.0f, 1.0f, 1.0f));
		Material->SetBool("bIsInnerGizmo", bInner);
		Material->SetBool("bClicking", bHolding);
		Material->SetUInt("SelectedAxis", (SelectedAxis >= 0 && bIsActiveOperation) ? static_cast<uint32>(SelectedAxis) : 0xffffffffu);
		Material->SetFloat("HoveredAxisOpacity", 0.3f);

		return Cmd;
		};

	RenderBus.AddCommand(ERenderPass::DepthLess, CreateGizmoCmd(false));

	if (!bHolding)
	{
		RenderBus.AddCommand(ERenderPass::DepthLess, CreateGizmoCmd(true));
	}
}

void FRenderCollector::CollectFromActor(AActor* Actor, const FShowFlags& ShowFlags, EViewMode ViewMode, FRenderBus& RenderBus, EWorldType WorldType)
{
	if (!Actor->IsVisible()) return;

	for (UPrimitiveComponent* Primitive : Actor->GetPrimitiveComponents())
	{
		CollectFromComponent(Primitive, ShowFlags, ViewMode, RenderBus, WorldType);
	}
}

bool FRenderCollector::CollectFromSelectedActor(AActor* Actor, const FShowFlags& ShowFlags, EViewMode ViewMode, FRenderBus& RenderBus)
{
	if (!Actor->IsVisible()) return false;

	bool bHasSelectionMask = false;
	std::unordered_set<int32> SeenBVHNodeIndices;

	for (UPrimitiveComponent* primitiveComponent : Actor->GetPrimitiveComponents())
	{
		if (!primitiveComponent->IsVisible()) continue;
		if (primitiveComponent->IsEditorOnly())
		{
			UWorld* World = Actor->GetFocusedWorld();
			if (World && World->GetWorldType() != EWorldType::Editor)
				continue;
		}

		FMeshBuffer* MeshBuffer = nullptr;
		if (primitiveComponent->GetPrimitiveType() == EPrimitiveType::EPT_StaticMesh)
		{
			auto* StaticMeshComp = static_cast<UStaticMeshComponent*>(primitiveComponent);
			MeshBuffer = MeshBufferManager.GetStaticMeshBuffer(StaticMeshComp->GetStaticMesh());
		}
		else
		{
			MeshBuffer = &MeshBufferManager.GetMeshBuffer(primitiveComponent->GetPrimitiveType());
		}

		if (!MeshBuffer)
		{
			continue;
		}

		FRenderCommand BaseCmd{};
		BaseCmd.MeshBuffer = MeshBuffer;
		BaseCmd.PerObjectConstants = FPerObjectConstants(primitiveComponent->GetWorldMatrix());
		BaseCmd.SectionIndexStart = 0;
		BaseCmd.SectionIndexCount = MeshBuffer->GetIndexBuffer().GetIndexCount();

		if (primitiveComponent->GetPrimitiveType() == EPrimitiveType::EPT_Text)
		{
			UTextRenderComponent* TextComp = static_cast<UTextRenderComponent*>(primitiveComponent);
			const FFontResource* Font = TextComp->GetFont();
			if (!Font || !Font->IsLoaded()) continue;
			const FString& Text = TextComp->GetText();
			if (Text.empty()) continue;

			FMatrix WorldMatrix = TextComp->GetTextMatrix();

			FRenderCommand TextCmd = BaseCmd;
			BaseCmd.PerObjectConstants = FPerObjectConstants(WorldMatrix);
			TextCmd.PerObjectConstants = FPerObjectConstants(TextComp->GetWorldMatrix(), TextComp->GetColor());
			TextCmd.Type = ERenderCommandType::Font;
			TextCmd.Constants.Font.Text = &Text;
			TextCmd.Constants.Font.Font = Font;
			TextCmd.Constants.Font.Scale = TextComp->GetFontSize();
			RenderBus.AddCommand(ERenderPass::Font, TextCmd);
		}
		else if (primitiveComponent->GetPrimitiveType() == EPrimitiveType::EPT_SubUV)
		{
			BaseCmd.PerObjectConstants.Model = MakeViewSubUVSelectionMatrix(
				static_cast<USubUVComponent*>(primitiveComponent),
				RenderBus);
		}

		else if(primitiveComponent->GetPrimitiveType() == EPrimitiveType::EPT_Billboard)
		{
			const UBillboardComponent* BComp = static_cast<const UBillboardComponent*>(primitiveComponent);
			BaseCmd.PerObjectConstants.Model = UBillboardComponent::MakeBillboardWorldMatrix(
				BComp->GetWorldLocation(),
				FVector(0.01f, BComp->GetWidth(), BComp->GetHeight()),
				RenderBus.GetCameraForward(),
				RenderBus.GetCameraRight(),
				RenderBus.GetCameraUp());
		}

		if (!primitiveComponent->SupportsOutline()) continue;

		// Selection Mask
		FRenderCommand MaskCmd = BaseCmd;
		if (primitiveComponent->GetPrimitiveType() == EPrimitiveType::EPT_Billboard)
		{
			MaskCmd.Type = ERenderCommandType::BillboardSelectionMask;
			MaskCmd.Constants.Billboard.Texture = static_cast<UBillboardComponent*>(primitiveComponent)->GetTexture();
		}
		else
		{
			MaskCmd.Type = ERenderCommandType::SelectionMask;
		}
		RenderBus.AddCommand(ERenderPass::SelectionMask, MaskCmd);
		bHasSelectionMask = true;

		if (ShowFlags.bBoundingVolume)
		{
			LineBatcher->AddAABB(BuildRenderAABB(primitiveComponent, RenderBus), FColor::White());
		}

		CollectBVHInternalNodeAABBs(primitiveComponent, ShowFlags, RenderBus, SeenBVHNodeIndices);
	}

    // 선택된 Light Components의 Bounding 시각화
    for (UActorComponent* Component : Actor->GetComponents())
    {
        const ULightComponent* LightComponent = Cast<ULightComponent>(Component);
        if (LightComponent == nullptr || !LightComponent->IsVisible())
        {
            continue;
        }

        switch (LightComponent->GetLightType())
        {
        case ELightType::LightType_Directional:
        case ELightType::LightType_AmbientLight:
        {
            break;
        }

        case ELightType::LightType_Point:
        {
            const UPointLightComponent* PointLightComponent = Cast<UPointLightComponent>(LightComponent);
            LineBatcher->AddPointLight(
                PointLightComponent->GetWorldLocation(),
                PointLightComponent->GetAttenuationRadius(),
                PointLightComponent->GetRightVector(),
                PointLightComponent->GetUpVector());
            break;
        }

        case ELightType::LightType_Spot:
        {
            const USpotLightComponent* SpotLightComponent = Cast<USpotLightComponent>(LightComponent);
            LineBatcher->AddSpotLight(
                SpotLightComponent->GetWorldLocation(),
                SpotLightComponent->GetUpVector() * -1.0f,
                SpotLightComponent->GetRightVector() * -1.0f,
                SpotLightComponent->GetAttenuationRadius(),
                SpotLightComponent->GetInnerConeAngle(),
                SpotLightComponent->GetOuterConeAngle());
            break;
        }
        }
    }

	return bHasSelectionMask;
}

void FRenderCollector::CollectFromComponent(UPrimitiveComponent* Primitive, const FShowFlags& ShowFlags, EViewMode ViewMode, FRenderBus& RenderBus, EWorldType WorldType)
{
	if (!Primitive->IsVisible()) return;
	if (Primitive->IsEditorOnly() && WorldType != EWorldType::Editor) return;

	EPrimitiveType PrimType = Primitive->GetPrimitiveType();

	ID3D11ShaderResourceView* DefaultSRV = FResourceManager::Get().GetDefaultWhiteSRV();
	auto ResolveSRV = [&](const FString& Path) -> ID3D11ShaderResourceView*
		{
			UTexture* Texture = FResourceManager::Get().GetTexture(Path);
			return Texture ? Texture->GetSRV() : DefaultSRV;
		};
	static const FMaterial EngineDefaultMaterial{};

	switch (PrimType)
	{
	case EPrimitiveType::EPT_StaticMesh:
	{
		if (!ShowFlags.bPrimitives) return;

		UStaticMeshComponent* StaticMeshComp = static_cast<UStaticMeshComponent*>(Primitive);
		const UStaticMesh* StaticMesh = StaticMeshComp->GetStaticMesh();

		if (!StaticMesh || !StaticMesh->HasValidMeshData()) return;

		// 1. 카메라 정보 및 AABB 가져오기
        FVector CameraPos = RenderBus.GetCameraPosition();
        FMatrix ProjMatrix = RenderBus.GetProj();
        FAABB Bounds = StaticMeshComp->GetWorldAABB();
        const int32 ValidLODCount = StaticMesh->GetValidLODCount();

        // 2. LOD 레벨 계산
		int32 SelectedLOD = 0; // 기본값은 항상 원본(최고 화질)
        if (ShowFlags.bEnableLOD)
        {
            SelectedLOD = SelectLODLevel(CameraPos, Bounds, ProjMatrix, ValidLODCount);
        }

		FMeshBuffer* MeshBuffer = MeshBufferManager.GetStaticMeshBuffer(StaticMesh, SelectedLOD);
        if (!MeshBuffer) return;

        const FStaticMesh* MeshData = StaticMesh->GetMeshData(SelectedLOD);
        const TArray<FStaticMeshSection>& Sections = MeshData->Sections;

		for (int32 SectionIdx = 0; SectionIdx < static_cast<int32>(Sections.size()); ++SectionIdx)
		{
			const FStaticMeshSection& Section = Sections[SectionIdx];
			UMaterialInterface* Material = Cast<UMaterialInterface>(StaticMeshComp->GetMaterial(SectionIdx));
			if (Material == nullptr)
			{
				Material = FResourceManager::Get().GetMaterial("DefaultWhite");
				if (Material == nullptr)
				{
					continue;
				}
			}

			FRenderCommand Cmd = {};
			Cmd.PerObjectConstants = FPerObjectConstants{ Primitive->GetWorldMatrix(), FColor::White().ToVector4() };
			Cmd.Type = ERenderCommandType::StaticMesh;
			Cmd.MeshBuffer = MeshBuffer;

			Cmd.SectionIndexStart = Section.StartIndex;
			Cmd.SectionIndexCount = Section.IndexCount;
			Cmd.Material = Material;

			//if (Material)
			//{
			//	Material->SetVector2("ScrollUV", FVector2(StaticMeshComp->GetScroll().first, StaticMeshComp->GetScroll().second));
			//}

			RenderBus.AddCommand(ERenderPass::Opaque, Cmd);

			if (Material->GetEffectiveLightingModel() == ELightingModel::Toon)
			{
                FRenderCommand OutlineCmd = {};
                OutlineCmd.Type = ERenderCommandType::ToonOutline;
                OutlineCmd.MeshBuffer = MeshBuffer;
                OutlineCmd.PerObjectConstants = FPerObjectConstants{
                    Primitive->GetWorldMatrix()
                };
                OutlineCmd.SectionIndexStart = Section.StartIndex;
                OutlineCmd.SectionIndexCount = Section.IndexCount;
                OutlineCmd.Material = Material;

                RenderBus.AddCommand(ERenderPass::ToonOutline, OutlineCmd);
			}
		}

		break;
	}

	case EPrimitiveType::EPT_Text:
	{
		if (!ShowFlags.bBillboardText) return;

		UTextRenderComponent* TextComp = static_cast<UTextRenderComponent*>(Primitive);
		const FFontResource* Font = TextComp->GetFont();
		if (!Font || !Font->IsLoaded()) return;

		const FString& Text = TextComp->GetText();
		if (Text.empty()) return;

		FRenderCommand Cmd = {};
		Cmd.Type = ERenderCommandType::Font;
		Cmd.PerObjectConstants = FPerObjectConstants{TextComp->GetWorldMatrix(), TextComp->GetColor()};
		Cmd.Constants.Font.Text = &Text;
		Cmd.Constants.Font.Font = Font;
		Cmd.Constants.Font.Scale = TextComp->GetFontSize();
		
		RenderBus.AddCommand(ERenderPass::Font, Cmd);
		break;
	}

	case EPrimitiveType::EPT_SubUV:
	{
		USubUVComponent* SubUVComp = static_cast<USubUVComponent*>(Primitive);
		const FParticleResource* Particle = SubUVComp->GetParticle();
		if (!Particle || !Particle->IsLoaded()) return;

		FRenderCommand Cmd = {};
		Cmd.PerObjectConstants = FPerObjectConstants{
			MakeViewBillboardMatrix(Primitive, RenderBus),
			FColor::White().ToVector4() };
		Cmd.Type = ERenderCommandType::SubUV;
		Cmd.Constants.SubUV.Particle = Particle;
		Cmd.Constants.SubUV.FrameIndex = SubUVComp->GetFrameIndex();
		Cmd.Constants.SubUV.Width = SubUVComp->GetWidth();
		Cmd.Constants.SubUV.Height = SubUVComp->GetHeight();

		RenderBus.AddCommand(ERenderPass::SubUV, Cmd);
		break;
	}

	case EPrimitiveType::EPT_Billboard:
	{
		UBillboardComponent* BillboardComp = static_cast<UBillboardComponent*>(Primitive);
		UTexture* Texture = BillboardComp->GetTexture();

		UMaterial* BillboardMat = FResourceManager::Get().GetMaterial("BillboardMat");
		BillboardMat->DepthStencilType = EDepthStencilType::Default;
		BillboardMat->BlendType        = EBlendType::AlphaBlend;
		BillboardMat->RasterizerType   = ERasterizerType::SolidNoCull;
		BillboardMat->SamplerType      = ESamplerType::EST_Linear;

		const FMatrix BillboardMatrix = UBillboardComponent::MakeBillboardWorldMatrix(
			BillboardComp->GetWorldLocation(),
			FVector(0.01f, BillboardComp->GetWidth(), BillboardComp->GetHeight()),
			RenderBus.GetCameraForward(),
			RenderBus.GetCameraRight(),
			RenderBus.GetCameraUp());

		// LightComponents에 부착된 빌보드의 색상 조정
		FVector4 LightColor = FColor::White().ToVector4();
		if (ULightComponent* LightComponent = Cast<ULightComponent>(BillboardComp->GetParent()))
		{
			LightColor = LightComponent->GetLightColor().ToVector4();
		}

		FRenderCommand Cmd = {};
		Cmd.Type = ERenderCommandType::Billboard;
		Cmd.MeshBuffer = &MeshBufferManager.GetMeshBuffer(EPrimitiveType::EPT_Billboard);
		Cmd.PerObjectConstants = FPerObjectConstants{ BillboardMatrix, LightColor };
		Cmd.Material = BillboardMat;
		Cmd.Constants.Billboard.Texture = Texture;

		RenderBus.AddCommand(ERenderPass::Billboard, Cmd);
		break;
	}

	case EPrimitiveType::EPT_Decal:
	{
		if (!ShowFlags.bDecals) return;

		FScopeCycleCounter RenderDecalScope({});

		UDecalComponent* DecalComp = static_cast<UDecalComponent*>(Primitive);
		UMaterialInterface* Material = Cast<UMaterialInterface>(DecalComp->GetMaterial());

		UWorld* World = DecalComp->GetOwner() ? DecalComp->GetOwner()->GetFocusedWorld() : nullptr;

		FOBB DecalOBB = FOBB::FromAABB(DecalComp->GetWorldAABB(), DecalComp->GetWorldMatrix());

		TArray<UPrimitiveComponent*> VisiblePrimitiveScratch;
		World->GetSpatialIndex().OBBQueryPrimitives(DecalOBB, VisiblePrimitiveScratch, OBBQueryScratch);

		for (UPrimitiveComponent* Prim : VisiblePrimitiveScratch)
		{
			if (Prim->GetPrimitiveType() != EPrimitiveType::EPT_StaticMesh) continue;

			UStaticMeshComponent* StaticMeshComp = static_cast<UStaticMeshComponent*>(Prim);
			const UStaticMesh* StaticMesh = StaticMeshComp->GetStaticMesh();

			if (!StaticMesh || !StaticMesh->HasValidMeshData()) continue;

			// 1. 카메라 정보 및 AABB 가져오기
			FVector CameraPos = RenderBus.GetCameraPosition();
			FMatrix ProjMatrix = RenderBus.GetProj();
			FAABB Bounds = StaticMeshComp->GetWorldAABB();
			const int32 ValidLODCount = StaticMesh->GetValidLODCount();

			int32 SelectedLOD = 0; // 기본값은 항상 원본(최고 화질)
			if (ShowFlags.bEnableLOD)
			{
				SelectedLOD = SelectLODLevel(CameraPos, Bounds, ProjMatrix, ValidLODCount);
			}

			FMeshBuffer* MeshBuffer = MeshBufferManager.GetStaticMeshBuffer(StaticMesh, SelectedLOD);
			if (!MeshBuffer) return;

			const FStaticMesh* MeshData = StaticMesh->GetMeshData(SelectedLOD);
			const TArray<FStaticMeshSection>& Sections = MeshData->Sections;

			for (int32 SectionIdx = 0; SectionIdx < static_cast<int32>(Sections.size()); ++SectionIdx)
			{
				const FStaticMeshSection& Section = Sections[SectionIdx];

				FRenderCommand Cmd = {};
				Cmd.Type = ERenderCommandType::Decal;
				Cmd.PerObjectConstants = FPerObjectConstants{ Prim->GetWorldMatrix(), DecalComp->GetDecalColor().ToVector4() };
				Cmd.MeshBuffer = MeshBuffer;

				Cmd.SectionIndexStart = Section.StartIndex;
				Cmd.SectionIndexCount = Section.IndexCount;

				Cmd.Material = Material;
				Cmd.DecalConstants.InvDecalWorld = DecalComp->GetDecalMatrix().GetInverse();

				RenderBus.AddCommand(ERenderPass::Decal, Cmd);
			}
		}

		if (WorldType == EWorldType::Editor)
		{
			LineBatcher->AddOBB(DecalOBB, FColor::Green());
		}

		LastDecalStats.TotalDecalCount += 1;
		LastDecalStats.CollectTimeMS += static_cast<int32>(RenderDecalScope.Finish());
		break;
	}

    case EPrimitiveType::EPT_FOG:
    {
        if (!ShowFlags.bFog)
            return;
        UHeightFogComponent* HeightFogComp = static_cast<UHeightFogComponent*>(Primitive);

        FRenderCommand Cmd = {};
        Cmd.Type = ERenderCommandType::Primitive;
        Cmd.Constants.Fog.FogDensity = HeightFogComp->GetFogDensity();
        Cmd.Constants.Fog.FogColor = HeightFogComp->GetFogInscatteringColor();
        Cmd.Constants.Fog.HeightFalloff = HeightFogComp->GetHeightFalloff();
        Cmd.Constants.Fog.FogHeight = HeightFogComp->GetFogHeight();
        Cmd.Constants.Fog.FogStartDistance = HeightFogComp->GetFogStartDistance();
        Cmd.Constants.Fog.FogMaxOpacity = HeightFogComp->GetFogMaxOpacity();
        Cmd.Constants.Fog.FogCutoffDistance = HeightFogComp->GetFogCutoffDistance();
        //Cmd.BlendState = EBlendState::AlphaBlend;
        //Cmd.DepthStencilState = EDepthStencilState::Default;

        RenderBus.AddCommand(ERenderPass::Fog, Cmd);
        break;
    }
	case EPrimitiveType::EPT_SKY:
	{
		if (!RenderBus.GetCommands(ERenderPass::Sky).empty())
		{
			return;
		}

		USkyAtmosphereComponent* SkyComponent = static_cast<USkyAtmosphereComponent*>(Primitive);
		SkyComponent->RefreshSkyStateFromWorld();

		FRenderCommand Cmd = {};
		Cmd.Type = ERenderCommandType::Sky;
		SkyComponent->FillSkyConstants(RenderBus, Cmd.Constants.Sky);
		RenderBus.AddCommand(ERenderPass::Sky, Cmd);
		break;
	}
	default:
		if (PrimType == EPrimitiveType::EPT_TransGizmo || PrimType == EPrimitiveType::EPT_RotGizmo || PrimType == EPrimitiveType::EPT_ScaleGizmo)
		{
			return;
		}
		return;
	}
}

void FRenderCollector::CollectBVHInternalNodeAABBs(UPrimitiveComponent* PrimitiveComponent, const FShowFlags& ShowFlags, FRenderBus& RenderBus, std::unordered_set<int32>& SeenNodeIndices)
{
	if (!ShowFlags.bBoundingVolume || !ShowFlags.bBVHBoundingVolume || PrimitiveComponent == nullptr || LineBatcher == nullptr)
	{
		return;
	}

	AActor* Owner = PrimitiveComponent->GetOwner();
	UWorld* World = Owner ? Owner->GetFocusedWorld() : nullptr;
	if (World == nullptr)
	{
		return;
	}

	const FWorldSpatialIndex& SpatialIndex = World->GetSpatialIndex();
	const int32 ObjectIndex = SpatialIndex.FindObjectIndex(PrimitiveComponent);
	if (ObjectIndex == FBVH::INDEX_NONE)
	{
		return;
	}

	const FBVH& BVH = SpatialIndex.GetBVH();
	const TArray<int32>& ObjectToLeafNode = BVH.GetObjectToLeafNode();
	if (ObjectIndex < 0 || ObjectIndex >= static_cast<int32>(ObjectToLeafNode.size()))
	{
		return;
	}

	const int32 LeafNodeIndex = ObjectToLeafNode[ObjectIndex];
	if (LeafNodeIndex == FBVH::INDEX_NONE)
	{
		return;
	}

	const TArray<FBVH::FNode>& Nodes = BVH.GetNodes();
	if (LeafNodeIndex < 0 || LeafNodeIndex >= static_cast<int32>(Nodes.size()))
	{
		return;
	}

	TArray<int32> PathToRoot;
	PathToRoot.reserve(16);

	int32 CurrentNodeIndex = Nodes[LeafNodeIndex].Parent;
	while (CurrentNodeIndex != FBVH::INDEX_NONE)
	{
		if (CurrentNodeIndex < 0 || CurrentNodeIndex >= static_cast<int32>(Nodes.size()))
		{
			break;
		}

		PathToRoot.push_back(CurrentNodeIndex);
		CurrentNodeIndex = Nodes[CurrentNodeIndex].Parent;
	}

	for (int32 PathIndex = 0; PathIndex < static_cast<int32>(PathToRoot.size()); ++PathIndex)
	{
		const int32 NodeIndex = PathToRoot[PathIndex];
		if (!SeenNodeIndices.insert(NodeIndex).second)
		{
			continue;
		}

		const FBVH::FNode& Node = Nodes[NodeIndex];
		if (Node.IsLeaf())
		{
			continue;
		}

		const FColor Color = MakeBVHInternalNodeColor(PathIndex, static_cast<int32>(PathToRoot.size()));
		LineBatcher->AddAABB(Node.Bounds, Color);
	}
}

