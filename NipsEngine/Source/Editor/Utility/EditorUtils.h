#pragma once
#include "Core/CoreMinimal.h"
#include "Runtime/ViewportRect.h"
#include "Render/Common/ViewTypes.h"

class UMovementComponent;

// Editor Widget에서 공통적으로 사용될 수 있는 잡다한 상수 및 함수들을 정의합니다.
namespace UIConstants
{
	constexpr float XButtonSize     = 20.0f;
	constexpr float TreeRightMargin = 24.0f; // X버튼이 위치할 우측 여백
	constexpr float ClipMargin      = 28.0f; // 버튼(20) + 여백(8)
	constexpr float MinScrollHeight = 50.0f;
}

namespace EditorUtils
{
    bool DrawXButton(const char* id, float size = UIConstants::XButtonSize);
    void MakeXButtonId(char* OutBuf, size_t BufSize, const void* Ptr);
    bool RenderStringComboOrInput(const char* Label, FString& Value, const TArray<FString>& Options);
    FString GetMovementComponentDisplayName(UMovementComponent* MoveComp);
}

// 뷰포트별 PIE 재생 상태를 나타냅니다.
// UEditorEngine::GetEditorState() / SetEditorState() 는 포커스된 뷰포트의 이 값을 읽고 씁니다.
enum class EViewportPlayState : uint8
{
    Editing,  // 편집 모드
    Playing,  // PIE 실행 중
    Paused,   // PIE 일시정지
};

struct FEditorViewportState
{
	EViewMode ViewMode = EViewMode::Lit;
	bool bHovered = false;

	// Stat Overlay (뷰포트별 독립 제어)
	bool bShowStatFPS        = false;
	bool bShowStatMemory     = false;
	bool bShowStatNameTable  = false;
	bool bShowStatLightCull  = false;

	// NameTable 오버레이 스크롤 오프셋 (휠로 조작)
	int32 NameTableScrollLine = 0;
};
