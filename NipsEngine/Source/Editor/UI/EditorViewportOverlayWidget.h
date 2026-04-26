#pragma once

#include "Editor/UI/EditorWidget.h"

struct ImVec2;
struct FEditorViewportState;

class FEditorViewportOverlayWidget : public FEditorWidget
{
private:
	bool bExpanded = false;
	bool bShowViewportSettings = true;
	bool bShowShortcutsWindow = false;

private:
	void RenderViewportSettings(float DeltaTime);
	void RenderDebugStats(float DeltaTime);
	void RenderSplitterBar();
	void RenderBoxSelectionOverlay();
	void RenderShortcutsWindow();

public:
	bool IsViewportSettingsVisible() const { return bShowViewportSettings; }
	void SetViewportSettingsVisible(bool bVisible) { bShowViewportSettings = bVisible; }
	bool IsShortcutsWindowVisible() const { return bShowShortcutsWindow; }
	void SetShortcutsWindowVisible(bool bVisible) { bShowShortcutsWindow = bVisible; }
	void Render(float DeltaTime) override;

	// ──────────── Debug Stat을 호출하는 단순 헬퍼 함수 ────────────
private:
	float RenderGeneralStatsWindow(int32 ViewportIndex, const FEditorViewportState& VS, const ImVec2& Pos, float DeltaTime);
	float RenderNameTableWindow(int32 ViewportIndex, const FEditorViewportState& VS, const ImVec2& Pos);
	float RenderLightCullWindow(int32 ViewportIndex, const FEditorViewportState& VS, const ImVec2& Pos);
};
