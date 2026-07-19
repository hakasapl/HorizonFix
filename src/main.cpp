#include "ConfigLoader.hpp"

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
}

/**
 * @brief Checks that the running game's flavor (AE vs SE) matches what this DLL was built for
 *
 * The plugin ships as separate AE and SE builds; loading the wrong one would use mismatched
 * struct layouts and addresses.
 *
 * @return true If the runtime flavor matches the build flavor
 * @return false If it does not (the plugin should refuse to load)
 */
auto runtimeMatchesBuild() -> bool
{
#ifdef SKYRIM_SUPPORT_AE
    constexpr bool BUILT_FOR_AE = true;
#else
    constexpr bool BUILT_FOR_AE = false;
#endif
    // AE is every 1.6.x runtime; SE is 1.5.x and below
    const auto version = REL::Module::get().version();
    const bool runtimeIsAE = version.minor() >= 6;
    return runtimeIsAE == BUILT_FOR_AE;
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
// CommonLib/SKSE Exports
//

extern "C" __declspec(dllexport) bool SKSEAPI SKSEPlugin_Load(const SKSE::LoadInterface* skse) // NOLINT
{
    SKSE::Init(skse);
    setupLog();

    spdlog::info("{} {} loading", PLUGIN_NAME, PLUGIN_VERSION);

    // Refuse to load an AE build on SE (and vice versa) instead of crashing later
    if (!runtimeMatchesBuild()) {
        return false;
    }

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

#ifdef SKYRIM_SUPPORT_AE
extern "C" __declspec(dllexport) constinit auto SKSEPlugin_Version // NOLINT
    = []() noexcept -> SKSE::PluginVersionData {
    SKSE::PluginVersionData v;
    v.PluginName(PLUGIN_NAME);
    v.PluginVersion(REL::Version(PLUGIN_VERSION));
    v.UsesAddressLibrary();
    v.UsesUpdatedStructs();
    v.CompatibleVersions({SKSE::RUNTIME_SSE_LATEST});
    return v;
}();
#endif

extern "C" __declspec(dllexport) bool SKSEAPI SKSEPlugin_Query(const SKSE::QueryInterface*, // NOLINT
                                                               SKSE::PluginInfo* pluginInfo)
{
#ifdef SKYRIM_SUPPORT_AE
    pluginInfo->name = &SKSEPlugin_Version.pluginName[0];
    pluginInfo->version = SKSEPlugin_Version.pluginVersion;
#else
    pluginInfo->name = PLUGIN_NAME;
    pluginInfo->version = 1;
#endif
    pluginInfo->infoVersion = SKSE::PluginInfo::kVersion;
    return true;
}
