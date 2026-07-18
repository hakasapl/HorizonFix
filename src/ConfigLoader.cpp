#include "ConfigLoader.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <string>
#include <vector>

using namespace HorizonFix;

void ConfigLoader::loadConfig()
{
    // The INI lives next to the plugin DLL; current_path is the game root at load time
    const auto iniPath = std::filesystem::current_path() / "Data" / "SKSE" / "Plugins" / "HorizonFix.ini";

    // Read every key from [General]; each falls back to its default independently
    s_config.skirtRadius = readIniFloat(iniPath, L"fWaterSkirtRadius", DEFAULT_RADIUS);
    s_config.skirtZOffset = readIniFloat(iniPath, L"fWaterSkirtZOffset", DEFAULT_Z_OFFSET);
    s_config.rimQuality = std::clamp(
        static_cast<int>(readIniFloat(iniPath, L"iWaterSkirtRimQuality", DEFAULT_RIM_QUALITY)), 0, MAX_RIM_QUALITY);
    s_config.nearWater = readIniFloat(iniPath, L"bWaterSkirtNearWater", DEFAULT_NEAR_WATER ? 1.0F : 0.0F) != 0.0F;
    s_config.worldSpaceBlocklist = readIniStringList(iniPath, L"sWorldSpaceBlocklist");
    s_config.smallWorldAllowlist = readIniStringList(iniPath, L"sSmallWorldAllowlist");

    // Log the effective values so user reports include them
    spdlog::info("Config Loaded: Water Skirt Radius: {}", s_config.skirtRadius);
    spdlog::info("Config Loaded: Water Skirt Z Offset: {}", s_config.skirtZOffset);
    spdlog::info("Config Loaded: Water Skirt Rim Quality: {}", s_config.rimQuality);
    spdlog::info("Config Loaded: Water Skirt Near Water: {}", s_config.nearWater);
    std::string blocklistJoined;
    for (const auto& entry : s_config.worldSpaceBlocklist) {
        if (!blocklistJoined.empty()) {
            blocklistJoined += ", ";
        }
        blocklistJoined += entry;
    }
    spdlog::info("Config Loaded: World Space Blocklist: [{}]", blocklistJoined);
    std::string allowlistJoined;
    for (const auto& entry : s_config.smallWorldAllowlist) {
        if (!allowlistJoined.empty()) {
            allowlistJoined += ", ";
        }
        allowlistJoined += entry;
    }
    spdlog::info("Config Loaded: Small World Allowlist: [{}]", allowlistJoined);
}

auto ConfigLoader::getSkirtRadius() -> float { return s_config.skirtRadius; }

auto ConfigLoader::getSkirtZOffset() -> float { return s_config.skirtZOffset; }

auto ConfigLoader::getRimQuality() -> int { return s_config.rimQuality; }

auto ConfigLoader::getNearWaterEnabled() -> bool { return s_config.nearWater; }

auto ConfigLoader::isWorldSpaceBlocked(const char* editorID) -> bool
{
    if (editorID == nullptr || *editorID == '\0') {
        return false;
    }
    return std::ranges::any_of(s_config.worldSpaceBlocklist, [editorID](const std::string& entry) -> bool {
        return _stricmp(entry.c_str(), editorID) == 0;
    });
}

auto ConfigLoader::isSmallWorldAllowed(const char* editorID) -> bool
{
    if (editorID == nullptr || *editorID == '\0') {
        return false;
    }
    return std::ranges::any_of(s_config.smallWorldAllowlist, [editorID](const std::string& entry) -> bool {
        return _stricmp(entry.c_str(), editorID) == 0;
    });
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
    if (buffer[0] == L'\0') {
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
