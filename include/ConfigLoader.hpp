#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

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
    constexpr static float DEFAULT_RIM_QUALITY = 2.0F; /**< Default rim subdivision level (iWaterSkirtRimQuality) */
    constexpr static int MAX_RIM_QUALITY
        = 6; /**< Upper clamp for rim quality; each level quadruples potential rim tiles */
    constexpr static float DEFAULT_HORIZON_BLEND = 0.0F; /**< Default horizon blend angle: 0 = feature disabled */
    constexpr static float MAX_HORIZON_BLEND
        = 45.0F; /**< Upper clamp for the blend angle; beyond this the haze band dominates the view */
    constexpr static float DEFAULT_HORIZON_OPAQUE_PERCENT
        = 0.0F; /**< Default opaque share of the blend above the horizon line: 0 = the fade starts at the line */
    constexpr static float MAX_HORIZON_OPAQUE_PERCENT
        = 100.0F; /**< Upper clamp for the opaque share: the whole upper half opaque, no fade at all */

    /**
     * @brief ConfigMap structure which holds the configuration values for the plugin
     */
    struct ConfigMap {
        float skirtRadius {}; /**< Radius of the water skirt around the player, in game units */
        int rimQuality {}; /**< How many times rim tiles may be quad-split to approximate the circular edge */
        float horizonBlendDegrees {}; /**< Angular height of the water-to-sky horizon blend, in degrees above and
                                         below the waterline (fHorizonBlendDegrees); 0 disables the blend */
        float horizonBlendOpaquePercent {}; /**< Share of the blend's height above the horizon line that stays fully
                                               opaque before the fade into the sky (fHorizonBlendOpaquePercent) */
        std::vector<std::string>
            worldSpaceBlocklist; /**< Worldspace editor IDs where the skirt is disabled (sWorldSpaceBlocklist) */
        std::vector<std::string> smallWorldAllowlist; /**< Small World-flagged worldspace editor IDs where the skirt is
                                                         enabled anyway (sSmallWorldAllowlist) */
    };

    static inline ConfigMap s_config; /**< Holds the current configuration values for the plugin */

    //
    // Hardcoded Settings
    //
    constexpr static size_t INI_BUFFER_SIZE = 64; /**< Character buffer size for reading a single INI value */
    constexpr static size_t INI_LIST_BUFFER_SIZE
        = 2048; /**< Character buffer size for reading a comma-separated INI list */

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
     * @brief Get the Rim Quality
     *
     * @return int The rim quality value from the configuration
     */
    static auto getRimQuality() -> int;

    /**
     * @brief Get the horizon blend angle (fHorizonBlendDegrees)
     *
     * The water-to-sky blend spans this many degrees above and below the waterline at the
     * horizon, regardless of the active weather; 0 (the default) disables the blend entirely.
     *
     * @return float The blend angle in degrees, clamped to [0, 45]
     */
    static auto getHorizonBlendDegrees() -> float;

    /**
     * @brief Get the opaque share of the horizon blend above the horizon line (fHorizonBlendOpaquePercent)
     *
     * The band stays fully opaque in the water color for this percentage of its height
     * above the horizon line before it starts fading into the sky. It exists to blot out a
     * thin bright sky strip that sits right on the line (a weather's horizon color under a
     * darker lower sky). 0 (the default) starts the fade at the line; 100 removes the fade.
     *
     * @return float The percentage, clamped to [0, 100]
     */
    static auto getHorizonBlendOpaquePercent() -> float;

    /**
     * @brief Whether a worldspace is on the user's blocklist (sWorldSpaceBlocklist)
     *
     * @param editorID Worldspace editor ID to test, compared case-insensitively
     * @return bool True if the skirt must not be built in this worldspace; nullptr or empty IDs are never blocked
     */
    static auto isWorldSpaceBlocked(const char* editorID) -> bool;

    /**
     * @brief Whether a Small World-flagged worldspace is on the user's allowlist (sSmallWorldAllowlist)
     *
     * Small World worldspaces (cities, pocket worlds) get no skirt by default; entries here
     * re-enable it for specific worlds (e.g. custom maps that are flagged small but represent
     * open terrain).
     *
     * @param editorID Worldspace editor ID to test, compared case-insensitively
     * @return bool True if the skirt may be built despite the Small World flag
     */
    static auto isSmallWorldAllowed(const char* editorID) -> bool;

private:
    /**
     * @brief Case-insensitive membership test of an editor ID in a config list
     *
     * @param list Config entries to search
     * @param editorID Editor ID to look for; nullptr or empty never matches
     * @return bool True when the list contains the ID
     */
    static auto listContainsID(const std::vector<std::string>& list,
                               const char* editorID) -> bool;

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

    /**
     * @brief Reads a comma-separated list of strings from the [General] section of an INI file
     *
     * Entries are trimmed of surrounding whitespace and empty entries are dropped. A missing
     * file, missing key, or blank value yields an empty list.
     *
     * @param path Path to the INI file
     * @param key Name of the key to read
     * @return std::vector<std::string> The parsed entries
     */
    static auto readIniStringList(const std::filesystem::path& path,
                                  const wchar_t* key) -> std::vector<std::string>;
};

} // namespace HorizonFix
