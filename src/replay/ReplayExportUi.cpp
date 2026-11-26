#include "ReplayUi.hpp"
#include "CinematicCamera.hpp"

#include <imgui.h>
#include <algorithm>
#include <cstdio>

namespace bedrocktools::replay {
namespace {
constexpr ImU32 Accent = IM_COL32(159, 121, 250, 255);
constexpr ImU32 Blue = IM_COL32(96, 157, 247, 255);
constexpr ImU32 Text = IM_COL32(238, 243, 251, 255);
constexpr ImU32 Dim = IM_COL32(145, 162, 186, 255);

std::string exportClock(std::uint32_t tick) {
    char text[32]{};
    std::snprintf(text, sizeof(text), "%02u:%02u.%02u", tick / 1200, tick / 20 % 60, tick % 20 * 5);
    return text;
}

void selectedColors() {
    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(48, 41, 72, 255));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(64, 52, 95, 255));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(80, 61, 126, 255));
    ImGui::PushStyleColor(ImGuiCol_Border, Accent);
}
}

void ReplayUi::drawExportPopup() {
    if (mOpenExportPopup) {
        ImGui::OpenPopup("Save replay as MP4");
        mOpenExportPopup = false;
    }
    const auto display = ImGui::GetIO().DisplaySize;
    const float s = std::clamp(display.y / 720.f, .78f, 1.45f);
    const float width = std::max(1.f, std::min(820.f * s, display.x - 32.f * s));
    const float height = std::max(1.f, std::min(410.f * s, display.y - 32.f * s));
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(.5f, .5f));
    ImGui::SetNextWindowSize(ImVec2(width, height), ImGuiCond_Always);
    ImGui::PushStyleColor(ImGuiCol_PopupBg, IM_COL32(10, 15, 25, 255));
    ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(53, 65, 86, 255));
    ImGui::PushStyleColor(ImGuiCol_Text, Text);
    ImGui::PushStyleColor(ImGuiCol_TextDisabled, Dim);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(17, 24, 36, 255));
    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(25, 34, 49, 255));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(40, 51, 72, 255));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(57, 51, 85, 255));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(26, 35, 52, 255));
    ImGui::PushStyleColor(ImGuiCol_SliderGrab, Accent);
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, Blue);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 16.f * s);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20.f * s, 18.f * s));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10.f * s, 8.f * s));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.f * s, 8.f * s));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.f * s);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 11.f * s);
    const bool visible = ImGui::BeginPopupModal("Save replay as MP4", nullptr,
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize
        | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar);
    mModalVisible.store(visible, std::memory_order_release);
    if (visible) {
        auto* draw = ImGui::GetWindowDrawList();
        const auto pos = ImGui::GetWindowPos();
        draw->AddRectFilledMultiColor(ImVec2(pos.x + 18.f * s, pos.y),
            ImVec2(pos.x + width - 18.f * s, pos.y + 3.f * s), Blue, Accent, Accent, Blue);
        const auto header = ImGui::GetCursorScreenPos();
        const float iconSize = 42.f * s;
        draw->AddCircleFilled(ImVec2(header.x + iconSize * .5f, header.y + iconSize * .5f),
            iconSize * .5f, IM_COL32(30, 36, 56, 255));
        if (const auto logo = logoTexture()) {
            draw->AddImage(reinterpret_cast<ImTextureID>(static_cast<std::uintptr_t>(logo)),
                ImVec2(header.x + 8.f * s, header.y + 8.f * s),
                ImVec2(header.x + iconSize - 8.f * s, header.y + iconSize - 8.f * s));
        }
        const float titleX = header.x + iconSize + 13.f * s;
        draw->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 1.2f,
            ImVec2(titleX, header.y + 2.f * s), Text, "EXPORT REPLAY");
        draw->AddText(ImVec2(titleX, header.y + 25.f * s), Dim, "Save your replay as MP4");
        ImGui::Dummy(ImVec2(1, 48.f * s));

        const auto times = CinematicCamera::get().timeKeyframes();
        const bool validRange = times.size() >= 2 && times.front() < times.back();
        ImGui::PushStyleColor(ImGuiCol_Text, validRange ? IM_COL32(179, 203, 233, 255) : IM_COL32(242, 147, 164, 255));
        if (validRange) ImGui::Text("TIME RANGE   %s  -  %s", exportClock(times.front()).c_str(), exportClock(times.back()).c_str());
        else ImGui::TextWrapped("Add two distinct time keyframes to set the export range.");
        ImGui::PopStyleColor();
        ImGui::Spacing();

        const float footerHeight = 46.f * s;
        const float bodyHeight = std::max(1.f, ImGui::GetContentRegionAvail().y - footerHeight);
        ImGui::BeginChild("##ExportSettingsBody", ImVec2(0, bodyHeight), false);
        const float contentWidth = ImGui::GetContentRegionAvail().x;
        const bool columns = contentWidth >= 510.f * s;
        const float gap = 12.f * s;
        const float leftWidth = columns ? (contentWidth - gap) * .53f : contentWidth;
        const float cardHeight = 208.f * s;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.f * s, 12.f * s));
        ImGui::BeginChild("##ExportResolution", ImVec2(leftWidth, cardHeight), true, ImGuiWindowFlags_NoScrollbar);
        ImGui::TextDisabled("RESOLUTION");
        struct Preset { const char* label; int width, height; };
        const Preset presets[] = {{"1080p    16:9", 1920, 1080}, {"1080p    9:16", 1080, 1920}, {"720p      16:9", 1280, 720}};
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.f);
        for (const auto& preset : presets) {
            const bool selected = mExportSettings.width == preset.width && mExportSettings.height == preset.height;
            if (selected) selectedColors();
            if (ImGui::Button(preset.label, ImVec2(-1, 42.f * s))) {
                mExportSettings.width = preset.width;
                mExportSettings.height = preset.height;
            }
            if (selected) ImGui::PopStyleColor(4);
        }
        ImGui::PopStyleVar();
        ImGui::EndChild();
        if (columns) ImGui::SameLine(0, gap);
        ImGui::BeginChild("##ExportQuality", ImVec2(columns ? contentWidth - leftWidth - gap : contentWidth, cardHeight),
            true, ImGuiWindowFlags_NoScrollbar);
        ImGui::TextDisabled("FRAME RATE");
        const float fpsWidth = (ImGui::GetContentRegionAvail().x - 2.f * ImGui::GetStyle().ItemSpacing.x) / 3.f;
        const int rates[] = {24, 30, 60};
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.f);
        for (int i = 0; i < 3; ++i) {
            if (i) ImGui::SameLine();
            char label[20]{};
            std::snprintf(label, sizeof(label), "%d", rates[i]);
            const bool selected = mExportSettings.frameRate == rates[i];
            if (selected) selectedColors();
            if (ImGui::Button(label, ImVec2(fpsWidth, 36.f * s))) mExportSettings.frameRate = rates[i];
            if (selected) ImGui::PopStyleColor(4);
        }
        ImGui::PopStyleVar();
        ImGui::Spacing();
        int bitrate = std::clamp(mExportSettings.bitrate / 1000000, 1, 50);
        ImGui::TextDisabled("BITRATE");
        ImGui::SetNextItemWidth(-1);
        if (ImGui::SliderInt("##ExportBitrate", &bitrate, 1, 50, "%d Mbps")) mExportSettings.bitrate = bitrate * 1000000;
        ImGui::Spacing();
        ImGui::TextDisabled("OUTPUT  %d x %d", mExportSettings.width, mExportSettings.height);
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::EndChild();

        const float actionWidth = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * .5f;
        if (ImGui::Button("Cancel", ImVec2(actionWidth, 38.f * s))) ImGui::CloseCurrentPopup();
        ImGui::SameLine();
        if (!validRange) ImGui::BeginDisabled();
        ImGui::PushStyleColor(ImGuiCol_Button, Accent);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(180, 151, 255, 255));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(126, 91, 216, 255));
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(12, 11, 27, 255));
        if (ImGui::Button("Start export", ImVec2(actionWidth, 38.f * s))) {
            if (ReplayExporter::get().beginCurrent(mExportSettings)) ImGui::CloseCurrentPopup();
        }
        ImGui::PopStyleColor(4);
        if (!validRange) ImGui::EndDisabled();
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar(6);
    ImGui::PopStyleColor(11);
}

}
