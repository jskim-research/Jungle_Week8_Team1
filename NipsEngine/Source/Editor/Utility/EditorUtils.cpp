#include "Editor/Utility/EditorUtils.h"
#include "ImGui/imgui.h"
#include "Engine/Component/Movement/MovementComponent.h"
#include "Engine/Component/SceneComponent.h"
#include "Object/FName.h"
#include <cstdio>
#include <cstring>

namespace EditorUtils
{
    bool DrawXButton(const char* id, float size)
    {
        ImGui::PushID(id);

        ImVec2 pos = ImGui::GetCursorScreenPos();
        bool bClicked = ImGui::InvisibleButton("##xbtn", ImVec2(size, size));

        ImVec4 col = ImVec4(0.6f, 0.6f, 0.6f, 1.0f);
        if      (ImGui::IsItemActive())  col = ImVec4(0.9f, 0.1f, 0.1f, 1.0f);
        else if (ImGui::IsItemHovered()) col = ImVec4(0.8f, 0.2f, 0.2f, 0.8f);

        ImDrawList* dl = ImGui::GetWindowDrawList();

        // 호버/클릭 시 배경 원
        ImVec2 center(pos.x + size * 0.5f + 0.5f, pos.y + size * 0.5f + 0.5f);
        dl->AddCircleFilled(center, size * 0.5f, ImGui::ColorConvertFloat4ToU32(
            ImGui::IsItemActive()
                ? ImVec4(0.9f, 0.1f, 0.1f, 1.0f)
                : ImVec4(0.8f, 0.2f, 0.2f, 0.8f)));

        // X 직접 그리기 (폰트 무관)
        float pad = size * 0.3f;
        ImU32 color = ImGui::ColorConvertFloat4ToU32(col);
        dl->AddLine(
            ImVec2(pos.x + pad,        pos.y + pad),
            ImVec2(pos.x + size - pad, pos.y + size - pad),
            color, 2.0f);
        dl->AddLine(
            ImVec2(pos.x + size - pad, pos.y + pad),
            ImVec2(pos.x + pad,        pos.y + size - pad),
            color, 2.0f);

        ImGui::PopID();
        return bClicked;
    }

    void MakeXButtonId(char* OutBuf, size_t BufSize, const void* Ptr)
    {
        snprintf(OutBuf, BufSize, "xbtn_%p", Ptr);
    }

    bool RenderStringComboOrInput(const char* Label, FString& Value, const TArray<FString>& Options)
    {
        if (!Options.empty())
        {
            bool bChanged = false;
            if (ImGui::BeginCombo(Label, Value.empty() ? "<None>" : Value.c_str()))
            {
                for (const FString& Path : Options)
                {
                    bool bSelected = (Value == Path);
                    if (ImGui::Selectable(Path.c_str(), bSelected))
                    {
                        Value = Path;
                        bChanged = true;
                    }
                    if (bSelected)
                        ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            return bChanged;
        }
        else
        {
            char Buf[256];
            strncpy_s(Buf, sizeof(Buf), Value.c_str(), _TRUNCATE);
            if (ImGui::InputText(Label, Buf, sizeof(Buf)))
            {
                Value = Buf;
                return true;
            }
            return false;
        }
    }

    FString GetMovementComponentDisplayName(UMovementComponent* MoveComp)
    {
        if (!MoveComp) return "None";

        USceneComponent* UpdatedComp = MoveComp->GetUpdatedComponent();
        if (UpdatedComp)
        {
            FString TargetName = UpdatedComp->GetFName().ToString();
            if (TargetName.empty())
            {
                TargetName = UpdatedComp->GetTypeInfo()->name;
            }
            return FString("MC_") + TargetName;
        }

        // 대상이 없는 경우
        FString DefaultName = MoveComp->GetFName().ToString();
        if (DefaultName.empty())
        {
            DefaultName = MoveComp->GetTypeInfo()->name;
        }
        return DefaultName;
    }
}
