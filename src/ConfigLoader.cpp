#include "ConfigLoader.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cwchar>
#include <filesystem>

using namespace HorizonFix;

void ConfigLoader::loadConfig()
{
    const auto iniPath = std::filesystem::current_path() / "Data" / "SKSE" / "Plugins" / "HorizonFix.ini";
    const auto path = iniPath.wstring();

    s_config.skirtRadius = readIniFloat(path.c_str(), L"fWaterSkirtRadius", DEFAULT_RADIUS);
    s_config.skirtZOffset = readIniFloat(path.c_str(), L"fWaterSkirtZOffset", DEFAULT_Z_OFFSET);
    s_config.waterDrawLast = readIniFloat(path.c_str(), L"bWaterSkirtDrawLast", DEFAULT_WATER_DRAW_LAST) != 0.0F;
    s_config.rimQuality = std::clamp(
        static_cast<int>(readIniFloat(path.c_str(), L"iWaterSkirtRimQuality", DEFAULT_RIM_QUALITY)),
        0,
        MAX_RIM_QUALITY);

    // Pre-0.2 configs set the global far clip plane through this key; the
    // skirt now fits itself inside the vanilla far clip instead, so the value
    // is ignored (this also keeps ENB's captured far clip untouched).
    if (readIniFloat(path.c_str(), L"fFarDistance", 0.0F) != 0.0F) {
        spdlog::info("fFarDistance is deprecated and ignored: the far clip plane is no longer modified");
    }

    spdlog::info("Config Loaded: Water Skirt Radius: {}", s_config.skirtRadius);
    spdlog::info("Config Loaded: Water Skirt Z Offset: {}", s_config.skirtZOffset);
    spdlog::info("Config Loaded: Water Skirt Draw Last: {}", s_config.waterDrawLast);
    spdlog::info("Config Loaded: Water Skirt Rim Quality: {}", s_config.rimQuality);
}

auto ConfigLoader::getSkirtRadius() -> float { return s_config.skirtRadius; }

auto ConfigLoader::getSkirtZOffset() -> float { return s_config.skirtZOffset; }

auto ConfigLoader::getWaterDrawLast() -> bool { return s_config.waterDrawLast; }

auto ConfigLoader::getRimQuality() -> int { return s_config.rimQuality; }

auto ConfigLoader::readIniFloat(const wchar_t* path,
                                const wchar_t* key,
                                float defVal) -> float
{
    std::array<wchar_t, INI_BUFFER_SIZE> buffer {};
    GetPrivateProfileStringW(L"General", key, L"", buffer.data(), static_cast<DWORD>(buffer.size()), path);
    if (buffer[0] == L'\0') {
        return defVal;
    }
    wchar_t* end = nullptr;
    const float parsed = std::wcstof(buffer.data(), &end);
    if (end == buffer.data()) {
        return defVal;
    }
    return parsed;
}
