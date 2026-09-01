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

    // The horizon blend band's tint-refresh hook is only needed while the feature is on;
    // disabled installs leave the effect shader untouched (config is loaded before this)
    if (ConfigLoader::getHorizonBlendDegrees() > 0.0F) {
        HorizonBand::installHooks();
    }
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
