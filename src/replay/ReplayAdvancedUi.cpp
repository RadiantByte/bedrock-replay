#include "ReplayUi.hpp"
#include "ReplayIcons.hpp"
#include "CinematicCamera.hpp"
#include "ReplayPlayer.hpp"

#include <imgui.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

namespace bedrocktools::replay {
namespace {
constexpr ImU32 Background = IM_COL32(7, 10, 17, 255);
constexpr ImU32 Panel = IM_COL32(15, 20, 30, 255);
constexpr ImU32 PanelRaised = IM_COL32(24, 31, 44, 255);
constexpr ImU32 Border = IM_COL32(42, 51, 67, 255);
constexpr ImU32 Text = IM_COL32(238, 243, 251, 255);
constexpr ImU32 Dim = IM_COL32(145, 162, 186, 255);
constexpr ImU32 Camera = IM_COL32(96, 157, 247, 255);
constexpr ImU32 Time = IM_COL32(249, 179, 81, 255);
constexpr ImU32 Accent = IM_COL32(159, 121, 250, 255);
constexpr ImGuiWindowFlags WindowFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize
    | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoCollapse
    | ImGuiWindowFlags_NoSavedSettings;

std::string clockText(std::uint32_t tick) {
    char out[40]{};
    std::snprintf(out, sizeof(out), "%02u:%02u.%02u", tick / 1200, (tick / 20) % 60, (tick % 20) * 5);
    return out;
}

std::string glyph(unsigned code) {
    return {static_cast<char>(0xe0 | (code >> 12)),
        static_cast<char>(0x80 | ((code >> 6) & 0x3f)),
        static_cast<char>(0x80 | (code & 0x3f))};
}
}

void ReplayUi::drawAdvancedControls() {
    loadAdvancedSensitivity();
    auto& replay = ReplayPlayer::get();
    auto& camera = CinematicCamera::get();
    const auto display = ImGui::GetIO().DisplaySize;
    const float s = std::clamp(std::min(display.x, display.y) / 720.0f, 0.72f, 1.65f);
    const bool compact = display.x < 1000.f;
    const float gap = 5 * s;
    const float margin = 6 * s;
    const float pad = 5 * s;
    const float width = display.x - 2 * margin;
    const float headerH = 48 * s;
    const float deckH = 122 * s;
    const float transportH = 48 * s;
    const float deckY = display.y - margin - deckH;
    const float sideY = margin + headerH + gap;
    const float sideH = std::max(1.f, deckY - sideY - gap);
    const float sceneH = std::max(1.f, sideH - transportH - gap);
    const float preferredSceneW = std::max(1.f, sceneH - 2 * pad) * (16.f / 9.f) + 2 * pad;
    float navW = compact ? 150.f * s : 210.f * s;
    float toolsW = compact ? 176.f * s : 230.f * s;
    float sceneW = 1.f;
    if (mAdvancedToolsOpen) {
        const float horizontal = std::max(1.f, width - 2 * gap);

        const float minSide = (compact ? 165.f : 150.f) * s;
        sceneW = std::min(preferredSceneW, std::max(1.f, horizontal - 2 * minSide));
        const float sideW = std::max(0.f, (horizontal - sceneW) * .5f);
        navW = sideW;
        toolsW = sideW;
        sceneW = std::max(1.f, horizontal - navW - toolsW);
    } else {
        navW = std::min(navW, width * .30f);
        sceneW = std::max(1.f, width - gap - navW);
        toolsW = 0.f;
    }
    const float sceneX = margin + navW + gap;
    const auto duration = replay.durationTicks();
    const auto current = replay.currentTick();
    const auto keys = camera.keyframes();
    const auto times = camera.timeKeyframes();
    const auto selectedCamera = camera.selectedIndex();
    const auto selectedTime = camera.selectedTimeKeyframeIndex();
    if (mAdvancedScrubTrack >= 0 && !ImGui::GetIO().MouseDown[0] && !ImGui::IsMouseReleased(0)) mAdvancedScrubTrack = -1;

    std::vector<ControlRect> regions;
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), display, Background);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, Panel);
    ImGui::PushStyleColor(ImGuiCol_Border, Border);
    ImGui::PushStyleColor(ImGuiCol_Text, Text);
    ImGui::PushStyleColor(ImGuiCol_TextDisabled, Dim);
    ImGui::PushStyleColor(ImGuiCol_Button, PanelRaised);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(44, 54, 73, 255));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(72, 58, 119, 255));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(20, 27, 39, 255));
    ImGui::PushStyleColor(ImGuiCol_SliderGrab, Accent);
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, Camera);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad, pad));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap, gap));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8 * s, 6 * s));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 11 * s);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 7 * s);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1 * s);
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 19 * s);

    const auto beginPanel = [&](const char* id, ImVec2 pos, ImVec2 size, bool scroll = false) {
        ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
        ImGui::SetNextWindowSize(size, ImGuiCond_Always);
        ImGui::Begin(id, nullptr, WindowFlags | (scroll ? 0 : ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse));
        ImGui::SetWindowFontScale(20.f * s / ImGui::GetIO().FontDefault->FontSize);
        regions.push_back({pos.x, pos.y, pos.x + size.x, pos.y + size.y});
    };

    const auto glyphSize = [&](const std::string& symbol, float size) {
        return ImGui::GetFont()->CalcTextSizeA(size, std::numeric_limits<float>::max(), 0.0f, symbol.c_str());
    };

    const auto drawGlyph = [&](ImDrawList* drawList, unsigned icon, ImVec2 center, ImU32 color, float size) {
        const auto symbol = glyph(icon);
        const auto measured = glyphSize(symbol, size);
        drawList->AddText(ImGui::GetFont(), size,
            ImVec2(std::round(center.x - measured.x * .5f), std::round(center.y - measured.y * .5f - 5.f * s)),
            color, symbol.c_str());
    };

    const auto iconButton = [&](unsigned icon, const char* label, const char* id, float w, float h, ImU32 tint = Text, bool active = false) {
        if (w < 0) w = ImGui::GetContentRegionAvail().x;
        if (active) ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(63, 47, 104, 255));
        const auto pos = ImGui::GetCursorScreenPos();
        const bool pressed = ImGui::Button(id, ImVec2(w, h));
        if (active) ImGui::PopStyleColor();
        auto* drawList = ImGui::GetWindowDrawList();
        const float iconFontSize = 23 * s;
        const auto symbol = glyph(icon);
        const auto iconMeasure = glyphSize(symbol, iconFontSize);
        const float iconX = label && *label ? pos.x + 12 * s : pos.x + (w - iconMeasure.x) * .5f;
        const ImVec2 iconCenter(iconX + iconMeasure.x * .5f, pos.y + h * .5f);
        drawGlyph(drawList, icon, iconCenter, tint, iconFontSize);
        if (label && *label) {
            const float tx = iconX + iconMeasure.x + 9 * s;
            const float textY = pos.y + (h - ImGui::GetFontSize()) * .5f;
            drawList->PushClipRect(ImVec2(tx, pos.y), ImVec2(pos.x + w - 8 * s, pos.y + h), true);
            drawList->AddText(ImVec2(tx, textY), Text, label);
            drawList->PopClipRect();
        }
        return pressed;
    };

    const auto flatRow = [&](unsigned icon, const char* label, const char* id, float h, ImU32 tint, bool active, bool enabled = true) {
        const float w = ImGui::GetContentRegionAvail().x;
        const auto pos = ImGui::GetCursorScreenPos();
        if (!enabled) ImGui::BeginDisabled();
        const bool pressed = ImGui::InvisibleButton(id, ImVec2(w, h));
        const bool hovered = ImGui::IsItemHovered();
        const bool held = ImGui::IsItemActive();
        if (!enabled) ImGui::EndDisabled();
        auto* drawList = ImGui::GetWindowDrawList();
        ImU32 fill = IM_COL32(20, 28, 42, 255);
        if (active) fill = IM_COL32(51, 43, 79, 235);
        else if (held) fill = IM_COL32(43, 51, 68, 235);
        else if (hovered) fill = IM_COL32(31, 39, 54, 220);
        drawList->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), fill, 8 * s);
        drawList->AddRect(pos, ImVec2(pos.x + w, pos.y + h), active ? tint : Border, 8 * s, 0, 1.f * s);
        if (active) drawList->AddRectFilled(pos, ImVec2(pos.x + 4 * s, pos.y + h), tint, 2 * s);
        const float iconFontSize = 23 * s;
        const auto symbol = glyph(icon);
        const auto iconMeasure = glyphSize(symbol, iconFontSize);
        const ImU32 iconColor = enabled ? tint : IM_COL32(93, 104, 122, 255);
        const ImU32 textColor = enabled ? Text : IM_COL32(105, 115, 132, 255);
        const float iconX = pos.x + 13 * s;
        drawGlyph(drawList, icon, ImVec2(iconX + iconMeasure.x * .5f, pos.y + h * .5f), iconColor, iconFontSize);
        const float tx = iconX + iconMeasure.x + 11 * s;
        drawList->PushClipRect(ImVec2(tx, pos.y), ImVec2(pos.x + w - 10 * s, pos.y + h), true);
        drawList->AddText(ImVec2(tx, pos.y + (h - ImGui::GetFontSize()) * .5f), textColor, label);
        drawList->PopClipRect();
        return pressed && enabled;
    };

    const auto playCard = [&](unsigned icon, const char* label, const char* id,
        float w, float h, ImU32 tint, bool active, bool enabled = true) {
        const auto pos = ImGui::GetCursorScreenPos();
        if (!enabled) ImGui::BeginDisabled();
        const bool pressed = ImGui::InvisibleButton(id, ImVec2(w, h));
        const bool hovered = ImGui::IsItemHovered();
        if (!enabled) ImGui::EndDisabled();
        auto* dl = ImGui::GetWindowDrawList();
        const ImVec2 end(pos.x + w, pos.y + h);
        dl->AddRectFilled(pos, end, active ? IM_COL32(48, 38, 83, 255)
            : hovered && enabled ? IM_COL32(32, 42, 62, 255) : IM_COL32(23, 31, 48, 255), 10 * s);
        dl->AddRect(pos, end, active ? tint : IM_COL32(56, 71, 99, 255), 10 * s, 0, 1.4f * s);
        const bool horizontal = h < 70 * s;
        drawGlyph(dl, icon, horizontal ? ImVec2(pos.x + 23 * s, pos.y + h * .5f)
            : ImVec2(pos.x + w * .5f, pos.y + h * .37f), enabled ? tint : Dim, 28 * s);
        const auto size = ImGui::CalcTextSize(label);
        const float tx = horizontal ? pos.x + 43 * s : pos.x + (w - size.x) * .5f;
        const float ty = horizontal ? pos.y + (h - size.y) * .5f : pos.y + h * .69f;
        dl->PushClipRect(ImVec2(pos.x + 3 * s, pos.y), ImVec2(end.x - 3 * s, end.y), true);
        dl->AddText(ImVec2(tx, ty), enabled ? Text : Dim, label);
        dl->PopClipRect();
        return pressed && enabled;
    };

    const auto modeBadge = [&](const char* label, ImU32 tint) {
        const auto pos = ImGui::GetCursorScreenPos();
        const float w = ImGui::GetContentRegionAvail().x;
        const float h = 37 * s;
        ImGui::Dummy(ImVec2(w, h));
        auto* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), IM_COL32(26, 35, 55, 255), 8 * s);
        dl->AddRect(pos, ImVec2(pos.x + w, pos.y + h), Border, 8 * s, 0, s);
        dl->AddCircleFilled(ImVec2(pos.x + 18 * s, pos.y + h * .5f), 5 * s, tint);
        dl->AddText(ImVec2(pos.x + 34 * s, pos.y + (h - ImGui::GetFontSize()) * .5f), tint, label);
    };

    const auto circleButton = [&](unsigned icon, const char* id, float radius, ImU32 fill, ImU32 tint, bool active) {
        const auto pos = ImGui::GetCursorScreenPos();
        const ImVec2 size(radius * 2.f, radius * 2.f);
        const bool pressed = ImGui::InvisibleButton(id, size);
        const bool hovered = ImGui::IsItemHovered();
        const bool held = ImGui::IsItemActive();
        auto* drawList = ImGui::GetWindowDrawList();
        ImU32 buttonFill = active ? fill : IM_COL32(26, 33, 46, 245);
        if (hovered && !active) buttonFill = IM_COL32(40, 48, 65, 250);
        if (held) buttonFill = fill;
        const ImVec2 center(std::round(pos.x + radius), std::round(pos.y + radius));
        drawList->AddCircleFilled(center, radius, buttonFill, 40);
        drawList->AddCircle(center, radius, active ? tint : Border, 40, active ? 2.0f * s : 1.2f * s);
        const ImU32 iconColor = active ? Text : tint;
        if (icon == 0xe13c) {
            const float halfH = radius * .42f;
            const float halfW = radius * .38f;
            const float cx = center.x + radius * .055f;
            drawList->AddTriangleFilled(ImVec2(cx - halfW * .72f, center.y - halfH),
                ImVec2(cx - halfW * .72f, center.y + halfH), ImVec2(cx + halfW, center.y), iconColor);
        } else if (icon == 0xe12e) {
            const float halfH = radius * .40f;
            const float barW = radius * .18f;
            const float gapW = radius * .12f;
            drawList->AddRectFilled(ImVec2(center.x - gapW - barW, center.y - halfH),
                ImVec2(center.x - gapW, center.y + halfH), iconColor, 1.2f * s);
            drawList->AddRectFilled(ImVec2(center.x + gapW, center.y - halfH),
                ImVec2(center.x + gapW + barW, center.y + halfH), iconColor, 1.2f * s);
        } else {
            drawGlyph(drawList, icon, center, iconColor, 24 * s);
        }
        return pressed;
    };

    const auto seek = [&](std::uint32_t tick, bool cinematic) {
        replay.pause();
        if (cinematic && !keys.empty()) camera.seekCinematic(tick);
        else {
            camera.enterReplayPreview();
            replay.seek(tick);
        }
    };

    const auto toggleReplay = [&] {
        if (replay.state() == ReplayPlayer::State::Playing
            && camera.previewMode() == CinematicCamera::PreviewMode::Replay) replay.pause();
        else {
            camera.enterReplayPreview();
            replay.resume();
        }
    };
    const auto toggleCamera = [&] {
        if (!keys.empty()) camera.toggleCinematicPreview();
    };

    const auto zoom = [&](float value) {
        mAdvancedZoom = std::clamp(value, 1.f, 16.f);
        const float span = std::max(1.f, static_cast<float>(duration) / mAdvancedZoom);
        mAdvancedViewStart = std::clamp(static_cast<float>(current) - span * .5f, 0.f,
            std::max(0.f, static_cast<float>(duration) - span));
    };

    beginPanel("##AdvancedHeader", ImVec2(margin, margin), ImVec2(width, headerH));
    const float headerButtonW = (compact ? 54 : 116) * s;
    const float titleW = std::max(1.f, ImGui::GetContentRegionAvail().x - 3 * headerButtonW - 3 * gap);
    const auto titlePos = ImGui::GetCursorScreenPos();
    auto* draw = ImGui::GetWindowDrawList();
    draw->PushClipRect(titlePos, ImVec2(titlePos.x + titleW, titlePos.y + 34 * s), true);
    const float titleX = titlePos.x;
    draw->AddText(ImVec2(titleX, titlePos.y), Text,
        compact ? "BEDROCK REPLAY" : "BEDROCK REPLAY  /  ADVANCED EDITOR");
    const auto name = replay.replayPath().stem().string();
    draw->AddText(ImVec2(titleX, titlePos.y + 17 * s), Dim, name.c_str());
    draw->PopClipRect();
    ImGui::Dummy(ImVec2(titleW, 32 * s));
    ImGui::SameLine();
    if (iconButton(0xe048, compact ? "" : "Basic", "##Basic", headerButtonW, 36 * s)) {
        mAdvancedMode = false;
        camera.releaseEditorCamera();
        mAdvancedScrubTrack = -1;
        mAdvancedInputActive.store(false, std::memory_order_release);
        std::scoped_lock lock(mMutex);
        mAdvancedMovePointer = mAdvancedLookPointer = mAdvancedLiftPointer = mAdvancedTurnPointer = -1;
        mAdvancedMoveX = mAdvancedMoveY = mAdvancedLift = mAdvancedTurn = mAdvancedLookX = mAdvancedLookY = 0;
    }
    ImGui::SameLine();
    if (iconButton(0xe0bb, compact ? "" : "Hide", "##Hide", headerButtonW, 36 * s)) {
        mReplayControlsHidden.store(true, std::memory_order_release);
        mAdvancedInputActive.store(false, std::memory_order_release);
        std::scoped_lock lock(mMutex);
        mAdvancedMovePointer = mAdvancedLookPointer = mAdvancedLiftPointer = mAdvancedTurnPointer = -1;
        mAdvancedMoveX = mAdvancedMoveY = mAdvancedLift = mAdvancedTurn = mAdvancedLookX = mAdvancedLookY = 0;
    }
    ImGui::SameLine();
    if (iconButton(0xe1b2, compact ? "" : "Close", "##Close", headerButtonW, 36 * s)) {
        camera.endSession();
        mAdvancedMode = false;
        mAdvancedInputActive.store(false, std::memory_order_release);
        mCameraPropertiesExpanded = false;
        mEditMode.store(false, std::memory_order_release);
        {
            std::scoped_lock lock(mMutex);
            mAdvancedMovePointer = mAdvancedLookPointer = mAdvancedLiftPointer = mAdvancedTurnPointer = -1;
            mAdvancedMoveX = mAdvancedMoveY = mAdvancedLift = mAdvancedTurn = mAdvancedLookX = mAdvancedLookY = 0;
        }
        replay.stop();
    }
    ImGui::End();

    beginPanel("##AdvancedNavigation", ImVec2(margin, sideY), ImVec2(navW, sideH), true);
    ImGui::TextDisabled("EDITOR");
    ImGui::Dummy(ImVec2(1, 2 * s));
    const float navButtonH = std::clamp((sideH / s - 42.f) / 6.f - 5.f, 32.f, 48.f) * s;
    if (flatRow(0xe13c, "Playback", "##NavPlay", navButtonH, Accent, mAdvancedToolTab == 0)) {
        mAdvancedToolsOpen = true;
        mAdvancedToolTab = 0;
    }
    if (flatRow(0xe106, "Timeline", "##NavTimeline", navButtonH, Camera, mAdvancedToolTab == 1)) {
        mAdvancedToolsOpen = true;
        mAdvancedToolTab = 1;
    }
    if (flatRow(0xe1a5, "Camera", "##NavCamera", navButtonH, Camera, mAdvancedToolTab == 2)) {
        mAdvancedToolsOpen = true;
        mAdvancedToolTab = 2;
    }
    if (flatRow(0xe250, "Time keys", "##NavTime", navButtonH, Time, mAdvancedToolTab == 3)) {
        mAdvancedToolsOpen = true;
        mAdvancedToolTab = 3;
    }
    if (flatRow(0xe0bb, "Players", "##NavPlayers", navButtonH, Accent, mAdvancedToolTab == 5)) {
        mAdvancedToolsOpen = true;
        mAdvancedToolTab = 5;
    }
    if (flatRow(0xe30b, "Settings", "##NavSettings", navButtonH, Dim, mAdvancedToolTab == 4)) {
        mAdvancedToolsOpen = true;
        mAdvancedToolTab = 4;
    }
    ImGui::End();

    beginPanel("##AdvancedViewport", ImVec2(sceneX, sideY), ImVec2(sceneW, sceneH));
    const ImVec2 videoAreaMin(sceneX + pad, sideY + pad);
    const ImVec2 videoAreaMax(sceneX + sceneW - pad, sideY + sceneH - pad);
    const float availW = std::max(1.f, videoAreaMax.x - videoAreaMin.x);
    const float availH = std::max(1.f, videoAreaMax.y - videoAreaMin.y);
    const float previewAspect = 16.f / 9.f;
    float frameW = std::min(availW, availH * previewAspect);
    float frameH = frameW / previewAspect;
    if (frameH > availH) {
        frameH = availH;
        frameW = frameH * previewAspect;
    }
    const ImVec2 videoMin(videoAreaMin.x + (availW - frameW) * .5f, videoAreaMin.y + (availH - frameH) * .5f);
    const ImVec2 videoMax(videoMin.x + frameW, videoMin.y + frameH);
    const float sourceAspect = mAdvancedSceneSourceHeight > 0 ? static_cast<float>(mAdvancedSceneSourceWidth) / mAdvancedSceneSourceHeight
        : display.x / std::max(display.y, 1.f);
    const float targetAspect = previewAspect;
    float cropX = 0.0f;
    float cropY = 0.0f;
    if (sourceAspect > targetAspect) cropX = (1.0f - targetAspect / sourceAspect) * .5f;
    else if (sourceAspect < targetAspect) cropY = (1.0f - sourceAspect / targetAspect) * .5f;
    auto* sceneDraw = ImGui::GetWindowDrawList();
    sceneDraw->AddRectFilled(videoAreaMin, videoAreaMax, IM_COL32(2, 5, 9, 255), 8 * s);
    if (mAdvancedSceneTexture) {
        sceneDraw->PushClipRect(videoMin, videoMax, true);
        sceneDraw->AddImage(reinterpret_cast<ImTextureID>(static_cast<std::uintptr_t>(mAdvancedSceneTexture)),
            videoMin, videoMax, ImVec2(cropX, 1.0f - cropY), ImVec2(1.0f - cropX, cropY));
        sceneDraw->PopClipRect();
        drawCameraMarkers(sceneDraw, videoMin.x, videoMin.y, videoMax.x, videoMax.y,
            sourceAspect, cropX, cropY);
    } else {
        sceneDraw->AddText(ImVec2(videoMin.x + 14 * s, videoMin.y + 14 * s), Dim, "Waiting for the game frame");
    }
    sceneDraw->AddRect(videoMin, videoMax, Border, 8 * s, 0, 1.2f * s);
    const char* previewModeLabel = "Free camera";
    if (camera.previewMode() == CinematicCamera::PreviewMode::Cinematic) previewModeLabel = "Camera path";
    else {
        switch (camera.playerPerspective()) {
        case PlayerPerspective::FirstPerson: previewModeLabel = "First person"; break;
        case PlayerPerspective::ThirdPersonBack: previewModeLabel = "Third person back"; break;
        case PlayerPerspective::ThirdPersonFront: previewModeLabel = "Third person front"; break;
        case PlayerPerspective::Free: break;
        }
    }
    const ImVec2 previewTitlePos(videoMin.x + 12 * s, videoMin.y + 10 * s);
    const auto worldSize = ImGui::CalcTextSize("WORLD PREVIEW");
    const auto modeSize = ImGui::CalcTextSize(previewModeLabel);
    const float previewTitleH = std::max(worldSize.y, modeSize.y) + 10 * s;
    const float previewTitleW = worldSize.x + modeSize.x + 20 * s;
    if (frameH >= 150 * s) {
        sceneDraw->AddRectFilled(ImVec2(previewTitlePos.x - 7 * s, previewTitlePos.y - 5 * s),
            ImVec2(previewTitlePos.x - 7 * s + previewTitleW, previewTitlePos.y - 5 * s + previewTitleH), IM_COL32(5, 9, 15, 170), 6 * s);
        sceneDraw->PushClipRect(videoMin, videoMax, true);
        sceneDraw->AddText(previewTitlePos, Text, "WORLD PREVIEW");
        sceneDraw->AddText(ImVec2(previewTitlePos.x + worldSize.x + 7 * s, previewTitlePos.y), Dim, previewModeLabel);
        sceneDraw->PopClipRect();
    }

    const float moveR = std::clamp(frameH * .14f, 30 * s, 50 * s);
    const ImVec2 moveCenter(videoMin.x + moveR + 18 * s, videoMax.y - moveR - 18 * s);
    const float liftR = std::clamp(frameH * .085f, 21 * s, 31 * s);
    const float liftX = videoMax.x - liftR - 18 * s;
    const ImVec2 downCenter(liftX, videoMax.y - liftR - 18 * s);
    const ImVec2 upCenter(liftX, downCenter.y - 2 * liftR - 10 * s);
    float moveX;
    float moveY;
    float lift;
    {
        std::scoped_lock lock(mMutex);
        moveX = mAdvancedMoveX;
        moveY = mAdvancedMoveY;
        lift = mAdvancedLift;
        mAdvancedViewportRect = {videoMin.x, videoMin.y, videoMax.x, videoMax.y};
        mAdvancedMoveRect = {moveCenter.x - moveR, moveCenter.y - moveR, moveCenter.x + moveR, moveCenter.y + moveR};
        mAdvancedLookRect = {videoMin.x, videoMin.y, videoMax.x, videoMax.y};
        mAdvancedTurnLeftRect = {};
        mAdvancedTurnRightRect = {};
        mAdvancedAscendRect = {upCenter.x - liftR, upCenter.y - liftR,
            upCenter.x + liftR, upCenter.y + liftR};
        mAdvancedDescendRect = {downCenter.x - liftR, downCenter.y - liftR,
            downCenter.x + liftR, downCenter.y + liftR};
    }

    const bool manualDisabled = camera.blocksManualInput();
    const int baseAlpha = manualDisabled ? 64 : 145;
    const int borderAlpha = manualDisabled ? 82 : 205;
    sceneDraw->AddCircleFilled(moveCenter, moveR, IM_COL32(8, 13, 21, baseAlpha), 48);
    sceneDraw->AddCircle(moveCenter, moveR, IM_COL32(236, 242, 252, borderAlpha), 48, 1.5f * s);
    sceneDraw->AddCircleFilled(ImVec2(moveCenter.x + moveX * moveR * .60f, moveCenter.y - moveY * moveR * .60f),
        moveR * .38f, manualDisabled ? IM_COL32(154, 160, 171, 92) : IM_COL32(238, 244, 255, 220), 36);
    const auto drawLiftButton = [&](ImVec2 center, bool up, bool pressed) {
        const ImU32 fill = manualDisabled ? IM_COL32(8, 13, 21, 64)
            : pressed ? IM_COL32(89, 65, 153, 218) : IM_COL32(8, 13, 21, 170);
        const ImU32 tint = manualDisabled ? IM_COL32(154, 160, 171, 92)
            : pressed ? IM_COL32(255, 255, 255, 255) : IM_COL32(238, 244, 255, 220);
        sceneDraw->AddCircleFilled(center, liftR, fill, 40);
        sceneDraw->AddCircle(center, liftR, tint, 40, 1.5f * s);

        const float direction = up ? -1.f : 1.f;
        const float tipY = center.y + direction * liftR * .39f;
        const float shoulderY = center.y - direction * liftR * .05f;
        const float stemEndY = center.y - direction * liftR * .38f;
        const float headW = liftR * .32f;
        const float stemW = liftR * .10f;
        sceneDraw->AddTriangleFilled(ImVec2(center.x, tipY),
            ImVec2(center.x - headW, shoulderY), ImVec2(center.x + headW, shoulderY), tint);
        sceneDraw->AddRectFilled(
            ImVec2(center.x - stemW, std::min(shoulderY, stemEndY)),
            ImVec2(center.x + stemW, std::max(shoulderY, stemEndY)), tint);
    };
    drawLiftButton(upCenter, true, lift > .5f);
    drawLiftButton(downCenter, false, lift < -.5f);
    ImGui::End();

    if (mAdvancedMode && !mReplayControlsHidden.load(std::memory_order_acquire)) {
        mAdvancedInputActive.store(true, std::memory_order_release);
    }

    beginPanel("##AdvancedTransport", ImVec2(sceneX, sideY + sceneH + gap), ImVec2(sceneW, transportH));
    const auto transportOrigin = ImGui::GetCursorScreenPos();
    const float transportW = ImGui::GetContentRegionAvail().x;
    const float controlY = transportOrigin.y + 1 * s;
    const std::string currentLabel = clockText(current);
    const std::string durationLabel = clockText(duration);
    const std::string timeLabel = currentLabel + " / " + durationLabel;
    const auto timeSize = ImGui::CalcTextSize(timeLabel.c_str());
    draw = ImGui::GetWindowDrawList();

    const bool narrowTransport = transportW < 540 * s;
    const float small = (narrowTransport ? 29.f : 36.f) * s;
    const float circleR = (narrowTransport ? 17.f : 20.f) * s;
    const float centerGap = (narrowTransport ? 5.f : 8.f) * s;
    const float speedW = ImGui::CalcTextSize("Speed  8.00x").x + 37 * s;
    const float groupW = small * 4 + circleR * 4 + centerGap * 5;
    const bool showTransportTime = transportW >= timeSize.x + groupW + speedW + 6 * gap;
    if (showTransportTime) draw->AddText(ImVec2(transportOrigin.x,
        controlY + (32 * s - timeSize.y) * .5f), Text, timeLabel.c_str());
    const float centerStart = transportOrigin.x + (showTransportTime ? timeSize.x + 2 * gap : 0.f);
    const float centerEnd = transportOrigin.x + transportW - speedW - 2 * gap;
    const float groupX = centerStart + std::max(0.f, (centerEnd - centerStart - groupW) * .5f);
    ImGui::SetCursorScreenPos(ImVec2(groupX, controlY));
    if (iconButton(0xe15f, "", "##SkipBack", small, small)) seek(0, false);
    ImGui::SameLine(0, centerGap);
    if (iconButton(0xe072, "", "##BackOne", small, small)) {
        seek(current > 20 ? current - 20 : 0, camera.previewMode() == CinematicCamera::PreviewMode::Cinematic);
    }
    ImGui::SameLine(0, centerGap);
    const bool replayPlaying = replay.state() == ReplayPlayer::State::Playing
        && camera.previewMode() == CinematicCamera::PreviewMode::Replay;
    if (circleButton(replayPlaying ? 0xe12e : 0xe13c, "##ReplayPlay", circleR,
        IM_COL32(94, 59, 190, 255), Accent, replayPlaying)) toggleReplay();
    ImGui::SameLine(0, centerGap);
    const bool cameraPlaying = camera.cinematicPlaying();
    ImGui::BeginDisabled(keys.empty());
    const bool cameraPressed = circleButton(cameraPlaying ? 0xe12e : 0xe13c, "##CameraPlay", circleR,
        IM_COL32(44, 91, 159, 255), Camera, cameraPlaying);
    ImGui::EndDisabled();
    if (cameraPressed) toggleCamera();
    ImGui::SameLine(0, centerGap);
    if (iconButton(0xe073, "", "##AheadOne", small, small)) {
        seek(std::min(duration, current + std::min(20u, duration - std::min(duration, current))),
            camera.previewMode() == CinematicCamera::PreviewMode::Cinematic);
    }
    ImGui::SameLine(0, centerGap);
    if (iconButton(0xe160, "", "##SkipAhead", small, small)) seek(duration, false);

    const float speed = replay.playbackSpeed();
    char speedLabel[32]{};
    std::snprintf(speedLabel, sizeof(speedLabel), "Speed  %.2fx", speed);
    ImGui::SetCursorScreenPos(ImVec2(transportOrigin.x + transportW - speedW, controlY + 1 * s));
    ImGui::SetNextItemWidth(speedW);
    if (ImGui::BeginCombo("##TransportSpeed", speedLabel, ImGuiComboFlags_HeightRegular)) {
        constexpr float speeds[] = {.25f, .5f, 1.f, 2.f, 4.f, 8.f};
        for (const float option : speeds) {
            char optionLabel[16]{};
            std::snprintf(optionLabel, sizeof(optionLabel), "%.2fx", option);
            const bool selected = std::abs(speed - option) < .001f;
            if (ImGui::Selectable(optionLabel, selected)) replay.setPlaybackSpeed(option);
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    ImGui::End();

    beginPanel("##AdvancedTimeline", ImVec2(margin, deckY), ImVec2(width, deckH));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap, 2 * s));
    const float innerW = ImGui::GetContentRegionAvail().x;
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(Accent), "TIMELINE");
    ImGui::SameLine();
    ImGui::TextDisabled("%s / %s", currentLabel.c_str(), durationLabel.c_str());
    const float zoomW = 28 * s;
    const float zoomSliderW = 95 * s;
    ImGui::SameLine(innerW - 2 * zoomW - zoomSliderW - 2 * gap);
    if (iconButton(0xe1b7, "", "##ZoomOut", zoomW, 24 * s)) zoom(mAdvancedZoom / 1.5f);
    ImGui::SameLine(0, gap);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(3 * s, 2 * s));
    ImGui::SetNextItemWidth(zoomSliderW);
    float headerZoom = mAdvancedZoom;
    if (ImGui::SliderFloat("##TimelineZoom", &headerZoom, 1.f, 16.f, "", ImGuiSliderFlags_NoInput)) zoom(headerZoom);
    ImGui::PopStyleVar();
    ImGui::SameLine(0, gap);
    if (iconButton(0xe1b6, "", "##ZoomIn", zoomW, 24 * s)) zoom(mAdvancedZoom * 1.5f);
    const float labelW = (compact ? 96.f : 140.f) * s;
    const auto ruler = ImGui::GetCursorScreenPos();
    const float lineLeft = ruler.x + labelW;
    const float lineRight = ruler.x + innerW - 8 * s;
    const float lineW = std::max(1.f, lineRight - lineLeft);
    const float span = std::max(1.f, static_cast<float>(duration) / mAdvancedZoom);
    const float maxStart = std::max(0.f, static_cast<float>(duration) - span);
    mAdvancedViewStart = std::clamp(mAdvancedViewStart, 0.f, maxStart);
    const auto tickX = [&](std::uint32_t tick) {
        return lineLeft + (static_cast<float>(tick) - mAdvancedViewStart) / span * lineW;
    };
    const auto mouseTick = [&] {
        const double amount = std::clamp(static_cast<double>((ImGui::GetIO().MousePos.x - lineLeft) / lineW), 0.0, 1.0);
        return static_cast<std::uint32_t>(std::clamp(std::round(mAdvancedViewStart + amount * span), 0.0, static_cast<double>(duration)));
    };
    draw = ImGui::GetWindowDrawList();

    ImGui::Dummy(ImVec2(innerW, ImGui::GetFontSize() + 4 * s));
    const int divisions = std::max(1, static_cast<int>(lineW / (110 * s)));
    for (int i = 0; i <= divisions; ++i) {
        const float x = lineLeft + lineW * i / divisions;
        const auto tick = static_cast<std::uint32_t>(std::min(static_cast<double>(duration),
            static_cast<double>(mAdvancedViewStart) + static_cast<double>(span) * i / divisions));
        const auto label = clockText(tick);
        const auto labelSize = ImGui::CalcTextSize(label.c_str());
        const float tx = std::clamp(x - labelSize.x * .5f, lineLeft, lineRight - labelSize.x);
        draw->AddText(ImVec2(tx, ruler.y), Dim, label.c_str());
        draw->AddLine(ImVec2(x, ruler.y + ImGui::GetFontSize() + 1 * s),
            ImVec2(x, deckY + deckH - pad), IM_COL32(73, 88, 119, 76), s);
    }

    for (int track = 0; track < 2; ++track) {
        ImGui::PushID(track);
        const auto p = ImGui::GetCursorScreenPos();
        const float h = 22 * s;
        ImGui::InvisibleButton("##Track", ImVec2(innerW, h));
        const bool active = ImGui::IsItemActive();
        const bool clicked = ImGui::IsItemClicked();
        const bool released = ImGui::IsItemDeactivated();
        const ImU32 color = track == 0 ? Camera : Time;
        draw->AddRectFilled(p, ImVec2(p.x + innerW, p.y + h), IM_COL32(19, 26, 37, 255), 6 * s);
        draw->AddRectFilled(p, ImVec2(p.x + 4 * s, p.y + h), color, 3 * s);
        for (int i = 0; i <= divisions; ++i) {
            const float x = lineLeft + lineW * i / divisions;
            draw->AddLine(ImVec2(x, p.y + 2 * s), ImVec2(x, p.y + h - 2 * s),
                IM_COL32(69, 83, 112, 60), s);
        }
        drawGlyph(draw, track == 0 ? 0xe1a5 : 0xe250, ImVec2(p.x + 19 * s, p.y + h * .5f), color, 18 * s);
        draw->AddText(ImVec2(p.x + 40 * s, p.y + (h - ImGui::GetFontSize()) * .5f), Text, track == 0 ? "Camera" : "Time");
        draw->PushClipRect(ImVec2(lineLeft - 7 * s, p.y), ImVec2(lineRight + 7 * s, p.y + h), true);
        if (track == 1 && times.size() >= 2) {
            draw->AddRectFilled(ImVec2(std::clamp(tickX(times.front()), lineLeft, lineRight), p.y + 5 * s),
                ImVec2(std::clamp(tickX(times.back()), lineLeft, lineRight), p.y + h - 5 * s),
                IM_COL32(135, 89, 27, 95), 4 * s);
        }
        draw->AddLine(ImVec2(lineLeft, p.y + h * .5f), ImVec2(lineRight, p.y + h * .5f), Border, 2 * s);
        const std::size_t count = track == 0 ? keys.size() : times.size();
        const auto selected = track == 0 ? selectedCamera : selectedTime;
        for (std::size_t i = 0; i < count; ++i) {
            const float x = tickX(track == 0 ? keys[i].tick : times[i]);
            if (x < lineLeft || x > lineRight) continue;
            const float y = p.y + h * .5f;
            const float r = (selected && *selected == i ? 8 : 6) * s;
            draw->AddQuadFilled(ImVec2(x, y - r), ImVec2(x + r, y), ImVec2(x, y + r), ImVec2(x - r, y),
                selected && *selected == i ? Text : color);
        }
        const float cursorX = tickX(mAdvancedScrubTrack >= 0 ? mAdvancedScrubTick : current);
        if (cursorX >= lineLeft && cursorX <= lineRight) {
            draw->AddLine(ImVec2(cursorX, p.y + 2 * s), ImVec2(cursorX, p.y + h - 2 * s), Accent, 2 * s);
            draw->AddCircleFilled(ImVec2(cursorX, p.y + 4 * s), 3 * s, Text);
        }
        draw->PopClipRect();
        if (clicked && ImGui::GetIO().MousePos.x >= lineLeft) mAdvancedScrubTrack = track;
        if (active && mAdvancedScrubTrack == track) mAdvancedScrubTick = mouseTick();
        if (released && mAdvancedScrubTrack == track) {
            auto target = mAdvancedScrubTick;
            std::optional<std::size_t> nearest;
            float distance = 16 * s;
            for (std::size_t i = 0; i < count; ++i) {
                const auto tick = track == 0 ? keys[i].tick : times[i];
                const float x = tickX(tick);
                if (x < lineLeft || x > lineRight) continue;
                const float d = std::abs(x - tickX(target));
                if (d <= distance) {
                    distance = d;
                    nearest = i;
                }
            }
            if (nearest) target = track == 0 ? keys[*nearest].tick : times[*nearest];
            if (track == 0) camera.select(nearest);
            else camera.selectTimeKeyframe(nearest);
            seek(target, track == 0);
            mAdvancedScrubTrack = -1;
        }
        ImGui::PopID();
    }

    const auto vacant = ImGui::GetCursorScreenPos();
    draw->AddRectFilled(vacant, ImVec2(vacant.x + innerW, vacant.y + 2 * s), IM_COL32(18, 24, 34, 255), 2 * s);
    ImGui::Dummy(ImVec2(innerW, 2 * s));
    ImGui::PopStyleVar();
    ImGui::End();

    if (mAdvancedToolsOpen) {
        const ImVec2 toolPos(display.x - margin - toolsW, sideY);
        beginPanel("##AdvancedTools", toolPos, ImVec2(toolsW, sideH), true);
        {
            std::scoped_lock lock(mMutex);
            mAdvancedToolsRect = {toolPos.x, toolPos.y, toolPos.x + toolsW, toolPos.y + sideH};
            if (mAdvancedToolScrollDelta != 0) {
                ImGui::SetScrollY(ImGui::GetScrollY() + mAdvancedToolScrollDelta);
                mAdvancedToolScrollDelta = 0;
            }
        }
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(Accent), "TOOLS");
        const float collapseW = 34 * s;
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x - collapseW);
        if (iconButton(0xe04a, "", "##HideTools", collapseW, 31 * s)) mAdvancedToolsOpen = false;
        ImGui::Separator();

        const float rowH = 44 * s;
        if (mAdvancedToolTab == 0) {
            const float cardArea = ImGui::GetContentRegionAvail().x;
            const bool stacked = cardArea < 226 * s;
            const float cardW = stacked ? cardArea : (cardArea - gap) * .5f;
            const float cardH = (stacked ? 52.f : 88.f) * s;
            const bool replayActive = replay.state() == ReplayPlayer::State::Playing
                && camera.previewMode() == CinematicCamera::PreviewMode::Replay;
            if (playCard(0xe13c, "Play Replay", "##ToolReplay", cardW, cardH, Accent, replayActive)) toggleReplay();
            if (!stacked) ImGui::SameLine(0, gap);
            const bool cameraActive = camera.cinematicPlaying();
            if (playCard(0xe1a5, "Play Camera", "##ToolCamera", cardW, cardH,
                Camera, cameraActive, !keys.empty())) toggleCamera();
            ImGui::Dummy(ImVec2(1, 3 * s));
            if (flatRow(0xe19e, "Export MP4", "##Export", rowH, Accent, false)) mOpenExportPopup = true;
            ImGui::Dummy(ImVec2(1, 10 * s));
            ImGui::Separator();
            ImGui::TextDisabled("PLAYBACK MODES");
            modeBadge("Purple Replay", Accent);
            modeBadge("Blue Camera Path", Camera);
        } else if (mAdvancedToolTab == 1) {
            ImGui::TextDisabled("TIMELINE");
            ImGui::TextDisabled("Zoom");
            ImGui::SetNextItemWidth(-1);
            float value = mAdvancedZoom;
            if (ImGui::SliderFloat("##Zoom", &value, 1.f, 16.f, "%.1fx", ImGuiSliderFlags_NoInput)) zoom(value);
            ImGui::TextDisabled("Position");
            ImGui::SetNextItemWidth(-1);
            ImGui::SliderFloat("##Pan", &mAdvancedViewStart, 0.f, maxStart, "", ImGuiSliderFlags_NoInput);
            if (flatRow(0xe145, "Fit entire replay", "##Fit", rowH, Accent, false)) {
                mAdvancedZoom = 1;
                mAdvancedViewStart = 0;
            }
        } else if (mAdvancedToolTab == 2) {
            ImGui::TextDisabled("CAMERA KEYS  %zu", keys.size());
            ImGui::Dummy(ImVec2(1, 2 * s));
            if (flatRow(0xe5e2, "Add camera key", "##AddCamera", rowH, Camera, false)) camera.captureCurrentKeyframe();
            const bool cameraSelectionValid = selectedCamera && *selectedCamera < keys.size();
            if (flatRow(0xe1a5, "Edit camera key", "##EditCamera", rowH, Camera, false, cameraSelectionValid)) {
                mCameraPropertiesExpanded = true;
                mAdvancedToolsOpen = false;
            }
            if (flatRow(0xe18e, "Delete camera key", "##DeleteCamera", rowH, Dim, false, cameraSelectionValid)) camera.deleteSelected();
            ImGui::Separator();
            for (std::size_t i = 0; i < keys.size(); ++i) {
                ImGui::PushID(static_cast<int>(i));
                const auto label = std::to_string(i + 1) + "   " + clockText(keys[i].tick);
                if (flatRow(0xe2d2, label.c_str(), "##CameraKey", 44 * s, Camera,
                    selectedCamera && *selectedCamera == i)) {
                    camera.select(i);
                    seek(keys[i].tick, true);
                }
                ImGui::PopID();
            }
        } else if (mAdvancedToolTab == 3) {
            ImGui::TextDisabled("TIME KEYS  %zu", times.size());
            ImGui::Dummy(ImVec2(1, 2 * s));
            if (flatRow(0xe13d, "Add time key", "##AddTime", rowH, Time, false)) camera.captureCurrentTimeKeyframe();
            const bool timeSelectionValid = selectedTime && *selectedTime < times.size();
            if (flatRow(0xe18e, "Delete time key", "##DeleteTime", rowH, Dim, false, timeSelectionValid)) {
                camera.deleteSelectedTimeKeyframe();
            }
            ImGui::Separator();
            for (std::size_t i = 0; i < times.size(); ++i) {
                ImGui::PushID(static_cast<int>(i));
                const auto label = std::to_string(i + 1) + "   " + clockText(times[i]);
                if (flatRow(0xe250, label.c_str(), "##TimeKey", 44 * s, Time,
                    selectedTime && *selectedTime == i)) {
                    camera.selectTimeKeyframe(i);
                    seek(times[i], false);
                }
                ImGui::PopID();
            }
        } else if (mAdvancedToolTab == 5) {
            const auto players = replay.recordedPlayers();
            const auto selected = camera.selectedPlayer();
            const auto perspective = camera.playerPerspective();
            const auto choosePerspective = [&](PlayerPerspective value) {
                auto target = selected;
                if (!target && !players.empty()) {
                    const auto own = std::find_if(players.begin(), players.end(),
                        [](const auto& player) { return player.recordingPlayer; });
                    target = own == players.end() ? players.front().id : own->id;
                }
                if (target) camera.selectPlayerView(*target, value);
            };
            ImGui::TextDisabled("PLAYER VIEW");
            if (flatRow(0xe1a5, "Free camera", "##ViewFree", rowH, Camera,
                perspective == PlayerPerspective::Free)) camera.enterFreeCamera();
            if (flatRow(0xe0bb, "First person", "##ViewFirst", rowH, Accent,
                perspective == PlayerPerspective::FirstPerson, !players.empty())) {
                choosePerspective(PlayerPerspective::FirstPerson);
            }
            ImGui::TextDisabled("THIRD PERSON");
            if (flatRow(0xe1a5, "Back view", "##ViewBack", rowH, Accent,
                perspective == PlayerPerspective::ThirdPersonBack, !players.empty())) {
                choosePerspective(PlayerPerspective::ThirdPersonBack);
            }
            if (flatRow(0xe1a5, "Front view", "##ViewFront", rowH, Accent,
                perspective == PlayerPerspective::ThirdPersonFront, !players.empty())) {
                choosePerspective(PlayerPerspective::ThirdPersonFront);
            }
            ImGui::Separator();
            if (selected && perspective != PlayerPerspective::Free) {
                const auto target = std::find_if(players.begin(), players.end(),
                    [&](const auto& player) { return player.id == *selected; });
                if (target != players.end()) {
                    ImGui::TextWrapped("Viewing: %s", target->name.c_str());
                    if (!target->available) ImGui::TextWrapped("Waiting for this player at the current replay time.");
                }
            }
            ImGui::TextWrapped("Views use recorded movement. Personal HUD and hand views are not recorded.");
            ImGui::TextDisabled("RECORDED PLAYERS  %zu", players.size());
            if (players.empty()) ImGui::TextWrapped("No recorded players are available in this replay.");
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(players.size()), rowH + ImGui::GetStyle().ItemSpacing.y);
            while (clipper.Step()) {
                for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                    const auto& player = players[static_cast<std::size_t>(i)];
                    ImGui::PushID(i);
                    const auto label = player.name + (player.recordingPlayer ? " (You)" : "");
                    if (flatRow(0xe0bb, label.c_str(), "##PlayerView", rowH,
                        player.available ? Accent : Dim, selected && *selected == player.id
                            && perspective != PlayerPerspective::Free)) {
                        camera.selectPlayerView(player.id, perspective == PlayerPerspective::Free
                            ? PlayerPerspective::FirstPerson : perspective);
                    }
                    ImGui::PopID();
                }
            }
        } else {
            ImGui::TextDisabled("SETTINGS");
            ImGui::Dummy(ImVec2(1, 2 * s));
            if (flatRow(replay.muted() ? 0xe1ac : 0xe0ba, replay.muted() ? "Sound off" : "Sound on",
                "##Sound", rowH, Accent, false)) {
                replay.setMuted(!replay.muted());
            }
            ImGui::Separator();
            ImGui::TextDisabled("CAMERA CONTROLS");
            ImGui::Text("Look sensitivity");
            float sensitivity = mAdvancedLookSensitivity.load(std::memory_order_acquire);
            ImGui::SetNextItemWidth(-1);
            if (ImGui::SliderFloat("##LookSensitivity", &sensitivity, 0.25f, 6.0f,
                "%.2fx", ImGuiSliderFlags_NoInput)) {
                mAdvancedLookSensitivity.store(sensitivity, std::memory_order_release);
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) saveAdvancedSensitivity();
            ImGui::TextDisabled("Finger drag in world preview");
        }
        ImGui::End();
    } else {
        std::scoped_lock lock(mMutex);
        mAdvancedToolsRect = {};
        mAdvancedToolPointer = -1;
    }

    ImGui::PopStyleVar(7);
    ImGui::PopStyleColor(10);
    {
        std::scoped_lock lock(mMutex);
        mAdvancedControlRects = std::move(regions);
        mControlMinX = mControlMinY = mControlMaxX = mControlMaxY = 0;
    }
}
}
