
#pragma once
#include "Editor/UI/EditorWidget.h"
#include "Object/Object.h"
#include "Render/Common/ComPtr.h"

class FSelectionManager;
class UActorComponent;
class AActor;
struct ID3D11Device;
struct ID3D11ShaderResourceView;
struct ID3D11Texture2D;

class FEditorPropertyWidget : public FEditorWidget
{
public:
	virtual void Render(float DeltaTime) override;
	void Initialize(UEditorEngine* InEditorEngine) override;

	UActorComponent* GetSelectedComponent() const { return SelectedComponent; }
	bool IsActorSelected() const { return bActorSelected; }

	void ResetSelection();

private:
	bool EnsureLightPreviewTexture(ID3D11Device* Device, uint32 Resolution);
	void ResetLightPreviewState();

	// 선택 상태 관리
	void UpdateSelectionState(AActor* PrimaryActor);

	// 헤더 영역
	void RenderActorHeaderRegion(AActor* PrimaryActor, const TArray<AActor*>& SelectedActors);
	void RenderMultiSelectionHeader(AActor* PrimaryActor, const TArray<AActor*>& SelectedActors, int32 SelectionCount);
	void RenderSingleSelectionHeader(AActor* PrimaryActor);
	void RenderAddComponentPopup(AActor* PrimaryActor);

	// 컴포넌트 트리
	void RenderComponentTree(AActor* Actor);
	void RenderSceneComponentNode(AActor* Actor, class USceneComponent* Comp, UActorComponent*& OutCompToDelete);

	// 디테일 패널
	void RenderDetails(AActor* PrimaryActor, const TArray<AActor*>& SelectedActors);
	void RenderActorProperties(AActor* PrimaryActor, const TArray<AActor*>& SelectedActors);
	void RenderComponentProperties();
	bool RenderPropertyWidget(struct FPropertyDescriptor& Prop);
	void RenderSceneComponentRefWidget(struct FPropertyDescriptor& Prop, AActor* Owner);
	void RenderInterpControlPoints(class UInterpToMovementComponent* Comp);
	void RenderLightPreview();

	// 유틸리티
	void AttachAndSelectNewComponent(AActor* PrimaryActor, UActorComponent* NewComp);

	// 이름 변경 및 UI 렌더링
	template<typename T>
	void RenderEditableName(const char* Label, T* TargetObject);

	// 멤버 변수
	FSelectionManager* SelectionManager  = nullptr;
	UActorComponent* SelectedComponent = nullptr;
	UActorComponent* LightPreviewOwnerComponent = nullptr;
	AActor* LastSelectedActor = nullptr;
	bool bActorSelected   = true; // true: Actor details, false: Component details
	uint32 LightPreviewSliceIndex = 0;
	uint32 LightPreviewResolution = 0;
	TComPtr<ID3D11Texture2D> LightPreviewTexture;
	TComPtr<ID3D11ShaderResourceView> LightPreviewSRV;
};
