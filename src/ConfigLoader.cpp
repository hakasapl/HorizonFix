#include "ConfigLoader.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cwchar>
#include <filesystem>
#include <spdlog/spdlog.h>
#include <string>
#include <vector>

using namespace HorizonFix;

void ConfigLoader::loadConfig()
{
    // The INI lives next to the plugin DLL; current_path is the game root at load time
    const auto iniPath = std::filesystem::current_path() / "Data" / "SKSE" / "Plugins" / "HorizonFix.ini";

    // Read every key from [General]; each falls back to its default independently
    s_config.iniSkirtRadius = readIniFloat(iniPath, L"fWaterSkirtRadius", DEFAULT_RADIUS);
    s_config.iniRimQuality = std::clamp(
        static_cast<int>(readIniFloat(iniPath, L"iWaterSkirtRimQuality", DEFAULT_RIM_QUALITY)), 0, MAX_RIM_QUALITY);
    s_config.skirtRadius = s_config.iniSkirtRadius;
    s_config.rimQuality = s_config.iniRimQuality;
    s_config.iniHorizonBlendDegrees
        = std::clamp(readIniFloat(iniPath, L"fHorizonBlendDegrees", DEFAULT_HORIZON_BLEND), 0.0F, MAX_HORIZON_BLEND);
    s_config.horizonBlendDegrees = s_config.iniHorizonBlendDegrees;
    s_config.horizonBlendOpaquePercent
        = std::clamp(readIniFloat(iniPath, L"fHorizonBlendOpaquePercent", DEFAULT_HORIZON_OPAQUE_PERCENT),
                     0.0F,
                     MAX_HORIZON_OPAQUE_PERCENT);
    s_config.horizonBlendDebug = readIniFloat(iniPath, L"bHorizonBlendDebug", 0.0F) != 0.0F;
    s_config.worldSpaceBlocklist = readIniStringList(iniPath, L"sWorldSpaceBlocklist");
    s_config.smallWorldAllowlist = readIniStringList(iniPath, L"sSmallWorldAllowlist");

    // Log the effective values so user reports include them
    spdlog::info("Config Loaded: Water Skirt Radius: {}", getSkirtRadius());
    spdlog::info("Config Loaded: Water Skirt Rim Quality: {}", getRimQuality());
    spdlog::info("Config Loaded: Horizon Blend Degrees: {}", getHorizonBlendDegrees());
    spdlog::info("Config Loaded: Horizon Blend Opaque Percent: {}", s_config.horizonBlendOpaquePercent);
    spdlog::info("Config Loaded: Horizon Blend Debug: {}", s_config.horizonBlendDebug);
    const auto joinList = [](const std::vector<std::string>& list) -> std::string {
        std::string joined;
        for (const auto& entry : list) {
            if (!joined.empty()) {
                joined += ", ";
            }
            joined += entry;
        }
        return joined;
    };
    spdlog::info("Config Loaded: World Space Blocklist: [{}]", joinList(s_config.worldSpaceBlocklist));
    spdlog::info("Config Loaded: Small World Allowlist: [{}]", joinList(s_config.smallWorldAllowlist));
}

auto ConfigLoader::getSkirtRadius() -> float { return s_config.skirtRadius.load(std::memory_order_relaxed); }

auto ConfigLoader::getRimQuality() -> int { return s_config.rimQuality.load(std::memory_order_relaxed); }

void ConfigLoader::setSkirtRadius(float radius) { s_config.skirtRadius.store(radius, std::memory_order_relaxed); }

void ConfigLoader::setRimQuality(int quality)
{
    s_config.rimQuality.store(std::clamp(quality, 0, MAX_RIM_QUALITY), std::memory_order_relaxed);
}

auto ConfigLoader::getIniSkirtRadius() -> float { return s_config.iniSkirtRadius; }

auto ConfigLoader::getIniRimQuality() -> int { return s_config.iniRimQuality; }

auto ConfigLoader::getHorizonBlendDegrees() -> float
{
    return s_config.horizonBlendDegrees.load(std::memory_order_relaxed);
}

void ConfigLoader::setHorizonBlendDegrees(float degrees)
{
    s_config.horizonBlendDegrees.store(std::clamp(degrees, 0.0F, MAX_HORIZON_BLEND), std::memory_order_relaxed);
}

auto ConfigLoader::getIniHorizonBlendDegrees() -> float { return s_config.iniHorizonBlendDegrees; }

auto ConfigLoader::getHorizonBlendOpaquePercent() -> float { return s_config.horizonBlendOpaquePercent; }

auto ConfigLoader::isHorizonBlendDebug() -> bool { return s_config.horizonBlendDebug; }

auto ConfigLoader::isWorldSpaceBlocked(const char* editorID) -> bool
{
    return listContainsID(s_config.worldSpaceBlocklist, editorID);
}

auto ConfigLoader::isSmallWorldAllowed(const char* editorID) -> bool
{
    return listContainsID(s_config.smallWorldAllowlist, editorID);
}

auto ConfigLoader::listContainsID(const std::vector<std::string>& list,
                                  const char* editorID) -> bool
{
    if (editorID == nullptr || *editorID == '\0') {
        return false;
    }
    return std::ranges::any_of(
        list, [editorID](const std::string& entry) -> bool { return _stricmp(entry.c_str(), editorID) == 0; });
}

auto ConfigLoader::readIniFloat(const std::filesystem::path& path,
                                const wchar_t* key,
                                float defVal) -> float
{
    // check if ini file exists
    if (!std::filesystem::exists(path)) {
        return defVal;
    }

    // Read the raw value string from the [General] section using the Windows API
    std::array<wchar_t, INI_BUFFER_SIZE> buffer {};
    const auto pathStr = path.wstring();
    GetPrivateProfileStringW(L"General", key, L"", buffer.data(), static_cast<DWORD>(buffer.size()), pathStr.c_str());

    // Empty result means the key is missing (or blank); use the default
    if (buffer.at(0) == L'\0') {
        return defVal;
    }

    // Parse as float; wcstof leaves end at the buffer start when nothing was consumed
    wchar_t* end = nullptr;
    const float parsed = std::wcstof(buffer.data(), &end);
    if (end == buffer.data()) {
        return defVal;
    }
    return parsed;
}

auto ConfigLoader::readIniStringList(const std::filesystem::path& path,
                                     const wchar_t* key) -> std::vector<std::string>
{
    std::vector<std::string> result;

    // check if ini file exists
    if (!std::filesystem::exists(path)) {
        return result;
    }

    // Read the raw value string from the [General] section using the Windows API
    std::array<wchar_t, INI_LIST_BUFFER_SIZE> buffer {};
    const auto pathStr = path.wstring();
    GetPrivateProfileStringW(L"General", key, L"", buffer.data(), static_cast<DWORD>(buffer.size()), pathStr.c_str());

    // Split on commas, trimming surrounding whitespace and dropping empty entries.
    // Editor IDs are plain ASCII, so narrowing each character is lossless in practice.
    std::string entry;
    const auto flushEntry = [&result, &entry]() -> void {
        const auto first = entry.find_first_not_of(" \t");
        if (first != std::string::npos) {
            const auto last = entry.find_last_not_of(" \t");
            result.emplace_back(entry.substr(first, last - first + 1));
        }
        entry.clear();
    };
    for (const wchar_t chr : buffer) {
        if (chr == L'\0') {
            break;
        }
        if (chr == L',') {
            flushEntry();
            continue;
        }
        entry.push_back(static_cast<char>(chr));
    }
    flushEntry();
    return result;
}
