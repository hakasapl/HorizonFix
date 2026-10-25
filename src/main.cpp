#include "ConfigLoader.hpp"

#include "HorizonBand.hpp"
#include "SkirtCull.hpp"
#include "SkirtDepth.hpp"
#include "WaterSkirt.hpp"

#include "PCH.h"

#include <spdlog/common.h>
#include <spdlog/logger.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/spdlog.h>

#include <filesystem>
#include <memory>
#include <string>
#include <utility>

using namespace HorizonFix;

/**
 * @brief How far the far water reaches right now, for renderers that fog the sky
 *
 * Community Shaders / Open Shaders resolve this export by name (GetProcAddress on
 * HorizonFix.dll) and call it every frame from their render thread. A height fog that
 * fogs the sky at the far clip plane leaves a seam against water that continues to its
 * own horizon far beyond it, so they fog the sky out to this distance instead. Returns 0
 * whenever there is no far water (blocklisted worldspace, interior, map menu), which the
 * caller treats as "fog the sky at the far plane as usual". The name and signature are
 * the contract; keep both stable.
 *
 * @return float Distance in game units, or 0 when there is no far water
 */
extern "C" __declspec(dllexport) auto HorizonFix_GetFarWaterDistance() -> float
{
    return WaterSkirt::farWaterDistance();
}

/**
 * @brief Shows or hides the horizon blend band at runtime
 *
 * For renderers that blend the horizon themselves and want HorizonFix's band mesh out of
 * the way. Callable from any thread; takes effect within a frame. Independent of the blend
 * angle: while the angle is 0 (fHorizonBlendDegrees, or HorizonFix_SetHorizonBlendDegrees)
 * nothing is drawn either way, and the return value tells the caller whether an angle is set
 * (only meaningful once HorizonFix has loaded). Nothing is persisted: every session starts
 * with the band shown. The name and signature are the contract; keep both stable.
 *
 * @param enabled True to show the band, false to hide it
 * @return bool True when the blend angle in effect is above 0
 */
extern "C" __declspec(dllexport) auto HorizonFix_SetHorizonBandEnabled(bool enabled) -> bool
{
    return HorizonBand::setEnabled(enabled);
}

/**
 * @brief Sets the water skirt radius at runtime, overriding fWaterSkirtRadius for the session
 *
 * Callable from any thread; the skirt is rebuilt at the new size on the main thread within
 * a frame, and HorizonFix_GetFarWaterDistance follows once it has. Each change re-clones
 * every tile, so prefer calling it when a value is committed over every frame of a slider
 * drag (calls within one frame coalesce, and an unchanged value costs nothing). Values
 * below the skirt's minimum of four LOD32 blocks (524288 units) are raised to it, as in the
 * INI; NaN and infinities are ignored. The name and signature are the contract; keep both
 * stable.
 *
 * @param radius Radius in game units
 */
extern "C" __declspec(dllexport) void HorizonFix_SetWaterSkirtRadius(float radius) { WaterSkirt::setRadius(radius); }

/**
 * @brief Sets the skirt rim quality at runtime, overriding iWaterSkirtRimQuality for the session
 *
 * Same threading and rebuild behavior as HorizonFix_SetWaterSkirtRadius. Clamped to 0-6
 * like the INI value. The name and signature are the contract; keep both stable.
 *
 * @param quality How many times tiles crossing the rim may be quad-split (0-6)
 */
extern "C" __declspec(dllexport) void HorizonFix_SetWaterSkirtRimQuality(int quality)
{
    WaterSkirt::setRimQuality(quality);
}

/**
 * @brief Drops a HorizonFix_SetHorizonBandEnabled override: the band shows again wherever a blend
 * angle is set
 *
 * For renderers whose own setting stops applying (a scene without their override, say), so the
 * plugin is back on its INI behavior. Same threading as HorizonFix_SetHorizonBandEnabled. The name
 * and signature are the contract; keep both stable.
 *
 * @return bool True when the blend angle in effect is above 0
 */
extern "C" __declspec(dllexport) auto HorizonFix_ResetHorizonBand() -> bool { return HorizonBand::setEnabled(true); }

/**
 * @brief Drops a HorizonFix_SetWaterSkirtRadius override: the skirt goes back to fWaterSkirtRadius
 *
 * Same threading and rebuild behavior as HorizonFix_SetWaterSkirtRadius; nothing is rebuilt
 * when the INI radius already is the one in effect. The name and signature are the contract;
 * keep both stable.
 */
extern "C" __declspec(dllexport) void HorizonFix_ResetWaterSkirtRadius() { WaterSkirt::resetRadius(); }

/**
 * @brief Drops a HorizonFix_SetWaterSkirtRimQuality override: the skirt goes back to iWaterSkirtRimQuality
 *
 * Same threading and rebuild behavior as HorizonFix_SetWaterSkirtRadius. The name and signature
 * are the contract; keep both stable.
 */
extern "C" __declspec(dllexport) void HorizonFix_ResetWaterSkirtRimQuality() { WaterSkirt::resetRimQuality(); }

/**
 * @brief Sets the horizon blend angle at runtime, overriding fHorizonBlendDegrees for the session
 *
 * Callable from any thread; applied on the main thread within a frame by rebuilding the band's
 * geometry (small buffers, cheap enough to follow a slider). 0 hides the band, and an angle
 * above 0 builds it even when the INI set none. Clamped to 0-45 like the INI value; NaN and
 * infinities are ignored. HorizonFix_SetHorizonBandEnabled hides the band independently of
 * the angle. The name and signature are the contract; keep both stable.
 *
 * @param degrees Angular height of the blend above and below the horizon line, in degrees
 */
extern "C" __declspec(dllexport) void HorizonFix_SetHorizonBlendDegrees(float degrees)
{
    HorizonBand::setDegrees(degrees);
}

/**
 * @brief Drops a HorizonFix_SetHorizonBlendDegrees override: the band goes back to fHorizonBlendDegrees
 *
 * Same threading as HorizonFix_SetHorizonBlendDegrees. The name and signature are the contract;
 * keep both stable.
 */
extern "C" __declspec(dllexport) void HorizonFix_ResetHorizonBlendDegrees() { HorizonBand::resetDegrees(); }

/**
 * @brief The horizon blend angle setting, for renderers that offer it in their own UI
 *
 * fHorizonBlendDegrees, or the last HorizonFix_SetHorizonBlendDegrees value once the main
 * thread has applied it (within a frame). Callable from any thread. The name and signature are
 * the contract; keep both stable.
 *
 * @return float Blend angle in degrees (0-45)
 */
extern "C" __declspec(dllexport) auto HorizonFix_GetHorizonBlendDegrees() -> float
{
    return ConfigLoader::getHorizonBlendDegrees();
}

/**
 * @brief The water skirt radius setting, for renderers that offer it in their own UI
 *
 * fWaterSkirtRadius, or the last HorizonFix_SetWaterSkirtRadius value once the main thread
 * has applied it (within a frame), raised to the skirt's minimum of four LOD32 blocks. This
 * is the setting, not what is drawn right now: HorizonFix_GetFarWaterDistance is the live
 * reach (0 wherever there is no far water). Callable from any thread. The name and
 * signature are the contract; keep both stable.
 *
 * @return float Radius in game units
 */
extern "C" __declspec(dllexport) auto HorizonFix_GetWaterSkirtRadius() -> float
{
    return WaterSkirt::effectiveRadius();
}

/**
 * @brief The skirt rim quality setting, for renderers that offer it in their own UI
 *
 * iWaterSkirtRimQuality, or the last HorizonFix_SetWaterSkirtRimQuality value once the main
 * thread has applied it (within a frame). Callable from any thread. The name and signature
 * are the contract; keep both stable.
 *
 * @return int Rim quality (0-6)
 */
extern "C" __declspec(dllexport) auto HorizonFix_GetWaterSkirtRimQuality() -> int
{
    return ConfigLoader::getRimQuality();
}

namespace {

/**
 * @brief Sets up the global log file for the plugin using spdlog
 */
void setupLog()
{
    // Resolve the SKSE log directory (Documents/My Games/.../SKSE)
    auto logsFolder = SKSE::log::log_directory();
    if (!logsFolder) {
        SKSE::stl::report_and_fail("SKSE log_directory not provided, logs disabled.");
    }

    // Create a truncating file sink named after the plugin and make it the default logger
    auto logFilePath = *logsFolder / (std::string(PLUGIN_NAME) + ".log");
    auto fileLogger = std::make_shared<spdlog::sinks::basic_file_sink_mt>(logFilePath.string(), true);
    auto logger = std::make_shared<spdlog::logger>("log", std::move(fileLogger));

    // Log everything and flush per message so crashes don't lose the tail of the log
    spdlog::set_default_logger(std::move(logger));
    spdlog::set_level(spdlog::level::trace);
    spdlog::flush_on(spdlog::level::trace);
}

/**
 * @brief Installs any hooks required for HorizonFix to operate
 */
void installHooks()
{
    SkirtCull::AtmosphereUpdateHook::install();
    SkirtDepth::install();

    // The horizon blend band's tint-refresh hooks go in even when fHorizonBlendDegrees is 0:
    // a renderer can set an angle at runtime (HorizonFix_SetHorizonBlendDegrees), and the
    // vtable cannot be patched safely once rendering has started
    HorizonBand::installHooks();
}

/**
 * @brief MessageHandler for HorizonFix
 *
 * @param msg The received message
 */
void messageHandler(SKSE::MessagingInterface::Message* msg)
{
    switch (msg->type) {
    case SKSE::MessagingInterface::kDataLoaded:
        // All forms are loaded; the event singletons now exist, so register the sinks that
        // drive skirt rebuilds (cell attach) and map-menu hiding
        if (auto* const holder = RE::ScriptEventSourceHolder::GetSingleton()) {
            holder->AddEventSink(CellAttachSink::getSingleton());
            spdlog::info("Water skirt: registered cell attach listener");
        }
        if (auto* const ui = RE::UI::GetSingleton()) {
            ui->AddEventSink(MapMenuSink::getSingleton());
            spdlog::info("Water skirt: registered map menu listener");
        }
        break;
    case SKSE::MessagingInterface::kNewGame:
        // A new game may start in an exterior without firing a cell attach we saw; build eagerly
        WaterSkirt::queueUpdate();
        break;
    default:
        break;
    }
}

} // namespace

//
// CommonLibSSE-NG / SKSE Exports
//

// In the exported SKSEPlugin_Version blob the StructCompatibility field occupies the
// versionIndependenceEx dword (PluginDeclarationInfo 0x300 lands on PluginVersionData 0x304),
// so besides Independent (= kVersionIndependentEx_NoStructUse) it must also carry
// kVersionIndependentEx_AddressLibraryV5: SKSE on 1.7.99+ refuses address-library plugins
// that don't declare the v5 database format (CommonLibSSE-NG 6.7.0 reads v1/v2/v5 alike),
// while every earlier SKSE only tests the bits it knows and ignores this one.
// CommonLibSSE-NG 6.7.0 sets the flag only in the PluginVersionData default, which the
// SKSEPluginInfo/PluginDeclaration path doesn't use - hence this OR until it grows a field.
SKSEPluginInfo(.Version = REL::Version {0,
                                        1,
                                        0,
                                        0},
               .Name = "HorizonFix",
               .Author = "hakasapl",
               .StructCompatibility = static_cast<SKSE::StructCompatibility>(
                   std::to_underlying(SKSE::StructCompatibility::Independent)
                   | SKSE::PluginVersionData::kVersionIndependentEx_AddressLibraryV5),
               .RuntimeCompatibility = SKSE::VersionIndependence::AddressLibrary)

    SKSEPluginLoad(const SKSE::LoadInterface* skse)
{
    SKSE::Init(skse);
    setupLog();

    const auto version = REL::Module::get().version();
    spdlog::info("{} {} loading (runtime {})", PLUGIN_NAME, PLUGIN_VERSION, version.string("."));

    // Read the INI once, then patch the engine vtables while nothing is rendering yet
    ConfigLoader::loadConfig();
    installHooks();

    // Register messaging interface
    const auto* messaging = SKSE::GetMessagingInterface();
    if (!messaging->RegisterListener("SKSE", messageHandler)) {
        return false;
    }

    spdlog::info("{} loaded", PLUGIN_NAME);
    return true;
}
