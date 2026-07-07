#include "ConfigLoader.hpp"

#include <Windows.h>

#include <array>
#include <cwchar>
#include <filesystem>

using namespace HorizonFix;

void ConfigLoader::loadConfig()
{
    const auto iniPath = std::filesystem::current_path() / "Data" / "SKSE" / "Plugins" / "HorizonFix.ini";
    const auto path = iniPath.wstring();

    const auto farDistance = readIniFloat(path.c_str(), L"fFarDistance", DEFAULT_FAR_DISTANCE);
    s_config.farPlaneDistance = farDistance;
    if (s_config.farPlaneDistance > FAR_DISTANCE_WARN_THRESHOLD) {
        spdlog::warn("fFarDistance {} exceeds the recommended maximum of "
                     "2000000, artifacts are expected",
                     s_config.farPlaneDistance);
    }

    s_config.skirtRadius = readIniFloat(path.c_str(), L"fWaterSkirtRadius", DEFAULT_RADIUS);
    s_config.skirtZOffset = readIniFloat(path.c_str(), L"fWaterSkirtZOffset", DEFAULT_Z_OFFSET);

    spdlog::info("Config Loaded: Far clip distance: {}", s_config.farPlaneDistance);
    spdlog::info("Config Loaded: Water Skirt Radius: {}", s_config.skirtRadius);
    spdlog::info("Config Loaded: Water Skirt Z Offset: {}", s_config.skirtZOffset);
}

auto ConfigLoader::getSkirtRadius() -> float { return s_config.skirtRadius; }

auto ConfigLoader::getSkirtZOffset() -> float { return s_config.skirtZOffset; }

auto ConfigLoader::getFarPlaneDistance() -> float { return s_config.farPlaneDistance; }

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