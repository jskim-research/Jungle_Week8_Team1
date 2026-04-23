#include "Editor/UI/EditorPropertyWidget.h"

#include "Editor/EditorEngine.h"
#include "ImGui/imgui.h"
#include "GameFramework/PrimitiveActors.h"
#include "Core/PropertyTypes.h"
#include "Math/Color.h"
#include "Core/ResourceManager.h"
#include "Object/FName.h"
#include <functional>

#include "Editor/Utility/EditorComponentFactory.h"

#include "Component/StaticMeshComponent.h"
#include "Component/GizmoComponent.h"
#include "Component/Movement/InterpToMovementComponent.h"
#include "Editor/Utility/EditorUtils.h"

#define SEPARATOR(); ImGui::Spacing(); ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing(); ImGui::Spacing();

void FEditorPropertyWidget::Initialize(UEditorEngine* InEditorEngine)
{
	FEditorWidget::Initialize(InEditorEngine);
	SelectionManager = &EditorEngine->GetSelectionManager();
}

void FEditorPropertyWidget::ResetSelection()
{
	SelectedComponent = nullptr;
	LastSelectedActor = nullptr;
	bActorSelected = true;
}

void FEditorPropertyWidget::Render(float DeltaTime)
{
	(void)DeltaTime;

	ImGui::SetNextWindowSize(ImVec2(350.0f, 500.0f), ImGuiCond_Once);
	ImGui::Begin("Jungle Property Window");

	AActor* PrimaryActor = SelectionManager->GetPrimarySelection();
	if (!PrimaryActor)
	{
		SelectedComponent = nullptr;
		LastSelectedActor = nullptr;
		bActorSelected = true;
		ImGui::Text("No object selected.");
		ImGui::End();
		return;
	}

	UpdateSelectionState(PrimaryActor);

	const TArray<AActor*>& SelectedActors = SelectionManager->GetSelectedActors();

	// 상단 액터 정보 및 컨트롤 영역
	RenderActorHeaderRegion(PrimaryActor, SelectedActors);

	if (SelectionManager->GetPrimarySelection() == nullptr)
	{
		ImGui::End();
		return;
	}

	// 컴포넌트 트리 영역
	SEPARATOR();
	RenderComponentTree(PrimaryActor);

	// 디테일 프로퍼티 영역
	SEPARATOR();
	ImGui::Text("Details");
	ImGui::Separator();

	float ScrollHeight = std::max(UIConstants::MinScrollHeight, ImGui::GetContentRegionAvail().y);
	ImGui::BeginChild("##Details", ImVec2(0, ScrollHeight), true, ImGuiWindowFlags_AlwaysVerticalScrollbar);
	{
		RenderDetails(PrimaryActor, SelectedActors);
	}
	ImGui::EndChild();

	ImGui::End();
}

void FEditorPropertyWidget::UpdateSelectionState(AActor* PrimaryActor)
{
	if (PrimaryActor != LastSelectedActor)
	{
		SelectedComponent = nullptr;
		LastSelectedActor = PrimaryActor;

		USceneComponent* RootComp = PrimaryActor->GetRootComponent();
		if (RootComp && RootComp->IsA<UStaticMeshComponent>())
		{
			SelectedComponent = RootComp;
			bActorSelected = false;
		}
		else
		{
			bActorSelected = true;
		}
	}
}

void FEditorPropertyWidget::RenderActorHeaderRegion(AActor* PrimaryActor, const TArray<AActor*>& SelectedActors)
{
	const int32 SelectionCount = static_cast<int32>(SelectedActors.size());

	if (SelectionCount > 1)
	{
		RenderMultiSelectionHeader(PrimaryActor, SelectedActors, SelectionCount);
	}
	else
	{
		RenderSingleSelectionHeader(PrimaryActor);
	}
}

void FEditorPropertyWidget::RenderMultiSelectionHeader(AActor* PrimaryActor, const TArray<AActor*>& SelectedActors, int32 SelectionCount)
{
	ImGui::Text("Class: %s", PrimaryActor->GetTypeInfo()->name);

	FString PrimaryName = PrimaryActor->GetFName().ToString();
	if (PrimaryName.empty()) PrimaryName = PrimaryActor->GetTypeInfo()->name;

	if (bActorSelected) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.8f, 0.2f, 1.0f));
	ImGui::Text("Name: %s (+%d)", PrimaryName.c_str(), SelectionCount - 1);
	if (bActorSelected) ImGui::PopStyleColor();

	if (ImGui::IsItemClicked())
	{
		bActorSelected = true;
		SelectedComponent = nullptr;
	}

	ImGui::SameLine();
	char RemoveLabel[64];
	snprintf(RemoveLabel, sizeof(RemoveLabel), "Remove %d Objects", SelectionCount);
	if (ImGui::SmallButton(RemoveLabel))
	{
		for (AActor* Actor : SelectedActors)
		{
			if (Actor && Actor->GetFocusedWorld()) Actor->GetFocusedWorld()->DestroyActor(Actor);
		}
		SelectionManager->ClearSelection();
		SelectedComponent = nullptr;
		LastSelectedActor = nullptr;
	}
}

void FEditorPropertyWidget::RenderSingleSelectionHeader(AActor* PrimaryActor)
{
	// SelectedComponent가 아직 설정되지 않은 경우 루트로 초기화
	if (SelectedComponent == nullptr)
		SelectedComponent = PrimaryActor->GetRootComponent();

	ImGui::Text("Actor: %s", PrimaryActor->GetFName().ToString().c_str());
	ImGui::Text("Component: %s", SelectedComponent ? SelectedComponent->GetTypeInfo()->name : "None");

	if (bActorSelected) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.8f, 0.2f, 1.0f));
	if (ImGui::IsItemClicked())
	{
		bActorSelected = true;
		SelectedComponent = nullptr;
	}
	if (bActorSelected) ImGui::PopStyleColor();

	ImGui::SameLine();
	if (ImGui::SmallButton("Remove"))
	{
		if (PrimaryActor->GetFocusedWorld()) PrimaryActor->GetFocusedWorld()->DestroyActor(PrimaryActor);
		SelectionManager->ClearSelection();
		SelectedComponent = nullptr;
		LastSelectedActor = nullptr;
	}

	ImGui::Spacing();
	RenderAddComponentPopup(PrimaryActor);
}

void FEditorPropertyWidget::RenderAddComponentPopup(AActor* PrimaryActor)
{
	if (ImGui::Button("Add Component", ImVec2(-1, 0)))
	{
		ImGui::OpenPopup("AddComponentPopup");
	}

	if (ImGui::BeginPopup("AddComponentPopup"))
	{
		const char* CurrentCategory = nullptr;
		for (const FComponentMenuEntry& Entry : FComponentFactory::GetMenuRegistry())
		{
			if (CurrentCategory == nullptr || strcmp(CurrentCategory, Entry.Category) != 0)
			{
				CurrentCategory = Entry.Category;
				ImGui::SeparatorText(CurrentCategory);
			}

			if (ImGui::Selectable(Entry.DisplayName))
			{
				if (UActorComponent* NewComp = Entry.Register(PrimaryActor))
				{
					AttachAndSelectNewComponent(PrimaryActor, NewComp);
				}
			}
		}
		ImGui::EndPopup();
	}
}

void FEditorPropertyWidget::RenderComponentTree(AActor* Actor)
{
    ImGui::Text("Components");
    ImGui::Separator();

    float TreeHeight = std::max(100.0f, ImGui::GetContentRegionAvail().y * 0.4f);
    
    // BeginChild를 호출하여 내부 스크롤이 가능한 Child Window를 생성합니다.
    ImGui::BeginChild("##ComponentTreeChild", ImVec2(0, TreeHeight), true, ImGuiWindowFlags_AlwaysVerticalScrollbar);

    USceneComponent* Root = Actor->GetRootComponent();
    UActorComponent* ComponentToDelete = nullptr;

    if (Root)
    {
        RenderSceneComponentNode(Actor, Root, ComponentToDelete);
    }

    // Non-scene ActorComponents 및 MovementComponent들 하단 출력
    for (UActorComponent* Comp : Actor->GetComponents())
    {
        // SceneComponent는 위의 트리 렌더링에서 처리되었으므로 패스
        if (!Comp || Comp->IsA<USceneComponent>()) { continue; }
        if (Comp->IsVisualizationComponent()) { continue; }

        ImGuiTreeNodeFlags Flags = ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
        if (!bActorSelected && SelectedComponent == Comp)
            Flags |= ImGuiTreeNodeFlags_Selected;

        // MovementComponent 일 때와 일반 컴포넌트 일 때의 출력 형식 분리
        if (UMovementComponent* MoveComp = Cast<UMovementComponent>(Comp))
        {
            FString MoveName = EditorUtils::GetMovementComponentDisplayName(MoveComp);
            ImGui::TreeNodeEx(Comp, Flags, "%s", MoveName.c_str());

            // --- DRAG SOURCE (MovementComponent) ---
            if (ImGui::BeginDragDropSource())
            {
                ImGui::SetDragDropPayload("DND_MOVE_COMP", &Comp, sizeof(UActorComponent*));
                ImGui::Text("Moving %s", MoveName.c_str());
                ImGui::EndDragDropSource();
            }
        }
        else
        {
            FString Name = Comp->GetFName().ToString();
            ImGui::TreeNodeEx(Comp, Flags, "%s", Name.c_str());
        }

        if (ImGui::IsItemClicked())
        {
            SelectedComponent = Comp;
            bActorSelected = false;
        }

        if (Comp != Actor->GetRootComponent())
        {
            ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - UIConstants::TreeRightMargin);
            char XId[64];
            EditorUtils::MakeXButtonId(XId, sizeof(XId), Comp);
            if (EditorUtils::DrawXButton(XId)) ComponentToDelete = Comp;
        }
    }

    ImGui::EndChild();

    // 삭제 처리는 렌더링 루프 바깥(Child Window 종료 후)에서 안전하게 수행
    if (ComponentToDelete)
    {
        if (SelectedComponent == ComponentToDelete)
        {
            SelectedComponent = nullptr;
            bActorSelected = true;
        }
        Actor->RemoveComponent(ComponentToDelete);
    }
}

// 계층 구조를 가진 씬 컴포넌트 노드를 재귀적으로 렌더링합니다. Drag & Drop, 삭제 버튼 등도 여기서 처리됩니다.
void FEditorPropertyWidget::RenderSceneComponentNode(AActor* Actor, USceneComponent* Comp, UActorComponent*& OutCompToDelete)
{
    if (!Comp || Comp->IsVisualizationComponent()) return;

    // 노드 이름 설정
    FString Name = Comp->GetFName().ToString();
    if (Name.empty()) Name = Comp->GetTypeInfo()->name;

    auto HasValidChildren = [&]() {
        for (USceneComponent* Child : Comp->GetChildren())
            if (!Child->IsVisualizationComponent()) return true;
        return false;
    };

    // 노드 이름, 자식 존재 여부에 따라 Tree Flag 설정
    ImGuiTreeNodeFlags Flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_DefaultOpen;
    if (!HasValidChildren()) Flags |= ImGuiTreeNodeFlags_Leaf;
    if (!bActorSelected && SelectedComponent == Comp) Flags |= ImGuiTreeNodeFlags_Selected;

    // 트리 노드 출력
    bool bIsRoot = (Comp->GetParent() == nullptr);
    if (!bIsRoot)
    {
        float ClipMaxX = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x - UIConstants::ClipMargin;
        ImGui::PushClipRect(ImGui::GetWindowPos(), ImVec2(ClipMaxX, ImGui::GetWindowPos().y + 99999.f), true);
    }

    bool bOpen = ImGui::TreeNodeEx(Comp, Flags, "%s%s", bIsRoot ? "[Root] " : "", Name.c_str());

    if (!bIsRoot) ImGui::PopClipRect();

    // 드래그 앤 드롭 (계층 구조 변경 및 MovementComponent가 이동시킬 대상 설정)
    if (ImGui::BeginDragDropSource())
    {
        ImGui::SetDragDropPayload("DND_SCENE_COMP", &Comp, sizeof(USceneComponent*));
        ImGui::Text("Dragging %s", Name.c_str());
        ImGui::EndDragDropSource();
    }

    if (ImGui::BeginDragDropTarget())
    {
        if (const ImGuiPayload* Payload = ImGui::AcceptDragDropPayload("DND_SCENE_COMP"))
        {
            USceneComponent* DraggedComp = *(USceneComponent**)Payload->Data;
            
            // 조상 여부 체크 (순환 참조 방지)
            bool bIsAncestor = false;
            for (USceneComponent* P = Comp; P; P = P->GetParent()) 
                if (P == DraggedComp) { bIsAncestor = true; break; }

            if (DraggedComp && DraggedComp != Comp && !bIsAncestor)
                DraggedComp->AttachToComponent(Comp);
        }
        if (const ImGuiPayload* Payload = ImGui::AcceptDragDropPayload("DND_MOVE_COMP"))
        {
            if (auto* DraggedMoveComp = *(UMovementComponent**)Payload->Data)
                DraggedMoveComp->SetUpdatedComponent(Comp);
        }
        ImGui::EndDragDropTarget();
    }

    // 컴포넌트가 선택될 경우 자식 노드 재귀 호출
    if (ImGui::IsItemClicked())
    {
        SelectedComponent = Comp;
        bActorSelected = false;
    }

    if (bOpen && Comp)
    {
        for (USceneComponent* Child : Comp->GetChildren())
            RenderSceneComponentNode(Actor, Child, OutCompToDelete);
        ImGui::TreePop();
    }

    // 삭제 버튼 렌더링 (루트 노드나 MovementComponent가 참조 중인 경우엔 skip)
    if (!bIsRoot)
    {
        auto IsReferenced = [&]() {
            for (UActorComponent* ActorComp : Actor->GetComponents())
                if (auto* MoveComp = Cast<UMovementComponent>(ActorComp))
                    if (MoveComp->GetUpdatedComponent() == Comp) return true;
            return false;
        };

        if (!IsReferenced())
        {
            ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - UIConstants::TreeRightMargin);
            char XId[64];
            EditorUtils::MakeXButtonId(XId, sizeof(XId), Comp);
            if (EditorUtils::DrawXButton(XId)) OutCompToDelete = Comp;
        }
    }
}

void FEditorPropertyWidget::RenderDetails(AActor* PrimaryActor, const TArray<AActor*>& SelectedActors)
{
	if (bActorSelected)
	{
		RenderActorProperties(PrimaryActor, SelectedActors);
	}
	else if (SelectedComponent)
	{
		RenderComponentProperties();
	}
	else
	{
		ImGui::TextDisabled("Select an actor or component to view details.");
	}
}

void FEditorPropertyWidget::RenderActorProperties(AActor* PrimaryActor, const TArray<AActor*>& SelectedActors)
{
	ImGui::Text("Actor: %s", PrimaryActor->GetTypeInfo()->name);
	RenderEditableName("Name##Actor", PrimaryActor); // 편집 가능한 UI

	if (PrimaryActor->GetRootComponent())
	{
		ImGui::Separator();
		ImGui::Text("Transform");
		ImGui::Spacing();

		// FVector(위치, 회전, 크기)를 읽어서 Properties를 그려 주는 단순한 친구입니다.
		auto DrawTransformField = [&](const char* Label, FVector CurrentValue, auto ApplyFunc)
		{
			float Arr[3] = { CurrentValue.X, CurrentValue.Y, CurrentValue.Z };
			if (ImGui::DragFloat3(Label, Arr, 0.1f))
			{
				FVector Delta = FVector(Arr[0], Arr[1], Arr[2]) - CurrentValue;
				for (AActor* Actor : SelectedActors)
				{
					if (Actor) ApplyFunc(Actor, Delta);
				}
				EditorEngine->GetGizmo()->UpdateGizmoTransform();
			}
		};

		// Location, Rotation, Scale을 한 번에 그려줍니다.
		DrawTransformField("Location", PrimaryActor->GetActorLocation(), [](AActor* A, FVector D) { A->AddActorWorldOffset(D); });
		DrawTransformField("Rotation", PrimaryActor->GetActorRotation(), [](AActor* A, FVector D) { A->SetActorRotation(A->GetActorRotation() + D); });
		DrawTransformField("Scale",    PrimaryActor->GetActorScale(),    [](AActor* A, FVector D) { A->SetActorScale(A->GetActorScale() + D); });
	}

	ImGui::Separator();
	bool bVisible = PrimaryActor->IsVisible();
	if (ImGui::Checkbox("Visible", &bVisible))
	{
		PrimaryActor->SetVisible(bVisible);
	}
}

void FEditorPropertyWidget::RenderComponentProperties()
{
	ImGui::Text("Component: %s", SelectedComponent->GetTypeInfo()->name);
	RenderEditableName("Name##Component", SelectedComponent); // 편집 가능한 UI

	ImGui::Separator();

	// PropertyDescriptor 기반 자동 위젯 렌더링
	TArray<FPropertyDescriptor> Props;
	SelectedComponent->GetEditableProperties(Props);

	AActor* Owner = SelectedComponent->GetOwner();
	
	bool bAnyChanged = false;
	for (auto& Prop : Props)
	{
		if (Prop.Type == EPropertyType::SceneComponentRef)
		{
			RenderSceneComponentRefWidget(Prop, Owner);
		}
		else
		{
			bAnyChanged |= RenderPropertyWidget(Prop);
		}
	}
	// Special: InterpToMovementComponent control points + behaviour + actions
	if (UInterpToMovementComponent* InterpComp = Cast<UInterpToMovementComponent>(SelectedComponent))
	{
		RenderInterpControlPoints(InterpComp);
	}

	ImGui::Separator();

	// 변경이 있을 경우에만 월드 행렬 갱신
	if (bAnyChanged && SelectedComponent->IsA<USceneComponent>())
	{
		static_cast<USceneComponent*>(SelectedComponent)->MarkTransformDirty();
		SelectionManager->GetGizmo()->UpdateGizmoTransform();
	}
}

void FEditorPropertyWidget::RenderSceneComponentRefWidget(FPropertyDescriptor& Prop, AActor* Owner)
{
	// ValuePtr은 USceneComponent* 변수의 주소 (USceneComponent**)
	USceneComponent** ValuePtr = reinterpret_cast<USceneComponent**>(Prop.ValuePtr);
	USceneComponent* CurrentComp = *ValuePtr;

	// 액터 소유 SceneComponent 목록 수집
	TArray<USceneComponent*> SceneComps;
	SceneComps.push_back(nullptr); // "None" 선택지
	if (Owner)
	{
		for (UActorComponent* Comp : Owner->GetComponents())
		{
			if (USceneComponent* SceneComp = Cast<USceneComponent>(Comp))
				SceneComps.push_back(SceneComp);
		}
	}

	// 드롭다운 레이블 생성: "[Root] ClassName" 또는 "ClassName [FName]"
	auto GetLabel = [&](USceneComponent* Comp) -> FString {
		if (!Comp) return "None";
		FString Name = Comp->GetFName().ToString();
		if (Name.empty()) Name = Comp->GetTypeInfo()->name;
		bool bIsRoot = Owner && (Comp == Owner->GetRootComponent());
		return bIsRoot ? ("[Root] " + Name) : Name;
	};

	FString CurrentLabel = GetLabel(CurrentComp);
	if (ImGui::BeginCombo(Prop.Name, CurrentLabel.c_str()))
	{
		for (USceneComponent* SceneComp : SceneComps)
		{
			bool bSelected = (SceneComp == CurrentComp);
			// ##ptr 으로 포인터를 ID로 사용하여 동일 이름 컴포넌트를 구별
			char SelectableId[128];
			snprintf(SelectableId, sizeof(SelectableId), "%s##%p",
				GetLabel(SceneComp).c_str(), static_cast<void*>(SceneComp));
			if (ImGui::Selectable(SelectableId, bSelected))
			{
				*ValuePtr = SceneComp;
				SelectedComponent->PostEditProperty(Prop.Name);
			}
			if (bSelected)
				ImGui::SetItemDefaultFocus();
		}
		ImGui::EndCombo();
	}
}

bool FEditorPropertyWidget::RenderPropertyWidget(FPropertyDescriptor& Prop)
{
	bool bChanged = false;

	switch (Prop.Type)
	{
	case EPropertyType::Bool:
	{
		bool* Val = static_cast<bool*>(Prop.ValuePtr);
		bChanged = ImGui::Checkbox(Prop.Name, Val);
		break;
	}
	case EPropertyType::Int:
	{
		int32* Val = static_cast<int32*>(Prop.ValuePtr);
		bChanged = ImGui::DragInt(Prop.Name, Val);
		break;
	}
	case EPropertyType::Float:
	{
		float* Val = static_cast<float*>(Prop.ValuePtr);
		if (Prop.Min != 0.0f || Prop.Max != 0.0f)
			bChanged = ImGui::DragFloat(Prop.Name, Val, Prop.Speed, Prop.Min, Prop.Max);
		else
			bChanged = ImGui::DragFloat(Prop.Name, Val, Prop.Speed);
		break;
	}
	case EPropertyType::Vec3:
	{
		float* Val = static_cast<float*>(Prop.ValuePtr);
		bChanged = ImGui::DragFloat3(Prop.Name, Val, Prop.Speed);
		break;
	}
	case EPropertyType::Vec4:
	{
		float* Val = static_cast<float*>(Prop.ValuePtr);
		bChanged = ImGui::ColorEdit4(Prop.Name, Val);
		break;
	}
	case EPropertyType::Color:
	{
		FColor* Val = static_cast<FColor*>(Prop.ValuePtr);
		bChanged = ImGui::ColorEdit4(Prop.Name, &Val->R);
		break;
	}
	case EPropertyType::String:
	{
		FString* Val = static_cast<FString*>(Prop.ValuePtr);
		TArray<FString> Options;
		if      (strcmp(Prop.Name, "Texture Path") == 0) Options = FResourceManager::Get().GetTextureFilePath();
		else if (strcmp(Prop.Name, "StaticMesh")   == 0) Options = FResourceManager::Get().GetStaticMeshPaths();
		bChanged = EditorUtils::RenderStringComboOrInput(Prop.Name, *Val, Options);
		break;
	}
	case EPropertyType::Name:
	{
		FName* Val = static_cast<FName*>(Prop.ValuePtr);
		FString Current = Val->ToString();
		TArray<FString> Options;
		if      (strcmp(Prop.Name, "Font")     == 0) Options = FResourceManager::Get().GetFontNames();
		else if (strcmp(Prop.Name, "Particle") == 0) Options = FResourceManager::Get().GetParticleNames();
		if (EditorUtils::RenderStringComboOrInput(Prop.Name, Current, Options))
		{
			*Val = FName(Current);
			bChanged = true;
		}
		break;
	}
    case EPropertyType::Enum:
	{
		int* Val = static_cast<int*>(Prop.ValuePtr);
		if (Prop.EnumNames && Prop.EnumCount)
			bChanged = ImGui::Combo(Prop.Name, Val, Prop.EnumNames, Prop.EnumCount);
		break;
	}
	case EPropertyType::Vec3Array:
	{
		TArray<FVector>* Arr = static_cast<TArray<FVector>*>(Prop.ValuePtr);
		int32 ToRemove = -1;

		ImGui::Text("%s", Prop.Name);
		ImGui::Spacing();

		for (int32 i = 0; i < static_cast<int32>(Arr->size()); i++)
		{
			ImGui::PushID(i);

			float Val[3] = { (*Arr)[i].X, (*Arr)[i].Y, (*Arr)[i].Z };
			char Label[32];
			snprintf(Label, sizeof(Label), "[%d]", i);

			ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - UIConstants::XButtonSize - 8.0f);
			if (ImGui::DragFloat3(Label, Val, 1.0f))
			{
				(*Arr)[i] = FVector(Val[0], Val[1], Val[2]);
				bChanged = true;
			}

			ImGui::SameLine();
			char XId[32];
			snprintf(XId, sizeof(XId), "##rm_%d", i);
			if (EditorUtils::DrawXButton(XId)) ToRemove = i;

			ImGui::PopID();
		}

		if (ToRemove >= 0)
		{
			Arr->erase(Arr->begin() + ToRemove);
			bChanged = true;
		}

		char AddLabel[64];
		snprintf(AddLabel, sizeof(AddLabel), "+ Add##%s", Prop.Name);
		if (ImGui::Button(AddLabel, ImVec2(-1, 0)))
		{
			Arr->push_back(Arr->empty() ? FVector(0.f, 0.f, 0.f) : Arr->back());
			bChanged = true;
		}
		break;
	}
	}

	if (bChanged && SelectedComponent)
	{
		SelectedComponent->PostEditProperty(Prop.Name);
	}

	return bChanged;
}

void FEditorPropertyWidget::RenderInterpControlPoints(UInterpToMovementComponent* Comp)
{
	// --- Playback actions -----------------------------------------------
	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Text("Playback");
	ImGui::Spacing();

	float HalfWidth = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
	if (ImGui::Button("Initiate", ImVec2(HalfWidth, 0))) Comp->Initiate();
	ImGui::SameLine();
	if (ImGui::Button("Stop",     ImVec2(HalfWidth, 0))) Comp->ResetAndHalt();
	if (ImGui::Button("Reset",    ImVec2(-1,        0))) Comp->Reset();
}

void FEditorPropertyWidget::AttachAndSelectNewComponent(AActor* PrimaryActor, UActorComponent* NewComp)
{
	if (!PrimaryActor || !NewComp) return;

	USceneComponent* AttachTarget = nullptr;
	if (SelectedComponent && SelectedComponent->IsA<USceneComponent>())
	{
		AttachTarget = static_cast<USceneComponent*>(SelectedComponent);
	}
	else
	{
		AttachTarget = PrimaryActor->GetRootComponent();
	}

	if (USceneComponent* SceneComp = Cast<USceneComponent>(NewComp))
	{
		if (AttachTarget) SceneComp->AttachToComponent(AttachTarget);
		else PrimaryActor->SetRootComponent(SceneComp);
	}
	else if (UMovementComponent* MoveComp = Cast<UMovementComponent>(NewComp))
	{
		if (AttachTarget) MoveComp->SetUpdatedComponent(AttachTarget);
	}

	SelectedComponent = NewComp;
	bActorSelected = false;
}

template<typename T>
void FEditorPropertyWidget::RenderEditableName(const char* Label, T* TargetObject)
{
	if (!TargetObject) return;

	char NameBuf[256];
	strncpy_s(NameBuf, sizeof(NameBuf), TargetObject->GetFName().ToString().c_str(), _TRUNCATE);

	// Enter 키를 누르거나 포커스를 잃었을 경우에 이름이 변경되도록 설정
	if (ImGui::InputText(Label, NameBuf, sizeof(NameBuf), ImGuiInputTextFlags_EnterReturnsTrue))
	{
		TargetObject->SetFName(FName(NameBuf));
	}
}
