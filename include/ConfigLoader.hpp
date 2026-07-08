#pragma once

namespace HorizonFix {

class ConfigLoader {
private:
    // DEFAULTS
    constexpr static float DEFAULT_RADIUS = 2000000.0F;
    constexpr static float DEFAULT_Z_OFFSET = -10000.0F;
    constexpr static float DEFAULT_WATER_DRAW_LAST = 1.0F;

    struct ConfigMap {
        float skirtRadius;
        float skirtZOffset;
        bool waterDrawLast;
    };

    static inline ConfigMap s_config;

    constexpr static size_t INI_BUFFER_SIZE = 64;

public:
    static void loadConfig();

    static auto getSkirtRadius() -> float;
    static auto getSkirtZOffset() -> float;
    static auto getWaterDrawLast() -> bool;

private:
    static auto readIniFloat(const wchar_t* path,
                             const wchar_t* key,
                             float defVal) -> float;
};

} // namespace HorizonFix
