#include "ConfigLoader.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cwchar>
#include <filesystem>

using namespace HorizonFix;

void ConfigLoader::loadConfig()
{
    // The INI lives next to the plugin DLL; current_path is the game root at load time
    const auto iniPath = std::filesystem::current_path() / "Data" / "SKSE" / "Plugins" / "HorizonFix.ini";

    // Read every key from [General]; each falls back to its default independently
    s_config.skirtRadius = readIniFloat(iniPath, L"fWaterSkirtRadius", DEFAULT_RADIUS);
    s_config.skirtZOffset = readIniFloat(iniPath, L"fWaterSkirtZOffset", DEFAULT_Z_OFFSET);
    s_config.waterDrawLast = readIniFloat(iniPath, L"bWaterSkirtDrawLast", DEFAULT_WATER_DRAW_LAST) != 0.0F;
    s_config.rimQuality = std::clamp(
        static_cast<int>(readIniFloat(iniPath, L"iWaterSkirtRimQuality", DEFAULT_RIM_QUALITY)), 0, MAX_RIM_QUALITY);

    // Log the effective values so user reports include them
    spdlog::info("Config Loaded: Water Skirt Radius: {}", s_config.skirtRadius);
    spdlog::info("Config Loaded: Water Skirt Z Offset: {}", s_config.skirtZOffset);
    spdlog::info("Config Loaded: Water Skirt Draw Last: {}", s_config.waterDrawLast);
    spdlog::info("Config Loaded: Water Skirt Rim Quality: {}", s_config.rimQuality);
}

auto ConfigLoader::getSkirtRadius() -> float { return s_config.skirtRadius; }

auto ConfigLoader::getSkirtZOffset() -> float { return s_config.skirtZOffset; }

auto ConfigLoader::getWaterDrawLast() -> bool { return s_config.waterDrawLast; }

auto ConfigLoader::getRimQuality() -> int { return s_config.rimQuality; }

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
