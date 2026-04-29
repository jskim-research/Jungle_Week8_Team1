#include "Editor/UI/EditorViewportOverlayWidget.h"

#include "Core/ResourceManager.h"

#include "Editor/EditorEngine.h"
#include "Editor/EditorRenderPipeline.h"
#include "Editor/Settings/EditorSettings.h"

#include "Engine/Slate/SlateApplication.h"
#include "Engine/Object/ObjectIterator.h"
#include "Engine/Object/EngineStatics.h"
#include "Engine/Asset/StaticMesh.h"
#include "Engine/Asset/StaticMeshTypes.h"
#include "Engine/Component/GizmoComponent.h"
#include "Engine/Object/FName.h"
#include "Engine/Render/Renderer/RenderFlow/LightCullingPass.h"
#include "Engine/Render/Renderer/RenderFlow/ShadowPass.h"

#include "Slate/SSplitterV.h"
#include "Slate/SSplitterH.h"
#include "Slate/SSplitterCross.h"

#include "Viewport/ViewportLayout.h"

#include "Input/InputSystem.h"

#include "ImGui/imgui.h"

// ──────────── 공통 UI 상수 ────────────
namespace
{
	constexpr ImGuiWindowFlags kStatFlags =
		ImGuiWindowFlags_NoDecoration       |
		ImGuiWindowFlags_AlwaysAutoResize   |
		ImGuiWindowFlags_NoSavedSettings    |
		ImGuiWindowFlags_NoFocusOnAppearing |
		ImGuiWindowFlags_NoNav              |
		ImGuiWindowFlags_NoMove             |
		ImGuiWindowFlags_NoInputs;

	constexpr ImGuiWindowFlags kNameTableFlags =
		ImGuiWindowFlags_NoTitleBar         |
		ImGuiWindowFlags_NoResize           |
		ImGuiWindowFlags_NoScrollbar        |
		ImGuiWindowFlags_NoSavedSettings    |
		ImGuiWindowFlags_NoFocusOnAppearing |
		ImGuiWindowFlags_NoNav              |
		ImGuiWindowFlags_NoMove;

	const ImVec4 ColorWhite  = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
	const ImVec4 ColorPaleBlue = ImVec4(0.8f, 0.8f, 1.0f, 1.0f);
	const ImVec4 ColorGreen  = ImVec4(0.0f, 1.0f, 0.0f, 1.0f);
	const ImVec4 ColorCyan   = ImVec4(0.25f, 0.9f, 1.0f, 1.0f);
	const ImVec4 ColorOrange = ImVec4(1.0f, 0.5f, 0.0f, 1.0f);
	const ImVec4 ColorYellow = ImVec4(1.0f, 1.0f, 0.0f, 1.0f);
	const ImVec4 ColorPink   = ImVec4(1.0f, 0.5f, 0.5f, 1.0f);
	const ImVec4 ColorRed      = ImVec4(1.0f, 0.2f, 0.2f, 1.0f); // 미사용
	const ImVec4 ColorPurple   = ImVec4(0.7f, 0.4f, 1.0f, 1.0f);
	const ImVec4 ColorMint     = ImVec4(0.3f, 1.0f, 0.7f, 1.0f);

	const char* GetShadowStatsTypeName(ELightType LightType)
	{
		switch (LightType)
		{
		case ELightType::LightType_Directional:
			return "Directional";
		case ELightType::LightType_Spot:
			return "Spot";
		case ELightType::LightType_Point:
			return "Point";
		default:
			return "Unknown";
		}
	}

	const char* GetShadowStatsProjectionName(const FLightShadowStat& Stat)
	{
		if (Stat.ProjectionMode == EShadowProjectionMode::PSM)
		{
			return "PSM";
		}

		return Stat.bUsesCSM ? "CSM" : "Standard";
	}

	const char* GetShadowStatsProjectionName(const FShadowMapStat& Stat)
	{
		if (Stat.ProjectionMode == EShadowProjectionMode::PSM)
		{
			return "PSM";
		}

		return Stat.bUsesCSM ? "CSM" : "Standard";
	}

	const char* GetShadowStatsFormatName(DXGI_FORMAT Format)
	{
		switch (Format)
		{
		case DXGI_FORMAT_D32_FLOAT:
			return "D32_FLOAT";
		case DXGI_FORMAT_R32_FLOAT:
			return "R32_FLOAT";
		case DXGI_FORMAT_R32_TYPELESS:
			return "R32_TYPELESS";
		case DXGI_FORMAT_D24_UNORM_S8_UINT:
			return "D24_UNORM_S8_UINT";
		case DXGI_FORMAT_R24G8_TYPELESS:
			return "R24G8_TYPELESS";
		case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
			return "R24_UNORM_X8_TYPELESS";
		case DXGI_FORMAT_D16_UNORM:
			return "D16_UNORM";
		case DXGI_FORMAT_R16_TYPELESS:
			return "R16_TYPELESS";
		case DXGI_FORMAT_R16G16_FLOAT:
			return "R16G16_FLOAT";
		case DXGI_FORMAT_R32G32_FLOAT:
			return "R32G32_FLOAT";
		default:
			return "UNKNOWN";
		}
	}

	const char* GetShadowStatsFaceName(int32 FaceIndex)
	{
		switch (FaceIndex)
		{
		case 0:
			return "+X";
		case 1:
			return "-X";
		case 2:
			return "+Y";
		case 3:
			return "-Y";
		case 4:
			return "+Z";
		case 5:
			return "-Z";
		default:
			return "?";
		}
	}

	int32 GetShadowStatsViewportIndex(const FEditorViewportLayout& Layout)
	{
		const int32 FocusedViewportIndex = Layout.GetLastFocusedViewportIndex();
		if (FocusedViewportIndex >= 0 &&
			FocusedViewportIndex < FEditorViewportLayout::MaxViewports &&
			Layout.GetViewportState(FocusedViewportIndex).bShowStatShadow)
		{
			return FocusedViewportIndex;
		}

		for (int32 ViewportIndex = 0; ViewportIndex < FEditorViewportLayout::MaxViewports; ++ViewportIndex)
		{
			if (Layout.GetViewportState(ViewportIndex).bShowStatShadow)
			{
				return ViewportIndex;
			}
		}

		return -1;
	}

	void SetShadowStatsEnabled(FEditorViewportLayout& Layout, bool bEnabled)
	{
		for (int32 ViewportIndex = 0; ViewportIndex < FEditorViewportLayout::MaxViewports; ++ViewportIndex)
		{
			Layout.GetViewportState(ViewportIndex).bShowStatShadow = bEnabled;
		}
	}

	FString BuildShadowStatsLightLabel(const FLightShadowStat& Stat)
	{
		FString Label = "#" + std::to_string(Stat.LightIndex);
		if (Stat.SourceLightSlotIndex != 0xFFFFFFFF)
		{
			Label += " / Slot " + std::to_string(Stat.SourceLightSlotIndex);
		}

		if (!Stat.LightName.empty())
		{
			Label += " " + Stat.LightName;
		}

		return Label;
	}

	FString BuildShadowStatsMapSummary(const FLightShadowStat& Stat)
	{
		FString Label = std::to_string(Stat.LogicalShadowMapCount);
		if (Stat.LightType == ELightType::LightType_Point)
		{
			Label += " faces";
		}
		else if (Stat.bUsesCSM)
		{
			Label += " cascades";
		}

		if (Stat.ResourceViewCount != Stat.LogicalShadowMapCount)
		{
			Label += " (" + std::to_string(Stat.ResourceViewCount) + " views)";
		}

		return Label;
	}

	FString BuildShadowStatsResolutionSummary(const FLightShadowStat& Stat)
	{
		if (Stat.ShadowMaps.empty())
		{
			return "-";
		}

		uint32 MinWidth = Stat.ShadowMaps[0].Width;
		uint32 MaxWidth = Stat.ShadowMaps[0].Width;
		uint32 MinHeight = Stat.ShadowMaps[0].Height;
		uint32 MaxHeight = Stat.ShadowMaps[0].Height;

		for (const FShadowMapStat& MapStat : Stat.ShadowMaps)
		{
			MinWidth = std::min(MinWidth, MapStat.Width);
			MaxWidth = std::max(MaxWidth, MapStat.Width);
			MinHeight = std::min(MinHeight, MapStat.Height);
			MaxHeight = std::max(MaxHeight, MapStat.Height);
		}

		if (MinWidth == MaxWidth && MinHeight == MaxHeight)
		{
			return std::to_string(MaxWidth) + "x" + std::to_string(MaxHeight);
		}

		return std::to_string(MaxWidth) + "x" + std::to_string(MaxHeight) +
			" .. " +
			std::to_string(MinWidth) + "x" + std::to_string(MinHeight);
	}

	FString BuildShadowStatsFormatSummary(const FLightShadowStat& Stat)
	{
		if (Stat.ShadowMaps.empty())
		{
			return "-";
		}

		FString FirstFormat = GetShadowStatsFormatName(Stat.ShadowMaps[0].Format);
		bool bMixed = false;
		for (const FShadowMapStat& MapStat : Stat.ShadowMaps)
		{
			if (FirstFormat != GetShadowStatsFormatName(MapStat.Format))
			{
				bMixed = true;
				break;
			}
		}

		return bMixed ? "Mixed" : FirstFormat;
	}

	FString BuildShadowStatsDetailLabel(const FShadowMapStat& Stat)
	{
		FString Label = Stat.Name;
		if (Stat.bHasAtlasRegion)
		{
			Label += " [Atlas " + std::to_string(Stat.AtlasX) + ", " + std::to_string(Stat.AtlasY) + "]";
		}
		else if (Stat.ArraySlice != 0u || Stat.FaceIndex >= 0 || Stat.CascadeIndex >= 0)
		{
			Label += " [Slice " + std::to_string(Stat.ArraySlice) + "]";
		}

		if (Stat.MipLevel > 0u)
		{
			Label += " [Mip " + std::to_string(Stat.MipLevel) + "]";
		}

		return Label;
	}
}

// 뷰포트 타입(Enum)을 UI에 표시할 문자열로 변환합니다.
static const char* GetViewportTypeName(EEditorViewportType Type)
{
	switch (Type)
	{
	case EVT_Perspective: return "Perspective";
	case EVT_OrthoTop:    return "Top";
	case EVT_OrthoBottom: return "Bottom";
	case EVT_OrthoFront:  return "Front";
	case EVT_OrthoBack:   return "Back";
	case EVT_OrthoLeft:   return "Left";
	case EVT_OrthoRight:  return "Right";
	default:              return "Viewport";
	}
}

// 뷰 모드(Enum)를 UI에 표시할 문자열로 변환합니다.
static const char* GetViewModeName(EViewMode Mode)
{
	switch (Mode)
	{
	case EViewMode::Lit:         return "Lit";
	case EViewMode::Unlit:       return "Unlit";
	case EViewMode::Wireframe:   return "Wireframe";
	case EViewMode::SceneDepth:  return "Scene Depth";
	case EViewMode::WorldNormal: return "World Normal";
	default:                     return "Lit";
	}
}

// ──────────── FEditorViewPortOverlayWidget의 메인 렌더링 함수입니다. ────────────
void FEditorViewportOverlayWidget::Render(float DeltaTime)
{
	if (bShowViewportSettings)
	{
		RenderViewportSettings(DeltaTime);
	}
	RenderShadowStatsWindow();
	RenderDebugStats(DeltaTime);
	RenderSplitterBar();
	RenderBoxSelectionOverlay();
	RenderShortcutsWindow();
}

// 뷰포트 설정(표시 플래그, 그리드, 카메라 감도, BVH 관리 정책 등)을 조작하는 창을 렌더링합니다.
void FEditorViewportOverlayWidget::RenderViewportSettings(float DeltaTime)
{
	FEditorSettings& Settings = FEditorSettings::Get();

	if (!ImGui::Begin("Viewport Settings"))
	{
		ImGui::End();
		return;
	}

	// 위젯 너비를 현재 창 콘텐츠 영역의 50%로 설정하는 람다 또는 변수
	float ItemWidth = ImGui::GetContentRegionAvail().x * 0.5f;
	auto SetControlWidth = [ItemWidth]()
	{ ImGui::SetNextItemWidth(ItemWidth); };
	auto BeginSettingsSection = [](const char* Label, bool bDefaultOpen)
	{
		ImGui::SetNextItemOpen(bDefaultOpen, ImGuiCond_FirstUseEver);
		return ImGui::CollapsingHeader(Label);
	};

	// Show Flags
	if (BeginSettingsSection("Show Flags", true))
	{
		ImGui::Checkbox("Primitives", &Settings.ShowFlags.bPrimitives);
		ImGui::Checkbox("BillboardText", &Settings.ShowFlags.bBillboardText);
		ImGui::Checkbox("Axis", &Settings.ShowFlags.bAxis);
		ImGui::Checkbox("Grid", &Settings.ShowFlags.bGrid);
		ImGui::Checkbox("Gizmo", &Settings.ShowFlags.bGizmo);
		ImGui::Checkbox("Bounding Volume", &Settings.ShowFlags.bBoundingVolume);
		if (Settings.ShowFlags.bBoundingVolume)
		{
			ImGui::Indent();
			ImGui::Checkbox("BVH Bounding Volume", &Settings.ShowFlags.bBVHBoundingVolume);
			ImGui::Unindent();
		}
		ImGui::Checkbox("Enable LOD", &Settings.ShowFlags.bEnableLOD);
		ImGui::Checkbox("Decals", &Settings.ShowFlags.bDecals);
		ImGui::Checkbox("Fog", &Settings.ShowFlags.bFog);
	}

	if (BeginSettingsSection("Light Settings", false))
	{
		ImGui::Checkbox("Directional Light Debug", &Settings.ShowFlags.bDirectionalLightDebug);
		ImGui::Checkbox("Point Light Debug", &Settings.ShowFlags.bPointLightDebug);
		ImGui::Checkbox("Spot Light Debug", &Settings.ShowFlags.bSpotLightDebug);
		ImGui::Checkbox("Light Hitmap Overlay", &Settings.ShowFlags.bShowLightHitmapOverlay);
	}

	if (BeginSettingsSection("Shadow Settings", false))
	{
		int32 ShadowProjectionMode = static_cast<int32>(Settings.ShowFlags.ShadowProjection);
		ImGui::SetNextItemWidth(ItemWidth);
		if (ImGui::Combo("Projection Mode", &ShadowProjectionMode, "Standard\0PSM\0"))
		{
			Settings.ShowFlags.ShadowProjection = SanitizeShadowProjectionMode(ShadowProjectionMode);
		}

		int32 ShadowFilterMode = static_cast<int32>(Settings.ShowFlags.ShadowFilter);
		ImGui::SetNextItemWidth(ItemWidth);
		if (ImGui::Combo("Filter Mode", &ShadowFilterMode, "SSM\0SSM + PCF\0VSM\0"))
		{
			Settings.ShowFlags.ShadowFilter = SanitizeShadowFilterMode(ShadowFilterMode);
		}

		ImGui::Text("Active Projection: %s", GetShadowProjectionModeDisplayName(Settings.ShowFlags.ShadowProjection));
		ImGui::Text("Active Filter: %s", GetShadowFilterModeDisplayName(Settings.ShowFlags.ShadowFilter));
		ImGui::TextWrapped("Standard/PSM selects the shadow projection build. SSM uses one hard depth compare, SSM + PCF reuses the depth map with filtered comparisons, and VSM switches directional/point lights to moment textures. Spot lights still use the depth atlas, so they fall back to PCF when VSM is selected.");
	}

	// Camera Sensitivity
	if (BeginSettingsSection("Camera Settings", true))
	{
		SetControlWidth();
		ImGui::SliderFloat("Move Sensitivity", &Settings.CameraMoveSensitivity, 0.05f, 5.0f, "%.1f");

		SetControlWidth();
		ImGui::SliderFloat("Rotate Sensitivity", &Settings.CameraRotateSensitivity, 0.05f, 5.0f, "%.1f");

		if (EditorEngine)
		{
			FEditorViewportLayout& Layout = EditorEngine->GetViewportLayout();
			const int32 FocusedIdx = Layout.GetLastFocusedViewportIndex();
			FEditorViewportClient* FocusedClient = Layout.GetViewportClient(FocusedIdx);
			float CameraMoveSpeed = FocusedClient->GetMoveSpeed();

			(void)CameraMoveSpeed;

			SetControlWidth(); // 너비 설정
			ImGui::SliderFloat("Zoom Speed", &Settings.CameraZoomSpeed, 0.01f, 30.0f, "%.2f");
		}
	}

	// Grid Settings
	if (BeginSettingsSection("Grid / Axis Settings", false))
	{
		SetControlWidth();
		ImGui::SliderFloat("Spacing", &Settings.GridSpacing, 0.1f, 10.0f, "%.1f");
		SetControlWidth();
		ImGui::SliderInt("Half Line Count", &Settings.GridHalfLineCount, 10, 500);
		SetControlWidth();
		ImGui::SliderFloat("Line Thickness", &Settings.GridRenderSettings.LineThickness, 0.0f, 4.0f, "%.2f");
		SetControlWidth();
		ImGui::SliderFloat("Major Line Thickness", &Settings.GridRenderSettings.MajorLineThickness, 0.0f, 6.0f, "%.2f");
		SetControlWidth();
		ImGui::SliderInt("Major Line Interval", &Settings.GridRenderSettings.MajorLineInterval, 1, 50);
		SetControlWidth();
		ImGui::SliderFloat("Minor Intensity", &Settings.GridRenderSettings.MinorIntensity, 0.0f, 1.5f, "%.2f");
		SetControlWidth();
		ImGui::SliderFloat("Major Intensity", &Settings.GridRenderSettings.MajorIntensity, 0.0f, 1.5f, "%.2f");
		SetControlWidth();
		ImGui::SliderFloat("Axis Thickness", &Settings.GridRenderSettings.AxisThickness, 0.0f, 8.0f, "%.2f");
		SetControlWidth();
		ImGui::SliderFloat("Axis Intensity", &Settings.GridRenderSettings.AxisIntensity, 0.0f, 1.5f, "%.2f");
		SetControlWidth();
		ImGui::SliderFloat("Axis Length Scale", &Settings.GridRenderSettings.AxisLengthScale, 0.25f, 4.0f, "%.2f");
	}

	if (BeginSettingsSection("BVH Settings", false))
	{
		bool bPolicyChanged = false;

		SetControlWidth();
		bPolicyChanged |= ImGui::SliderInt("Batch Refit Min Dirty", &Settings.SpatialBatchRefitMinDirtyCount, 1, 256);

		SetControlWidth();
		bPolicyChanged |=
			ImGui::SliderInt("Batch Refit Dirty %%", &Settings.SpatialBatchRefitDirtyPercentThreshold, 1, 100);

		SetControlWidth();
		bPolicyChanged |=
			ImGui::SliderInt("Rotation Structural Changes", &Settings.SpatialRotationStructuralChangeThreshold, 1, 256);

		SetControlWidth();
		bPolicyChanged |=
			ImGui::SliderInt("Rotation Dirty Count", &Settings.SpatialRotationDirtyCountThreshold, 1, 512);

		SetControlWidth();
		bPolicyChanged |= ImGui::SliderInt("Rotation Dirty %%", &Settings.SpatialRotationDirtyPercentThreshold, 1, 100);

		if (bPolicyChanged && EditorEngine)
		{
			EditorEngine->ApplySpatialIndexMaintenanceSettings();
		}
	}

	// FXAA Settings
	if (BeginSettingsSection("FXAA", false))
	{
		ImGui::Checkbox("Enable FXAA", &Settings.bEnableFXAA);
	}

	ImGui::End();
}

void FEditorViewportOverlayWidget::RenderShadowStatsWindow()
{
	if (!EditorEngine)
	{
		return;
	}

	FEditorViewportLayout& Layout = EditorEngine->GetViewportLayout();
	const int32 ShadowStatsViewportIndex = GetShadowStatsViewportIndex(Layout);
	if (ShadowStatsViewportIndex < 0)
	{
		return;
	}

	const UWorld* ShadowStatsWorld = EditorEngine->GetFocusedWorld();
	const FShadowStats ShadowStats = FShadowPass::GetShadowStats(ShadowStatsViewportIndex, ShadowStatsWorld);

	ImGui::SetNextWindowSize(ImVec2(980.0f, 440.0f), ImGuiCond_FirstUseEver);
	bool bWindowOpen = true;
	if (!ImGui::Begin("Shadow Stats", &bWindowOpen))
	{
		ImGui::End();
		if (!bWindowOpen)
		{
			SetShadowStatsEnabled(Layout, false);
		}
		return;
	}

	ImGui::Text("Viewport: %d", ShadowStatsViewportIndex);
	ImGui::Text(
		"Shadow Lights: %u | Logical Maps: %u | Resource Views: %u",
		ShadowStats.ShadowCastingLightCount,
		ShadowStats.TotalShadowMapCount,
		ShadowStats.TotalResourceViewCount);
	ImGui::Text(
		"Total: %s | Depth: %s | VSM Moments: %s | VSM Temp: %s",
		FShadowPass::FormatBytes(ShadowStats.TotalMemoryBytes).c_str(),
		FShadowPass::FormatBytes(ShadowStats.TotalDepthMemoryBytes).c_str(),
		FShadowPass::FormatBytes(ShadowStats.TotalVSMMomentMemoryBytes).c_str(),
		FShadowPass::FormatBytes(ShadowStats.TotalVSMTempMemoryBytes).c_str());
	if (ShadowStats.TotalAtlasMemoryBytes > 0u)
	{
		ImGui::Text("Shared Atlas Memory: %s", FShadowPass::FormatBytes(ShadowStats.TotalAtlasMemoryBytes).c_str());
	}
	ImGui::TextWrapped("Per-light memory is a logical allocation estimate. Shared atlases and shared cubemap arrays are counted once in the total summary.");
	ImGui::Separator();

	if (ShadowStats.Lights.empty())
	{
		ImGui::TextUnformatted("No active shadow resources for the focused viewport.");
		ImGui::End();
		return;
	}

	constexpr ImGuiTableFlags TableFlags =
		ImGuiTableFlags_RowBg |
		ImGuiTableFlags_Borders |
		ImGuiTableFlags_Resizable |
		ImGuiTableFlags_ScrollY;

	if (ImGui::BeginTable("##ShadowStatsTable", 9, TableFlags, ImVec2(0.0f, 0.0f)))
	{
		ImGui::TableSetupColumn("Light", ImGuiTableColumnFlags_WidthStretch, 2.4f);
		ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 0.9f);
		ImGui::TableSetupColumn("Cast Shadow", ImGuiTableColumnFlags_WidthFixed, 0.95f);
		ImGui::TableSetupColumn("Projection", ImGuiTableColumnFlags_WidthFixed, 0.95f);
		ImGui::TableSetupColumn("Filter", ImGuiTableColumnFlags_WidthFixed, 0.95f);
		ImGui::TableSetupColumn("Shadow Maps", ImGuiTableColumnFlags_WidthFixed, 1.1f);
		ImGui::TableSetupColumn("Resolution", ImGuiTableColumnFlags_WidthFixed, 1.1f);
		ImGui::TableSetupColumn("Format", ImGuiTableColumnFlags_WidthFixed, 1.0f);
		ImGui::TableSetupColumn("Memory", ImGuiTableColumnFlags_WidthFixed, 1.0f);
		ImGui::TableHeadersRow();

		for (const FLightShadowStat& LightStat : ShadowStats.Lights)
		{
			ImGui::PushID(static_cast<int32>(LightStat.LightIndex));
			ImGui::TableNextRow();

			const bool bHasChildren = !LightStat.ShadowMaps.empty();
			ImGuiTreeNodeFlags RowFlags =
				ImGuiTreeNodeFlags_SpanFullWidth |
				ImGuiTreeNodeFlags_SpanAvailWidth;
			if (!bHasChildren)
			{
				RowFlags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
			}

			ImGui::TableSetColumnIndex(0);
			const bool bOpen = ImGui::TreeNodeEx("##ShadowLightRow", RowFlags, "%s", BuildShadowStatsLightLabel(LightStat).c_str());

			ImGui::TableSetColumnIndex(1);
			ImGui::TextUnformatted(GetShadowStatsTypeName(LightStat.LightType));

			ImGui::TableSetColumnIndex(2);
			ImGui::TextUnformatted(LightStat.bCastShadow ? "Yes" : "No");

			ImGui::TableSetColumnIndex(3);
			ImGui::TextUnformatted(GetShadowStatsProjectionName(LightStat));

			ImGui::TableSetColumnIndex(4);
			ImGui::TextUnformatted(GetShadowFilterModeDisplayName(LightStat.FilterMode));

			ImGui::TableSetColumnIndex(5);
			ImGui::TextUnformatted(BuildShadowStatsMapSummary(LightStat).c_str());

			ImGui::TableSetColumnIndex(6);
			ImGui::TextUnformatted(BuildShadowStatsResolutionSummary(LightStat).c_str());

			ImGui::TableSetColumnIndex(7);
			ImGui::TextUnformatted(BuildShadowStatsFormatSummary(LightStat).c_str());

			ImGui::TableSetColumnIndex(8);
			ImGui::TextUnformatted(FShadowPass::FormatBytes(LightStat.TotalMemoryBytes).c_str());

			if (bOpen && bHasChildren)
			{
				for (const FShadowMapStat& MapStat : LightStat.ShadowMaps)
				{
					ImGui::TableNextRow();

					ImGui::TableSetColumnIndex(0);
					ImGui::Indent();
					ImGui::BulletText("%s", BuildShadowStatsDetailLabel(MapStat).c_str());
					ImGui::Unindent();

					ImGui::TableSetColumnIndex(1);
					ImGui::TextUnformatted("");

					ImGui::TableSetColumnIndex(2);
					ImGui::TextUnformatted(MapStat.bSharedResource ? "Shared" : "");

					ImGui::TableSetColumnIndex(3);
					ImGui::TextUnformatted(GetShadowStatsProjectionName(MapStat));

					ImGui::TableSetColumnIndex(4);
					ImGui::TextUnformatted(GetShadowFilterModeDisplayName(MapStat.FilterMode));

					ImGui::TableSetColumnIndex(5);
					if (MapStat.FaceIndex >= 0)
					{
						ImGui::Text("Face %s", GetShadowStatsFaceName(MapStat.FaceIndex));
					}
					else if (MapStat.CascadeIndex >= 0)
					{
						ImGui::Text("Cascade %d", MapStat.CascadeIndex);
					}
					else
					{
						ImGui::TextUnformatted("1");
					}

					ImGui::TableSetColumnIndex(6);
					ImGui::Text("%ux%u", MapStat.Width, MapStat.Height);

					ImGui::TableSetColumnIndex(7);
					ImGui::TextUnformatted(GetShadowStatsFormatName(MapStat.Format));

					ImGui::TableSetColumnIndex(8);
					const FString MemoryLabel =
						FShadowPass::FormatBytes(MapStat.MemoryBytes) +
						(MapStat.bSharedResource ? " est." : "");
					ImGui::TextUnformatted(MemoryLabel.c_str());
				}

				ImGui::TreePop();
			}

			ImGui::PopID();
		}

		ImGui::EndTable();
	}

	ImGui::End();

	if (!bWindowOpen)
	{
		SetShadowStatsEnabled(Layout, false);
	}
}

// 활성화된 뷰포트를 순회하며 설정에 따라 디버그 스탯(FPS, Culling, Memory 등) 오버레이를 화면에 배치하고 렌더링합니다.
void FEditorViewportOverlayWidget::RenderDebugStats(float DeltaTime)
{
	if (!EditorEngine) return;

	FEditorViewportLayout& Layout = EditorEngine->GetViewportLayout();

	for (int32 i = 0; i < FEditorViewportLayout::MaxViewports; ++i)
	{
		const FEditorViewportState& VS = Layout.GetViewportState(i);
		FViewportRect ViewportRect = Layout.GetSceneViewport(i).GetRect();

		if (!VS.bShowStatFPS && !VS.bShowStatMemory && !VS.bShowStatNameTable && !VS.bShowStatLightCull) 
			continue;
		
		if (ViewportRect.Width <= 0 || ViewportRect.Height <= 0) 
			continue;

		ImVec2 CurrentDrawPos(static_cast<float>(ViewportRect.X) + 8.f, static_cast<float>(ViewportRect.Y) + 32.f);

		float GeneralWidth = RenderGeneralStatsWindow(i, VS, CurrentDrawPos, DeltaTime);
		if (GeneralWidth > 0.f)
			CurrentDrawPos.x += GeneralWidth + 8.f;

		float NTWidth = RenderNameTableWindow(i, VS, CurrentDrawPos);
		if (NTWidth > 0.f)
			CurrentDrawPos.x += NTWidth + 8.f;

		float LightCullWidth = RenderLightCullWindow(i, VS, CurrentDrawPos);

		// [중요] 통계를 띄우고 싶을 경우엔 여기에 위의 양식과 똑같이 추가합니다.
	}
}

// 다중 뷰포트 모드에서 뷰포트 간의 경계선(Splitter) 및 교차점(Cross)을 드래그 시 강조해 렌더링합니다.
void FEditorViewportOverlayWidget::RenderSplitterBar()
{
	 // 뷰포트를 클릭했거나, 휠 드래그를 하고 있을 때 강조하지 않습니다.
	if (FSlateApplication::Get().GetCapturedWidget() || InputSystem::Get().GetMiddleDragging())
		 return;

	// 기즈모를 잡고 있을 때 강조하지 않습니다.
	bool bIsHodingGizmo = EditorEngine->GetGizmo()->IsHolding();

	 if (bIsHodingGizmo || InputSystem::Get().GetRightDragging())
	 {
		 return;
	 }

	 if (!EditorEngine) return;
	
	FEditorViewportLayout& ViewportLayout = EditorEngine->GetViewportLayout();

	// 뷰포트가 1개일 때 Splitter Bar를 띄우지 않습니다.
	if (!ViewportLayout.IsSingleViewportMode())
	{
		ImDrawList* DrawList = ImGui::GetForegroundDrawList();
		constexpr ImU32 BarColor = IM_COL32(80, 80, 80, 220);
		constexpr ImU32 HoverColor = IM_COL32(140, 180, 255, 255);

		const SWidget* Hovered  = FSlateApplication::Get().GetHoveredWidget();
		const SWidget* Captured = FSlateApplication::Get().GetCapturedWidget();

		const bool bIsDragging = InputSystem::Get().GetRightDragging();

		SSplitterCross* Cross = ViewportLayout.GetCrossWidget();
		constexpr ImU32 CrossHoverColor = IM_COL32(140, 180, 255, 255);

		const bool bCrossHovered = (Cross && Cross == Hovered);

		SSplitter* Splitters[] = {
			ViewportLayout.GetRootSplitterV(),
			ViewportLayout.GetTopSplitterH(),
			ViewportLayout.GetBotSplitterH()
		};

		for (SSplitter* S : Splitters)
		{
			if (!S) continue;
			const FRect Bar = S->GetBarRect();

			const SSplitter* Linked = S->GetLinkedSplitter();
			const bool bSplitterHover = !bIsDragging
				&& ((S == Hovered || S == Captured)
					|| (Linked && (Linked == Hovered || Linked == Captured)));

			ImU32 Color = BarColor;
			if (bCrossHovered)       Color = CrossHoverColor;
			else if (bSplitterHover) Color = HoverColor;

			DrawList->AddRectFilled(
				ImVec2(Bar.X, Bar.Y),
				ImVec2(Bar.X + Bar.Width, Bar.Y + Bar.Height),
				Color);
		}

		if (Cross)
		{
			const FRect CR = Cross->GetCrossRect();
			DrawList->AddRectFilled(
				ImVec2(CR.X, CR.Y),
				ImVec2(CR.X + CR.Width, CR.Y + CR.Height),
				bCrossHovered ? CrossHoverColor : BarColor);
		}
	}
}

// 마우스 드래그를 통한 다중 선택(Box Selection) 시 뷰포트 위에 반투명한 선택 영역 박스를 렌더링합니다.
void FEditorViewportOverlayWidget::RenderBoxSelectionOverlay()
{
	if (!EditorEngine)
	{
		return;
	}

	FEditorViewportLayout& Layout = EditorEngine->GetViewportLayout();
	ImDrawList* DrawList = ImGui::GetForegroundDrawList();
	const bool bAdditive = InputSystem::Get().GetKey(VK_SHIFT);
	const ImU32 RectColor = bAdditive ? IM_COL32(128, 240, 128, 220) : IM_COL32(128, 192, 255, 220);
	const ImU32 FillColor = bAdditive ? IM_COL32(64, 180, 64, 40) : IM_COL32(64, 128, 220, 40);

	for (int32 i = 0; i < FEditorViewportLayout::MaxViewports; ++i)
	{
		const FEditorViewportState& VS = Layout.GetViewportState(i);
		FViewportRect ViewportRect = Layout.GetSceneViewport(i).GetRect();
		if (ViewportRect.Width <= 0 || ViewportRect.Height <= 0)
		{
			continue;
		}

		const FEditorViewportClient* Client = Layout.GetViewportClient(i);
		if (!Client->IsBoxSelecting())
		{
			continue;
		}

		const POINT Start = Client->GetBoxSelectStart();
		const POINT End = Client->GetBoxSelectEnd();

		const float MinX = static_cast<float>(std::min(Start.x, End.x));
		const float MinY = static_cast<float>(std::min(Start.y, End.y));
		const float MaxX = static_cast<float>(std::max(Start.x, End.x));
		const float MaxY = static_cast<float>(std::max(Start.y, End.y));

		const ImVec2 P0(static_cast<float>(ViewportRect.X) + MinX, static_cast<float>(ViewportRect.Y) + MinY);
		const ImVec2 P1(static_cast<float>(ViewportRect.X) + MaxX, static_cast<float>(ViewportRect.Y) + MaxY);
		DrawList->AddRectFilled(P0, P1, FillColor);
		DrawList->AddRect(P0, P1, RectColor, 0.0f, 0, 1.5f);
	}
}

// 현재 에디터에서 사용 가능한 단축키 목록을 보여주는 팝업 창을 렌더링합니다.
void FEditorViewportOverlayWidget::RenderShortcutsWindow()
{
	if (!bShowShortcutsWindow)
	{
		return;
	}

	ImGui::OpenPopup("Shortcuts##Modal");
	ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, ImVec4(0.0f, 0.0f, 0.0f, 0.55f));
	ImGui::SetNextWindowSize(ImVec2(760.0f, 560.0f), ImGuiCond_Appearing);

	if (!ImGui::BeginPopupModal("Shortcuts##Modal", &bShowShortcutsWindow, ImGuiWindowFlags_NoResize))
	{
		ImGui::PopStyleColor();
		return;
	}

	if (!bShowShortcutsWindow)
	{
		ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
		ImGui::PopStyleColor();
		return;
	}

	if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
	{
		bShowShortcutsWindow = false;
		ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
		ImGui::PopStyleColor();
		return;
	}

	ImGui::TextUnformatted("Shortcuts");

	ImGui::Separator();
	ImGui::Text("현재 코드상 실제로 동작하는 에디터 단축키만 정리했습니다.");

	auto DrawShortcutTable = [](const char* Header, std::initializer_list<TPair<const char*, const char*>> Rows)
	{
		if (!ImGui::CollapsingHeader(Header, ImGuiTreeNodeFlags_DefaultOpen))
		{
			return;
		}

		if (ImGui::BeginTable(Header, 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_BordersOuter))
		{
			ImGui::TableSetupColumn("Shortcut");
			ImGui::TableSetupColumn("Action");
			ImGui::TableHeadersRow();

			for (const auto& Row : Rows)
			{
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				ImGui::TextUnformatted(Row.first);
				ImGui::TableSetColumnIndex(1);
				ImGui::TextUnformatted(Row.second);
			}

			ImGui::EndTable();
		}
	};

	DrawShortcutTable("Viewport Navigation",
	{
		{"Mouse Right Drag", "뷰포트 카메라 회전"},
		{"Mouse Middle Drag", "뷰포트 카메라 팬 이동"},
		{"Alt + Mouse Right Drag", "카메라 돌리 인/아웃"},
		{"Mouse Wheel", "원근 카메라 FOV 또는 직교 카메라 높이 조절"},
		{"Mouse Wheel while rotating", "카메라 이동 속도 조절"},
		{"W / A / S / D / Q / E", "카메라 이동 (회전 중일 때만 적용)"},
		{"F", "현재 선택된 Actor 쪽으로 카메라 포커스"},
	});

	DrawShortcutTable("Selection",
	{
		{"Mouse Left Click", "Actor 단일 선택"},
		{"Shift + Mouse Left Click", "선택 추가"},
		{"Ctrl + Mouse Left Click", "선택 토글"},
		{"Ctrl + A", "전체 Actor 선택"},
		{"Ctrl + Alt + Drag", "박스 선택"},
		{"Ctrl + Alt + Shift + Drag", "기존 선택에 박스 선택 추가"},
	});

	DrawShortcutTable("Gizmo",
	{
		{"Mouse Left Drag", "기즈모 축 드래그 조작"},
		{"Space", "기즈모 타입 순환"},
		{"X", "월드/로컬 기즈모 모드 전환"},
	});

	DrawShortcutTable("Editor",
	{
		{"Delete", "선택된 Actor 삭제"},
	});

	ImGui::Spacing();
	ImGui::TextUnformatted("참고: ImGui 입력창이 키보드를 잡고 있을 때는 일부 단축키가 동작하지 않습니다.");
	ImGui::EndPopup();
	ImGui::PopStyleColor();
}

// ──────────── 헬퍼 함수  ────────────

// 특정 뷰포트의 일반적인 렌더링 통계(FPS, Culling, Decal, Memory) 정보를 출력하는 창을 그립니다.
float FEditorViewportOverlayWidget::RenderGeneralStatsWindow(int32 ViewportIndex, const FEditorViewportState& VS, const ImVec2& Pos, float DeltaTime)
{
	if (!VS.bShowStatFPS && !VS.bShowStatMemory) 
		return 0.f;

	const FEditorRenderPipeline* RenderPipeline = EditorEngine->GetEditorRenderPipeline();

	ImGui::SetNextWindowPos(Pos, ImGuiCond_Always);
	ImGui::SetNextWindowBgAlpha(0.3f);

	char WinId[32];
	snprintf(WinId, sizeof(WinId), "##StatOverlay_%d", ViewportIndex);

	float WindowWidth = 0.f;
	if (ImGui::Begin(WinId, nullptr, kStatFlags))
	{
		const FRenderCollector::FCullingStats* CullingStats = 
			(RenderPipeline != nullptr) ? &RenderPipeline->GetViewportCullingStats(ViewportIndex) : nullptr;

		if (VS.bShowStatFPS)
		{
			const float FPS = (DeltaTime > 0.f) ? (1.f / DeltaTime) : 0.f;
			ImGui::TextColored(ColorGreen, "FPS: %.1f (%.2f ms)", FPS, DeltaTime * 1000.f);
		}

		if (CullingStats != nullptr)
		{
			const int32 CulledPrimitiveCount = std::max(0, CullingStats->TotalVisiblePrimitiveCount - (CullingStats->BVHPassedPrimitiveCount + CullingStats->FallbackPassedPrimitiveCount));
			if (VS.bShowStatFPS) ImGui::Separator();

			ImGui::TextColored(ColorCyan, "Culling");
			ImGui::TextColored(ColorPaleBlue, "- Total Visible: %d", CullingStats->TotalVisiblePrimitiveCount);
			ImGui::TextColored(ColorPaleBlue, "- BVH Passed: %d", CullingStats->BVHPassedPrimitiveCount);
			ImGui::TextColored(ColorPaleBlue, "- Fallback Passed: %d", CullingStats->FallbackPassedPrimitiveCount);
			ImGui::TextColored(ColorPaleBlue, "- Culled: %d", CulledPrimitiveCount);
		}

		const FRenderCollector::FDecalStats* DecalStats = 
			(RenderPipeline != nullptr) ? &RenderPipeline->GetViewportDecalStats(ViewportIndex) : nullptr;
		
		if (DecalStats != nullptr)
		{
			if (CullingStats != nullptr || VS.bShowStatFPS) ImGui::Separator();

			ImGui::TextColored(ColorOrange, "Decal");
			ImGui::TextColored(ColorPaleBlue, "- Total Decals: %d", DecalStats->TotalDecalCount);
			ImGui::TextColored(ColorPaleBlue, "- Decal Time: %.4f ms", DecalStats->CollectTimeMS / 1000.f);
		}

		if (VS.bShowStatMemory)
		{
			if (CullingStats != nullptr || VS.bShowStatFPS) ImGui::Separator();

			size_t MeshMemoryBytes = 0;
			for (TObjectIterator<UStaticMesh> It; It; ++It)
			{
				UStaticMesh* Mesh = *It;
				if (Mesh && Mesh->HasValidMeshData())
				{
					MeshMemoryBytes += sizeof(UStaticMesh)
						+ Mesh->GetVertices().size()  * sizeof(FNormalVertex)
						+ Mesh->GetIndices().size()   * sizeof(uint32)
						+ Mesh->GetSections().size()  * sizeof(FStaticMeshSection);
				}
			}

			const size_t MatMemoryBytes = FResourceManager::Get().GetMaterialMemorySize();

			ImGui::TextColored(ColorYellow, "Memory Stat");
			ImGui::TextColored(ColorPaleBlue, "- Mesh: %.2f KB", MeshMemoryBytes / 1024.f);
			ImGui::TextColored(ColorPaleBlue, "- Material: %.2f KB", MatMemoryBytes / 1024.f);
			ImGui::Separator();

			FNamePool& Pool = FNamePool::Get();
			
			ImGui::TextColored(ColorPink, "FName Stat");
			ImGui::TextColored(ColorPaleBlue, "- Entries: %u", Pool.GetEntryCount());
			ImGui::TextColored(ColorPaleBlue, "- Size: %.2f KB", Pool.GetTotalBytes() / 1024.f);
			ImGui::Separator();

			ImGui::TextColored(ColorPaleBlue, "- Total Allocated Counts: %d", EngineStatics::GetTotalAllocationCount());
			ImGui::TextColored(ColorPaleBlue, "- Total Allocated Bytes: %.2f KB", EngineStatics::GetTotalAllocationBytes() / 1024.f);
		}

		WindowWidth = ImGui::GetWindowSize().x; 
	}
	ImGui::End();

	return WindowWidth;
}

// 현재 엔진의 FNamePool에 등록된 전체 문자열 목록을 스크롤 가능한 창으로 렌더링합니다.
float FEditorViewportOverlayWidget::RenderNameTableWindow(int32 ViewportIndex, const FEditorViewportState& VS, const ImVec2& Pos)
{
	if (!VS.bShowStatNameTable) 
		return 0.f;

	ImGui::SetNextWindowPos(Pos, ImGuiCond_Always);
	ImGui::SetNextWindowBgAlpha(0.3f);
	ImGui::SetNextWindowSize(ImVec2(280.f, 300.f), ImGuiCond_Always);

	char WinId[32];
	snprintf(WinId, sizeof(WinId), "##NameTableOverlay_%d", ViewportIndex);

	if (ImGui::Begin(WinId, nullptr, kNameTableFlags))
	{
		FNamePool& Pool = FNamePool::Get();
		const uint32 Count = Pool.GetEntryCount();
		const TArray<FString>& Entries = Pool.GetEntries();

		ImGui::TextColored(ColorPurple, "FName Table (%u entries)", Count);
		ImGui::Separator();

		ImGui::BeginChild("##NTScroll", ImVec2(0.f, 0.f), false);
		ImGuiListClipper Clipper;
		Clipper.Begin(static_cast<int>(Count));
		while (Clipper.Step())
		{
			for (int j = Clipper.DisplayStart; j < Clipper.DisplayEnd; ++j)
			{
				ImGui::TextColored(ColorPaleBlue, "[%d] %s", j, Entries[static_cast<uint32>(j)].c_str());
			}
		}
		Clipper.End();
		ImGui::EndChild();
	}
	ImGui::End();

	return 280.f;
}

// 라이트 컬링(Light Culling) 패스의 디버그 통계(타일 수, 타일당 라이트 개수 등)를 출력하는 창을 그립니다.
float FEditorViewportOverlayWidget::RenderLightCullWindow(int32 ViewportIndex, const FEditorViewportState& VS, const ImVec2& Pos)
{
	if (!VS.bShowStatLightCull) 
		return 0.f;

	ImGui::SetNextWindowPos(Pos, ImGuiCond_Always);
	ImGui::SetNextWindowBgAlpha(0.3f);

	char WinId[32];
	snprintf(WinId, sizeof(WinId), "##LightCullOverlay_%d", ViewportIndex);

	if (ImGui::Begin(WinId, nullptr, kStatFlags))
	{
		const FLightCullingDebugStats& S = FLightCullingPass::GetDebugStats();
		
		ImGui::TextColored(ColorMint, "Light Culling");
		ImGui::Separator();
		
		ImGui::TextColored(ColorPaleBlue, "- Lights: %u", S.LightCount);
		ImGui::TextColored(ColorPaleBlue, "- Tiles:  %ux%u (%u)", S.TileCountX, S.TileCountY, S.TileCount);
		ImGui::TextColored(ColorPaleBlue, "- Non-zero Tiles: %u", S.NonZeroTileCount);
		ImGui::TextColored(ColorPaleBlue, "- Max / Tile: %u", S.MaxLightsInTile);
		ImGui::TextColored(ColorPaleBlue, "- Avg / Tile: %.2f", S.AvgLightsPerTile);
	}
	ImGui::End();

	return 280.0f;
}
