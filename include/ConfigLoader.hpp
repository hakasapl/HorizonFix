#pragma once

namespace HorizonFix {

class ConfigLoader {
private:
    // DEFAULTS
    constexpr static float DEFAULT_RADIUS = 2000000.0F;
    constexpr static float DEFAULT_Z_OFFSET = -10000.0F;
    constexpr static float DEFAULT_FAR_DISTANCE = 2000000.0F;

    struct ConfigMap {
        // Half-extent of the tile window, centered on the player, in game units.
        // Clamped at runtime to 90% of the far clip distance: the window must end
        // in a real geometry edge before the far plane, because the far plane
        // slicing through the water produces a waterline that wobbles with camera
        // movement (depth precision at that range is thousands of units).
        float skirtRadius;
        // Offset applied to the carpet's base height. The carpet sits deep below
        // the vanilla LOD ocean plane: vanilla water draws on top of it, and it
        // shows through wherever vanilla water is missing. The total depth must
        // stay larger than the depth buffer noise of distant water (~4200 units
        // at 1M units viewing distance with SSE's non-reversed fp32 depth) so the
        // overlap never shimmers; it is invisible at horizon distances.
        float skirtZOffset;
        // The far clip distance configured for the far-clip hook; used to clamp
        // the radius. Set by the loader.
        float farPlaneDistance;
    };

    static inline ConfigMap s_config;

    constexpr static float FAR_DISTANCE_WARN_THRESHOLD = 2000000.0F;
    constexpr static size_t INI_BUFFER_SIZE = 64;

public:
    static void loadConfig();

    static auto getSkirtRadius() -> float;
    static auto getSkirtZOffset() -> float;
    static auto getFarPlaneDistance() -> float;

private:
    static auto readIniFloat(const wchar_t* path,
                             const wchar_t* key,
                             float defVal) -> float;
};

} // namespace HorizonFix