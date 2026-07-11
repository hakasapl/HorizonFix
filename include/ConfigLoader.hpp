#pragma once

namespace HorizonFix {

/**
 * @brief Loads and serves the plugin configuration from Data/SKSE/Plugins/HorizonFix.ini
 *
 * The configuration is read once at plugin load (loadConfig) into a static ConfigMap; the
 * getters are plain accessors and never touch the disk. Missing files, missing keys, or
 * unparsable values silently fall back to the compiled-in defaults.
 */
class ConfigLoader {
private:
    //
    // DEFAULT CFG VALUES
    //
    constexpr static float DEFAULT_RADIUS = 2000000.0F; /**< Default skirt radius in game units (fWaterSkirtRadius) */
    constexpr static float DEFAULT_Z_OFFSET = -10000.0F; /**< Default skirt height offset below LOD water (fWaterSkirtZOffset) */
    constexpr static float DEFAULT_WATER_DRAW_LAST = 1.0F; /**< Default for bWaterSkirtDrawLast (nonzero = enabled) */
    constexpr static float DEFAULT_RIM_QUALITY = 2.0F; /**< Default rim subdivision level (iWaterSkirtRimQuality) */
    constexpr static int MAX_RIM_QUALITY = 6; /**< Upper clamp for rim quality; each level quadruples potential rim tiles */

    /**
     * @brief ConfigMap structure which holds the configuration values for the plugin
     */
    struct ConfigMap {
        float skirtRadius; /**< Radius of the water skirt around the player, in game units */
        float skirtZOffset; /**< Vertical offset applied to the skirt relative to the LOD water height */
        bool waterDrawLast; /**< Whether skirt tiles attach under the LOD land root so water sorts after terrain */
        int rimQuality; /**< How many times rim tiles may be quad-split to approximate the circular edge */
    };

    static inline ConfigMap s_config; /**< Holds the current configuration values for the plugin */

    //
    // Hardcoded Settings
    //
    constexpr static size_t INI_BUFFER_SIZE = 64; /**< Character buffer size for reading a single INI value */

public:
    /**
     * @brief Loads the configuration values from the HorizonFix.ini file and stores them in the s_config variable
     */
    static void loadConfig();

    /**
     * @brief Get the Skirt Radius
     *
     * @return float The skirt radius value from the configuration
     */
    static auto getSkirtRadius() -> float;

    /**
     * @brief Get the Skirt Z Offset
     *
     * @return float The skirt Z offset value from the configuration
     */
    static auto getSkirtZOffset() -> float;

    /**
     * @brief Get the Water Draw Last setting
     *
     * @return true If water should be drawn last
     * @return false Otherwise
     */
    static auto getWaterDrawLast() -> bool;

    /**
     * @brief Get the Rim Quality
     *
     * @return int The rim quality value from the configuration
     */
    static auto getRimQuality() -> int;

private:
    /**
     * @brief Reads a single float value from the [General] section of an INI file
     *
     * @param path Path to the INI file
     * @param key Name of the key to read
     * @param defVal Value to return when the file or key is missing, or the value does not parse
     * @return float The parsed value, or defVal on any failure
     */
    static auto readIniFloat(const std::filesystem::path& path,
                             const wchar_t* key,
                             float defVal) -> float;
};

} // namespace HorizonFix
