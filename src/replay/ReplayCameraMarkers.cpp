#include "ReplayUi.hpp"
#include "CameraMarkerGeometry.hpp"
#include "CinematicCamera.hpp"
#include <imgui.h>

namespace bedrocktools::replay {

void ReplayUi::drawCameraMarkers(ImDrawList* draw, float left, float top, float right, float bottom,
    float sourceAspect, float cropX, float cropY) {
    if (!draw || !editingReplay() || mReplayControlsHidden.load(std::memory_order_acquire)
        || ReplayExporter::get().active() || right <= left || bottom <= top) return;
    const auto scene = CinematicCamera::get().markerView();
    if (!scene.camera || !std::isfinite(sourceAspect) || sourceAspect <= 0
        || !std::isfinite(scene.camera->fov) || scene.camera->fov <= 1 || scene.camera->fov >= 179) return;
    const markers::Projection projection(*scene.camera, sourceAspect, cropX, cropY);
    const float thickness = std::clamp((bottom - top) / 360.f, 1.25f, 2.5f);
    draw->PushClipRect(ImVec2(left, top), ImVec2(right, bottom), true);
    for (std::size_t i = 0; i < scene.keys.size(); ++i) {
        const auto& key = scene.keys[i];
        if (key.dimension != scene.camera->dimension) continue;
        const auto p = key.position, eye = scene.camera->position;
        const float dx = p.x - eye.x, dy = p.y - eye.y, dz = p.z - eye.z;
        if (dx * dx + dy * dy + dz * dz < .0625f) continue;
        const ImU32 color = scene.selected == i ? IM_COL32(78, 221, 244, 255) : IM_COL32(174, 142, 250, 205);
        for (const auto& edge : markers::cameraGlyph(key)) {
            if (const auto line = projection.line(edge)) {
                const ImVec2 a(left + float(line->x1) * (right - left), top + float(line->y1) * (bottom - top));
                const ImVec2 b(left + float(line->x2) * (right - left), top + float(line->y2) * (bottom - top));
                draw->AddLine(a, b, IM_COL32(3, 7, 14, 190), thickness + 2.f);
                draw->AddLine(a, b, color, thickness);
            }
        }
    }
    draw->PopClipRect();
}

}
