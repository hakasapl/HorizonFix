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

SKSEPluginInfo(.Version = REL::Version {0,
                                        1,
                                        0,
                                        0},
               .Name = "HorizonFix",
               .Author = "hakasapl",
               .StructCompatibility = SKSE::StructCompatibility::Independent,
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
