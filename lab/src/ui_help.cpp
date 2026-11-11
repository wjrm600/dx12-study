#include "ui_help.h"
#include "imgui.h"
#include <algorithm>

static ImGuiStorage g_helpOpen;   // 어떤 (?) 를 펼쳐 뒀는지 (설명 글자로 구분)

void Help(const char* text)
{
    ImGui::SameLine();
    ImGuiID id = ImGui::GetID(text);
    bool open = g_helpOpen.GetBool(id, false);
    if (open) ImGui::TextColored(ImVec4(0.45f, 0.8f, 1.0f, 1), "(접기)");
    else      ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    if (ImGui::IsItemClicked()) { open = !open; g_helpOpen.SetBool(id, open); }
    if (!open && ImGui::BeginItemTooltip())
    {
        float wrap = std::min(ImGui::GetFontSize() * 38.0f, ImGui::GetMainViewport()->Size.x * 0.7f);
        ImGui::PushTextWrapPos(wrap);
        ImGui::TextUnformatted(text);
        ImGui::TextDisabled("(클릭하면 패널 안에 펼쳐 둡니다)");
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
    if (open)
    {
        ImGui::Indent();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.78f, 0.86f, 0.95f, 1));
        ImGui::PushTextWrapPos(0.0f);   // 패널 오른쪽 끝에서 줄바꿈
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        ImGui::Unindent();
        ImGui::Spacing();
    }
}

void AboutHelp(const char* text)
{
    ImGui::TextDisabled("이 부분은 무엇을 보여 주나?");
    Help(text);
}
