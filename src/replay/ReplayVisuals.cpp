#include "ReplayVisuals.hpp"

#include "CinematicCamera.hpp"
#include "ReplayPlayer.hpp"
#include "NativePacket.hpp"
#include "ReplayExporter.hpp"
#include "ReplayUi.hpp"
#include "ReplayWorldLauncher.hpp"
#include "runtime/InputBridge.hpp"
#include "core/memory/Hooks.hpp"
#include "core/memory/Rtti.hpp"
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/offsets/Render.hpp>
#include <bedrocktools/sdk/client/ClientInstance.hpp>

namespace bedrocktools::replay {
namespace {

using ActorRenderFn = void(*)(void*, void*, bedrocktools::sdk::Actor*,
    const bedrocktools::sdk::Vec3*, const bedrocktools::sdk::Vec3*, const bedrocktools::sdk::Vec2*, bool);
ActorRenderFn actorRenderOriginal{};
bedrocktools::hooks::Handle actorRenderHook{};
thread_local std::uint64_t firstPersonActor{};

struct WorldActorRenderScope {
    std::uint64_t previous{firstPersonActor};
    bool interpolated{};
    ~WorldActorRenderScope() {
        firstPersonActor = previous;
        if (interpolated) ReplayPlayer::get().endRenderInterpolation();
    }
};

void actorRenderDetour(void* self, void* context, bedrocktools::sdk::Actor* actor,
    const bedrocktools::sdk::Vec3* origin, const bedrocktools::sdk::Vec3* offset,
    const bedrocktools::sdk::Vec2* rotation, bool flag) {
    const auto target = firstPersonActor ? firstPersonActor
        : CinematicCamera::get().firstPersonRenderTarget(origin);
    if (target && actor && ReplayPlayer::get().ownsActor(actor)
        && native::actorRuntimeId(*actor) == target) return;
    if (actorRenderOriginal) actorRenderOriginal(self, context, actor, origin, offset, rotation, flag);
}

using RenderFirstPersonFn = void(*)(void*, void*, const void*, std::uint8_t);
using RenderLevelFn = void(*)(void*, void*, void*);
using LevelRendererPlayerSetupCameraFn = void(*)(void*, void*, float);
using LevelRendererPlayerUpdateCameraFn = void(*)(void*, float);
using NativeTouchDrawFn = void(*)(void*, void*);
using GetFovFn = float(*)(void*, float, int);
using ApplyTurnDeltaFn = void(*)(void*, bedrocktools::sdk::Vec2*);
using PreRenderUpdateFn = void(*)(void*, void*, void*);
using UpdateViewAreaFn = void(*)(void*, void*);
using GetRenderCameraComponentFn = void*(*)(void*);

RenderFirstPersonFn renderFirstPersonOriginal{};
RenderLevelFn renderLevelOriginal{};
LevelRendererPlayerSetupCameraFn levelRendererPlayerSetupCameraOriginal{};
LevelRendererPlayerUpdateCameraFn levelRendererPlayerUpdateCameraOriginal{};
NativeTouchDrawFn touchGlyphDrawOriginal{};
NativeTouchDrawFn touchTapHoldDrawOriginal{};
NativeTouchDrawFn touchTextDrawOriginal{};
NativeTouchDrawFn touchMoveTurnDrawOriginal{};
GetFovFn getFovOriginal{};
ApplyTurnDeltaFn applyTurnDeltaOriginal{};
PreRenderUpdateFn preRenderUpdateOriginal{};
UpdateViewAreaFn updateViewAreaOriginal{};
GetRenderCameraComponentFn getRenderCameraComponentOriginal{};
bedrocktools::hooks::Handle renderFirstPersonHook{};
bedrocktools::hooks::Handle renderLevelHook{};
bedrocktools::hooks::Handle levelRendererPlayerSetupCameraHook{};
bedrocktools::hooks::Handle levelRendererPlayerUpdateCameraHook{};
bedrocktools::hooks::Handle touchGlyphDrawHook{};
bedrocktools::hooks::Handle touchTapHoldDrawHook{};
bedrocktools::hooks::Handle touchTextDrawHook{};
bedrocktools::hooks::Handle touchMoveTurnDrawHook{};
bedrocktools::hooks::Handle getFovHook{};
bedrocktools::hooks::Handle applyTurnDeltaHook{};
bedrocktools::hooks::Handle preRenderUpdateHook{};
bedrocktools::hooks::Handle updateViewAreaHook{};
bedrocktools::hooks::Handle getRenderCameraComponentHook{};

void* getRenderCameraComponentDetour(void* self) {
    auto* component = getRenderCameraComponentOriginal ? getRenderCameraComponentOriginal(self) : nullptr;
    auto* client = ReplayWorldLauncher::get().clientInstance();
    if (client && self == client) {

        auto* levelRenderer = client->levelRenderer();
        if (levelRenderer) {
            CinematicCamera::get().applyRendererCamera(levelRenderer->playerRenderer(), component);
        }
    }
    return component;
}

void preRenderUpdateDetour(void* self, void* screenContext, void* parameters) {
    if (preRenderUpdateOriginal) preRenderUpdateOriginal(self, screenContext, parameters);

    auto* client = ReplayWorldLauncher::get().clientInstance();
    auto* levelRenderer = client ? client->levelRenderer() : nullptr;
    if (levelRenderer && levelRenderer->playerRenderer() == self) {
        CinematicCamera::get().applyRendererCamera(self);
    }
}

void updateViewAreaDetour(void* self, void* parameters) {

    CinematicCamera::get().applyRendererCamera(self);
    if (updateViewAreaOriginal) updateViewAreaOriginal(self, parameters);
}

void renderLevelDetour(void* self, void* screenContext, void* value) {
    if (!ReplayPlayer::get().rendererActive(self, true)) {
        if (renderLevelOriginal) renderLevelOriginal(self, screenContext, value);
        return;
    }

    CinematicCamera::get().applyLevelCamera(self);
    const auto exportRenderToken = ReplayPlayer::get().exportRenderToken();
    {
        WorldActorRenderScope scope;
        scope.interpolated = ReplayPlayer::get().beginRenderInterpolation();
        firstPersonActor = CinematicCamera::get().firstPersonRenderTarget();
        if (renderLevelOriginal) renderLevelOriginal(self, screenContext, value);
    }
    ReplayPlayer::get().completeExportRender(exportRenderToken);
}

void levelRendererPlayerSetupCameraDetour(void* self, void* camera, float partialTick) {

    ReplayUi::get().applyAdvancedLookInput();
    if (levelRendererPlayerSetupCameraOriginal) levelRendererPlayerSetupCameraOriginal(self, camera, partialTick);
    CinematicCamera::get().applyCamera(self, camera);
}

void levelRendererPlayerUpdateCameraDetour(void* self, float partialTick) {
    if (levelRendererPlayerUpdateCameraOriginal) levelRendererPlayerUpdateCameraOriginal(self, partialTick);
    CinematicCamera::get().applyRendererCamera(self);
}

void renderFirstPersonDetour(void* self, void* context, const void* projection, std::uint8_t flags) {
    if (self && (ReplayUi::get().advancedViewportActive() || ReplayExporter::get().active())
        && ReplayPlayer::get().renderReady()) {
        auto* client = bedrocktools::sdk::field<bedrocktools::sdk::ClientInstance*>(
            self, bedrocktools::sdk::offsets::ItemInHandRenderer::mClientInstance);
        if (client && client == ReplayWorldLauncher::get().clientInstance()) return;
    }
    if (renderFirstPersonOriginal) renderFirstPersonOriginal(self, context, projection, flags);
}

bool suppressNativeTouchDraw() {
    if (!ReplayUi::get().advancedViewportActive() && !ReplayExporter::get().active()) return false;
    if (!ReplayPlayer::get().replayWorldActive()) return false;
    const auto menu = bedrockreplay::input::showingMenu();
    const auto pause = bedrockreplay::input::pauseMenuOpen();
    return (!menu || !*menu) && (!pause || !*pause);
}

void touchGlyphDrawDetour(void* self, void* context) {
    if (!suppressNativeTouchDraw() && touchGlyphDrawOriginal) touchGlyphDrawOriginal(self, context);
}

void touchTapHoldDrawDetour(void* self, void* context) {
    if (!suppressNativeTouchDraw() && touchTapHoldDrawOriginal) touchTapHoldDrawOriginal(self, context);
}

void touchTextDrawDetour(void* self, void* context) {
    if (!suppressNativeTouchDraw() && touchTextDrawOriginal) touchTextDrawOriginal(self, context);
}

void touchMoveTurnDrawDetour(void* self, void* context) {
    if (!suppressNativeTouchDraw() && touchMoveTurnDrawOriginal) touchMoveTurnDrawOriginal(self, context);
}

float getFovDetour(void* self, float amount, int enableVariableFov) {
    const float nativeFov = getFovOriginal ? getFovOriginal(self, amount, enableVariableFov) : 70.0f;
    return CinematicCamera::get().overrideFov(nativeFov);
}

void applyTurnDeltaDetour(void* self, bedrocktools::sdk::Vec2* delta) {
    if (CinematicCamera::get().blocksManualInput()) return;
    if (applyTurnDeltaOriginal) applyTurnDeltaOriginal(self, delta);
}

}

ReplayVisuals& ReplayVisuals::get() {
    static ReplayVisuals visuals;
    return visuals;
}

bool ReplayVisuals::install() {
    if (mInstalled) return true;
    const auto renderFirstPerson = bedrocktools::memory::resolve(
        bedrocktools::memory::SignatureId::ItemInHandRendererRenderFirstPerson);
    const auto renderLevel = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::RenderLevel);
    const auto levelRendererPlayerSetupCamera = bedrocktools::memory::resolve(
        bedrocktools::memory::SignatureId::LevelRendererPlayerSetupCamera
    );
    const auto levelRendererPlayerUpdateCamera = levelRendererPlayerSetupCamera
        + bedrocktools::sdk::offsets::LevelRendererPlayer::mUpdateCameraFromSetupCamera;
    const auto touchGlyphDraw = bedrocktools::core::rtti::virtualFunction("23TouchGlyphButtonControl", 5);
    const auto touchTapHoldDraw = bedrocktools::core::rtti::virtualFunction("32TouchTapOrHoldGlyphButtonControl", 5);
    const auto touchTextDraw = bedrocktools::core::rtti::virtualFunction("22TouchTextButtonControl", 5);
    const auto touchMoveTurnDraw = bedrocktools::core::rtti::virtualFunction("31TouchMoveAndTurnInteractControl", 5);
    const auto getFov = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::GetFov);
    const auto applyTurnDelta = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::LocalPlayerApplyTurnDelta);
    const auto preRenderUpdate = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::LevelRendererCameraPreRenderUpdate);
    const auto updateViewArea = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::LevelRendererPlayerUpdateViewArea);
    const auto getRenderCameraComponent = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ClientInstanceGetRenderCameraComponent);
    const auto actorRender = bedrocktools::memory::resolve(
        bedrocktools::memory::SignatureId::ReplayActorRenderDispatcherRender);
    if (!renderFirstPerson || !actorRender || !renderLevel || !levelRendererPlayerSetupCamera || !touchGlyphDraw || !touchTapHoldDraw || !touchTextDraw || !touchMoveTurnDraw || !getFov || !applyTurnDelta
        || !preRenderUpdate || !updateViewArea || !getRenderCameraComponent) return false;
    renderFirstPersonHook = bedrocktools::hooks::install(reinterpret_cast<void*>(renderFirstPerson),
        reinterpret_cast<void*>(renderFirstPersonDetour), reinterpret_cast<void**>(&renderFirstPersonOriginal));
    actorRenderHook = bedrocktools::hooks::install(reinterpret_cast<void*>(actorRender),
        reinterpret_cast<void*>(actorRenderDetour), reinterpret_cast<void**>(&actorRenderOriginal));
    getRenderCameraComponentHook = bedrocktools::hooks::install(reinterpret_cast<void*>(getRenderCameraComponent),
        reinterpret_cast<void*>(getRenderCameraComponentDetour), reinterpret_cast<void**>(&getRenderCameraComponentOriginal));
    preRenderUpdateHook = bedrocktools::hooks::install(reinterpret_cast<void*>(preRenderUpdate),
        reinterpret_cast<void*>(preRenderUpdateDetour), reinterpret_cast<void**>(&preRenderUpdateOriginal));
    updateViewAreaHook = bedrocktools::hooks::install(reinterpret_cast<void*>(updateViewArea),
        reinterpret_cast<void*>(updateViewAreaDetour), reinterpret_cast<void**>(&updateViewAreaOriginal));
    renderLevelHook = bedrocktools::hooks::install(
        reinterpret_cast<void*>(renderLevel),
        reinterpret_cast<void*>(renderLevelDetour),
        reinterpret_cast<void**>(&renderLevelOriginal)
    );
    levelRendererPlayerSetupCameraHook = bedrocktools::hooks::install(
        reinterpret_cast<void*>(levelRendererPlayerSetupCamera),
        reinterpret_cast<void*>(levelRendererPlayerSetupCameraDetour),
        reinterpret_cast<void**>(&levelRendererPlayerSetupCameraOriginal)
    );
    levelRendererPlayerUpdateCameraHook = bedrocktools::hooks::install(
        reinterpret_cast<void*>(levelRendererPlayerUpdateCamera),
        reinterpret_cast<void*>(levelRendererPlayerUpdateCameraDetour),
        reinterpret_cast<void**>(&levelRendererPlayerUpdateCameraOriginal)
    );
    touchGlyphDrawHook = bedrocktools::hooks::install(
        touchGlyphDraw, reinterpret_cast<void*>(touchGlyphDrawDetour),
        reinterpret_cast<void**>(&touchGlyphDrawOriginal));
    touchTapHoldDrawHook = bedrocktools::hooks::install(
        touchTapHoldDraw, reinterpret_cast<void*>(touchTapHoldDrawDetour),
        reinterpret_cast<void**>(&touchTapHoldDrawOriginal));
    touchTextDrawHook = bedrocktools::hooks::install(
        touchTextDraw, reinterpret_cast<void*>(touchTextDrawDetour),
        reinterpret_cast<void**>(&touchTextDrawOriginal));
    touchMoveTurnDrawHook = bedrocktools::hooks::install(
        touchMoveTurnDraw, reinterpret_cast<void*>(touchMoveTurnDrawDetour),
        reinterpret_cast<void**>(&touchMoveTurnDrawOriginal));
    getFovHook = bedrocktools::hooks::install(
        reinterpret_cast<void*>(getFov),
        reinterpret_cast<void*>(getFovDetour),
        reinterpret_cast<void**>(&getFovOriginal)
    );
    applyTurnDeltaHook = bedrocktools::hooks::install(
        reinterpret_cast<void*>(applyTurnDelta),
        reinterpret_cast<void*>(applyTurnDeltaDetour),
        reinterpret_cast<void**>(&applyTurnDeltaOriginal)
    );
    mInstalled = renderFirstPersonHook != nullptr && actorRenderHook != nullptr && renderLevelHook != nullptr && levelRendererPlayerSetupCameraHook != nullptr
        && levelRendererPlayerUpdateCameraHook != nullptr && touchGlyphDrawHook != nullptr
        && touchTapHoldDrawHook != nullptr && touchTextDrawHook != nullptr && touchMoveTurnDrawHook != nullptr
        && getFovHook != nullptr
        && applyTurnDeltaHook != nullptr && preRenderUpdateHook != nullptr && updateViewAreaHook != nullptr
        && getRenderCameraComponentHook != nullptr;
    if (!mInstalled) uninstall();
    return mInstalled;
}

void ReplayVisuals::uninstall() {
    if (renderFirstPersonHook) bedrocktools::hooks::remove(renderFirstPersonHook);
    renderFirstPersonHook = nullptr;
    renderFirstPersonOriginal = nullptr;
    if (actorRenderHook) bedrocktools::hooks::remove(actorRenderHook);
    actorRenderHook = nullptr;
    actorRenderOriginal = nullptr;
    firstPersonActor = 0;
    if (getRenderCameraComponentHook) bedrocktools::hooks::remove(getRenderCameraComponentHook);
    getRenderCameraComponentHook = nullptr;
    getRenderCameraComponentOriginal = nullptr;
    if (preRenderUpdateHook) bedrocktools::hooks::remove(preRenderUpdateHook);
    if (updateViewAreaHook) bedrocktools::hooks::remove(updateViewAreaHook);
    preRenderUpdateHook = nullptr;
    updateViewAreaHook = nullptr;
    preRenderUpdateOriginal = nullptr;
    updateViewAreaOriginal = nullptr;
    if (applyTurnDeltaHook) bedrocktools::hooks::remove(applyTurnDeltaHook);
    if (getFovHook) bedrocktools::hooks::remove(getFovHook);
    if (touchMoveTurnDrawHook) bedrocktools::hooks::remove(touchMoveTurnDrawHook);
    if (touchTextDrawHook) bedrocktools::hooks::remove(touchTextDrawHook);
    if (touchTapHoldDrawHook) bedrocktools::hooks::remove(touchTapHoldDrawHook);
    if (touchGlyphDrawHook) bedrocktools::hooks::remove(touchGlyphDrawHook);
    if (levelRendererPlayerUpdateCameraHook) bedrocktools::hooks::remove(levelRendererPlayerUpdateCameraHook);
    if (levelRendererPlayerSetupCameraHook) bedrocktools::hooks::remove(levelRendererPlayerSetupCameraHook);
    if (renderLevelHook) bedrocktools::hooks::remove(renderLevelHook);
    applyTurnDeltaHook = nullptr;
    getFovHook = nullptr;
    touchGlyphDrawHook = nullptr;
    touchTapHoldDrawHook = nullptr;
    touchTextDrawHook = nullptr;
    touchMoveTurnDrawHook = nullptr;
    levelRendererPlayerUpdateCameraHook = nullptr;
    levelRendererPlayerSetupCameraHook = nullptr;
    renderLevelHook = nullptr;
    applyTurnDeltaOriginal = nullptr;
    getFovOriginal = nullptr;
    touchGlyphDrawOriginal = nullptr;
    touchTapHoldDrawOriginal = nullptr;
    touchTextDrawOriginal = nullptr;
    touchMoveTurnDrawOriginal = nullptr;
    levelRendererPlayerUpdateCameraOriginal = nullptr;
    levelRendererPlayerSetupCameraOriginal = nullptr;
    renderLevelOriginal = nullptr;
    mInstalled = false;
}

}
