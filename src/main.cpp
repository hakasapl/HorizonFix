#include "ConfigLoader.hpp"
#include "SkirtCull.hpp"
#include "SkirtDepth.hpp"
#include "WaterSkirt.hpp"

#include <spdlog/sinks/basic_file_sink.h>

#include <filesystem>

using namespace HorizonFix;

namespace {
void setupLog()
{
    auto logsFolder = SKSE::log::log_directory();
    if (!logsFolder) {
        SKSE::stl::report_and_fail("SKSE log_directory not provided, logs disabled.");
    }

    auto logFilePath = *logsFolder / (std::string(PLUGIN_NAME) + ".log");
    auto fileLogger = std::make_shared<spdlog::sinks::basic_file_sink_mt>(logFilePath.string(), true);
    auto logger = std::make_shared<spdlog::logger>("log", std::move(fileLogger));

    spdlog::set_default_logger(std::move(logger));
    spdlog::set_level(spdlog::level::trace);
    spdlog::flush_on(spdlog::level::trace);
}

void installHooks()
{
    SkirtCull::AtmosphereUpdateHook::install();
    SkirtDepth::install();
}

// This DLL is compiled for exactly one runtime family (CommonLibSSE selects
// address library IDs and struct layouts at compile time from
// SKYRIM_SUPPORT_AE). A wrong-flavor DLL must bail out before any address
// lookup: letting it continue would abort with a "failed to locate address
// library" error box instead of being skipped quietly.
auto runtimeMatchesBuild() -> bool
{
#ifdef SKYRIM_SUPPORT_AE
    constexpr bool BUILT_FOR_AE = true;
#else
    constexpr bool BUILT_FOR_AE = false;
#endif
    const auto version = REL::Module::get().version();
    const bool runtimeIsAE = version.minor() >= 6;
    if (runtimeIsAE != BUILT_FOR_AE) {
        spdlog::warn("{} is built for {} but the running game is {}.{}.{}; "
                     "install the matching DLL instead",
                     PLUGIN_NAME,
                     BUILT_FOR_AE ? "AE (1.6.x)" : "SE (1.5.x)",
                     version.major(),
                     version.minor(),
                     version.patch());
        return false;
    }
    return true;
}

void messageHandler(SKSE::MessagingInterface::Message* msg)
{
    switch (msg->type) {
    case SKSE::MessagingInterface::kDataLoaded:
        if (auto* const holder = RE::ScriptEventSourceHolder::GetSingleton()) {
            holder->AddEventSink(CellAttachSink::getSingleton());
            spdlog::info("Water skirt: registered cell attach listener");
        }
        if (auto* const ui = RE::UI::GetSingleton()) {
            ui->AddEventSink(MapMenuSink::getSingleton());
            spdlog::info("Water skirt: registered map menu listener");
        }
        break;
    case SKSE::MessagingInterface::kPostLoadGame:
    case SKSE::MessagingInterface::kNewGame:
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

    if (!runtimeMatchesBuild()) {
        return false;
    }

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
