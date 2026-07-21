#pragma once

#include "PCH.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace HorizonFix {

/**
 * @brief Builds and maintains the water skirt: a ring of oversized LOD water clones around the loaded area
 *
 * The skirt fills the horizon gap between the far clip plane and the sky with water. It works by
 * cloning the worldspace's own LOD water mesh (so material, flow, and reflections match) and tiling
 * the clones in a disc around the player, beyond the area the game's real LOD water covers. Tiles
 * are made cull-proof (huge model bound + kAlwaysDraw) so the engine never rejects them against the
 * far plane; SkirtDepth then forces their fragments to a depth just inside 1.0 so they only win in
 * pixels nothing else drew. Because engine culling is defeated, this class performs its own frustum
 * culling every frame (updateVisibility, driven by the SkirtCull hook).
 */
class WaterSkirt {
public:
    static constexpr const char* K_TILE_NAME
        = "HSF_WaterSkirt"; /**< Name of tiles; SkirtDepth identifies skirt render passes by this name */
    static constexpr const char* K_ROOT_NAME = "HSF_WaterSkirtRoot"; /**< Name of the tile parent node */

private:
    /**
     * @brief One inward-facing frustum plane: a point p is inside when normal.Dot(p) - d > -radius
     */
    struct FrustumPlane {
        RE::NiPoint3 normal; /**< Unit normal pointing into the frustum */
        float d {}; /**< Plane offset: normal.Dot(anyPointOnPlane) */
    };

    /** @brief The six planes of a view frustum: near, far, left, right, top, bottom */
    static constexpr size_t NUM_FRUSTUM_PLANES = 6;
    using FrustumPlanes = std::array<FrustumPlane, NUM_FRUSTUM_PLANES>;

    static constexpr float K_TILE_SIZE
        = 131072.0F; /**< Base tile edge length: one LOD32 block (32 cells x 4096 units) */
    static constexpr float K_CULL_PROOF_RADIUS
        = 1.0e9F; /**< modelBound radius large enough that the engine's frustum test always passes */
    static constexpr float K_NEAR_SUBTILE_SIZE
        = K_TILE_SIZE / 8.0F; /**< Edge length of the subtile representation of the 3x3 near blocks (16384 = 4 cells).
                                 Each near block carries both a full tile (drawn while the block is entirely free of
                                 content - one draw call) and this 8x8 grid (drawn when live water or defined map
                                 cells touch the block, minus the covered pieces); updateVisibility shows one
                                 representation per frame */
    static constexpr float K_COVERAGE_CELL = 4096.0F; /**< Cell edge of the near-zone coverage grids (one game cell) */
    static constexpr int K_COVERAGE_GRID = static_cast<int>(
        (3.0F * K_TILE_SIZE) / K_COVERAGE_CELL); /**< Coverage grid cells per axis over the 3x3 near blocks */
    static constexpr int K_CELLS_PER_BLOCK
        = static_cast<int>(K_TILE_SIZE / K_COVERAGE_CELL); /**< Game cells per LOD32 block edge (32) */
    static constexpr int K_CELL_SHIFT
        = 12; /**< log2 of the cell size; the engine converts world units to cell coords by flooring and shifting */
    static constexpr float K_COVERAGE_EPSILON
        = 0.01F; /**< Tolerance in cells for float noise on cell-aligned water boxes */

    /**
     * @brief One tile of the skirt layout, relative to the center block's midpoint
     */
    struct RelTile {
        float dx; /**< X offset of the tile center from the layout center */
        float dy; /**< Y offset of the tile center from the layout center */
        float size; /**< Edge length of the tile */
    };

    /**
     * @brief XY rectangle of one live water piece
     *
     * Taken from the engine's multibound AABBs when present (exact), or from the shape's
     * sphere bound as a fallback - a big water mesh's bounding circle overhangs its true
     * rectangle by up to ~40% of its half-width, and hiding skirt on the overhang punches
     * a thin arc of missing skirt beside large waters (field-observed in Apocrypha).
     */
    struct WaterFootprint {
        float x; /**< World X of the rectangle center */
        float y; /**< World Y of the rectangle center */
        float halfX; /**< Half-extent along X */
        float halfY; /**< Half-extent along Y */
    };

    /**
     * @brief Per-frame picture of where the game's real water renders in the near zone
     *
     * Refreshed by refreshNearWaterCoverage, consumed by isHiddenByNearCoverage (together
     * with s_nearMap) to pick each near block's representation and to hide the skirt pieces
     * real water covers.
     */
    struct NearWaterCoverage {
        std::vector<WaterFootprint> footprints; /**< Live water rectangles */
        std::array<std::array<bool, K_COVERAGE_GRID>, K_COVERAGE_GRID>
            covered {}; /**< Cells fully inside the water union */
        std::array<std::array<bool, 3>, 3> blockHasWater {}; /**< Which of the 3x3 near blocks touch any water */
    };

    /**
     * @brief Layout of the engine's per-file worldspace cell offset table (WRLD OFST)
     *
     * The loader skips the exterior-cell groups of master-flagged files and later finds those
     * cells by seeking to fileOffsets[(x - minCellX) + (y - minCellY) * width]; a zero entry
     * means the file has no record for that cell. Extents are stored in world units and become
     * cell coordinates by flooring and an arithmetic shift (see K_CELL_SHIFT). Verified on
     * 1.6.1170: TESWorldSpace::Load's OFST case (0x140305380), the index math (0x140306750),
     * and the cell-record lookup that gates on the master flag (0x1403064C0). The tables live
     * in the hashmap at TESWorldSpace+0x1D0 (CommonLib's unk1D0), keyed by the file's
     * canonical threadSafeParent.
     */
    struct WorldCellOffsetData {
        std::uint32_t* fileOffsets; /**< Per-cell record offsets, row-major by Y; 0 = cell absent */
        float minX; /**< West edge of the covered rectangle, world units */
        float minY; /**< South edge, world units */
        float maxX; /**< East edge, world units */
        float maxY; /**< North edge, world units */
        std::uint32_t unk18; /**< Unknown; unused here */
    };

    /**
     * @brief Where the worldspace's map defines cells over the near zone, at cell resolution
     *
     * A cell counts as map when the land-owning worldspace has an exterior cell there - found
     * via cellMap (non-master plugins register every exterior cell at load; master-file cells
     * appear as the engine materializes them) or via a master file's OFST table (the engine's
     * lazy path for cells it has not materialized, see WorldCellOffsetData). Near tiles must
     * not paint skirt water where the map has content of its own; only genuinely undefined
     * void gets filled. Unlike the per-frame water picture this is static per (worldspace,
     * center block), so refreshNearMapCoverage recomputes it only when either changes.
     */
    struct NearMapCoverage {
        std::array<std::array<bool, K_COVERAGE_GRID>, K_COVERAGE_GRID> covered {}; /**< Cells the map defines */
        std::array<std::array<bool, 3>, 3> blockHasMap {}; /**< Which of the 3x3 near blocks contain any map cell */
        RE::TESWorldSpace* worldSpace = nullptr; /**< Worldspace the grid was computed for (cache key) */
        int centerBx = std::numeric_limits<int>::max(); /**< Center block X the grid was computed for (cache key) */
        int centerBy = std::numeric_limits<int>::max(); /**< Center block Y the grid was computed for (cache key) */
    };

    static inline std::vector<RE::NiPointer<RE::BSGeometry>>
        s_tiles; /**< Live tile geometries, index-matched to s_layout */
    static inline std::vector<RelTile> s_layout; /**< Relative tile layout, rebuilt per worldspace */
    static inline RE::NiPointer<RE::NiNode>
        s_skirtRoot; /**< Parent node holding every tile, attached under the LOD roots */
    static inline RE::TESWorldSpace* s_skirtWorldSpace; /**< Worldspace the current skirt was built for */
    static inline int s_centerBx; /**< X index of the LOD32 block the skirt is centered on */
    static inline int s_centerBy; /**< Y index of the LOD32 block the skirt is centered on */
    static inline RE::NiPoint3 s_modelCenter; /**< Center of the template quad in its own model space */
    static inline float s_modelSide; /**< Edge length of the template quad in model space */
    static inline float s_skirtHeight; /**< World Z the tiles sit at: the worldspace's effective water height */
    static inline std::atomic_bool s_taskPending; /**< Coalesces queueUpdate calls into a single queued task */
    static inline bool s_mapMenuOpen = false; /**< True while the map menu is open and the skirt is force-hidden */
    static inline NearWaterCoverage s_nearWater; /**< Live water picture for the current frame (see updateVisibility) */
    static inline NearMapCoverage s_nearMap; /**< Defined-cell picture for the current center block (see updateSkirt) */

    /**
     * @brief What classifyDonor concluded about a candidate donor mesh
     *
     * kNotSolidWater candidates are always rejected. kUnverifiable ones are trusted only when
     * they have the exact shape signature of an engine-created water quad (4 vertices, 2
     * triangles): full square quads by construction. Engine quads themselves now usually
     * measure properly (their bare float4 vertex layout is decoded since the 1.6.1170
     * verification of descriptor 0x0000100000000004), so the topology trust remains only for
     * meshes with no readable CPU copy at all. An unverifiable mesh with any other topology
     * is assumed irregular and rejected - a striped horizon was traced to exactly that case
     * (a 6-vertex donor whose geometry could not be measured).
     */
    enum class DonorVerdict : std::uint8_t {
        K_SOLID_WATER, /**< Measured: one flat, gap-free, square sheet of water */
        K_NOT_SOLID_WATER, /**< Measured: narrow, gappy, or otherwise not a solid square */
        K_UNVERIFIABLE, /**< No usable CPU geometry to measure */
    };

    /**
     * @brief classifyDonor result: the verdict plus a short reason for the log
     */
    struct DonorCheck {
        DonorVerdict verdict = DonorVerdict::K_UNVERIFIABLE; /**< What the measurement concluded */
        const char* detail = "not measured"; /**< Why, in a word or two (log only) */
    };

    /**
     * @brief Running best candidate while scanning the LOD water tree for a clone template
     *
     * BTR water meshes follow shorelines, so most are irregular; a 4-vertex tri shape is a clean
     * full rectangle, and the largest one is a fully submerged ocean chunk. Prefer that as the
     * clone template. Candidates that are verifiably not solid water are rejected outright
     * (classifyDonor); cloning one would tile the horizon with evenly spaced stripes.
     */
    struct TemplateSearch {
        RE::BSTriShape* best = nullptr; /**< Best template found so far */
        std::uint32_t bestVertexCount = 0; /**< Vertex count of best (lower wins) */
        float bestRadius = 0.0F; /**< World bound radius of best (tie-breaker, higher wins) */
        DonorCheck bestCheck; /**< classifyDonor result for best */
        std::uint32_t rejectedCount
            = 0; /**< Candidates rejected: measured not solid, or unmeasurable and not quad-shaped */
    };

public:
    /**
     * @brief Schedules a skirt update (updateSkirt) on the main thread via the SKSE task queue
     *
     * Safe to call from any thread and from event sinks; repeated calls while a task is already
     * queued are coalesced into one.
     */
    static void queueUpdate();

    /**
     * @brief Per-frame manual frustum culling of the skirt tiles
     *
     * Because the tiles are cull-proof to the engine, this replicates the frustum test the engine
     * would have done, using each tile's true bound instead of the inflated one. Called every frame
     * from the Atmosphere::Update hook (SkirtCull).
     */
    static void updateVisibility();

    /**
     * @brief Hide or show the whole skirt for the map menu
     *
     * Hide the whole skirt while the map menu is up. The local map renders the world through its
     * own camera, where the skirt's cull-proof tiles and the per-draw depth hook have no business
     * (and crash the local map). Driven by a menu event rather than a per-frame check because the
     * map pauses the game, so the Atmosphere update that ticks updateVisibility stops firing.
     *
     * @param open True when the map menu is opening, false when it closes
     */
    static void setMapMenuOpen(bool open);

private:
    /**
     * @brief Extracts one column of a rotation matrix as a vector (a world-space basis axis)
     *
     * @param rot Rotation matrix to read from
     * @param col Column index: 0 = forward, 1 = up, 2 = right for a camera world rotation
     * @return RE::NiPoint3 The column as a vector
     */
    static auto rotationColumn(const RE::NiMatrix3& rot,
                               int col) -> RE::NiPoint3;

    /**
     * @brief Computes the six inward-facing world-space planes of the camera's view frustum
     *
     * Handles both perspective and orthographic frusta.
     *
     * @param camera Camera whose world transform and viewFrustum are used
     * @param planes Receives the planes in near, far, left, right, top, bottom order
     */
    static void buildFrustumPlanes(const RE::NiCamera* camera,
                                   FrustumPlanes& planes);

    /**
     * @brief Sphere-vs-frustum visibility test
     *
     * @param bound Bounding sphere to test
     * @param planes Frustum planes from buildFrustumPlanes
     * @return true If the sphere is at least partially inside every plane
     * @return false If the sphere is fully outside any plane
     */
    static auto boundInFrustum(const RE::NiBound& bound,
                               const FrustumPlanes& planes) -> bool;

    /**
     * @brief Reads TES::worldSpace in a way that is safe on every runtime
     *
     * Works around the TES layout change in game patch 1.6.1130 (see the implementation for
     * details); reading the struct member directly would misread the field on older runtimes.
     *
     * @param tesPtr The TES singleton
     * @return RE::TESWorldSpace* The current exterior worldspace, or nullptr
     */
    static auto getWorldSpace(RE::TES* tesPtr) -> RE::TESWorldSpace*;

    /**
     * @brief Resolves the worldspace that owns the LOD data (and thus the LOD water height)
     *
     * Small worldspaces (e.g. cities) inherit LOD from a parent; this walks up parentWorld while
     * the kUseLODData flag is set.
     *
     * @param worldSpacePtr Worldspace to start from
     * @return RE::TESWorldSpace* The worldspace whose LOD data is in use
     */
    static auto getLODWorldSpace(RE::TESWorldSpace* worldSpacePtr) -> RE::TESWorldSpace*;

    /**
     * @brief Recursively scans the LOD water scene graph for the best tile to clone
     *
     * Prefers the tri shape with the fewest vertices (ties broken by largest world bound), which
     * selects a clean fully-submerged rectangular quad; see TemplateSearch. Skips geometry this
     * plugin created itself.
     *
     * @param objPtr Subtree to scan (tolerates nullptr)
     * @param search In/out running best candidate
     */
    static void searchTemplateQuad(RE::NiAVObject* objPtr,
                                   TemplateSearch& search);

    /**
     * @brief Detaches every tile and the skirt root from the scene and resets all cached state
     */
    static void removeSkirt();

    /**
     * @brief Clones the template water quad through the engine's NiCloningProcess
     *
     * @param templatePtr Tri shape to clone
     * @return RE::NiPointer<RE::BSGeometry> The clone, or nullptr if cloning failed
     */
    static auto cloneTemplate(RE::BSTriShape* templatePtr) -> RE::NiPointer<RE::BSGeometry>;

    /**
     * @brief Measures whether a candidate donor mesh is one solid square sheet of water
     *
     * The template search scores by vertex count and bound, which cannot tell a full square
     * ocean quad from a narrow 4-vertex river rectangle; the scale math in updateSkirt assumes
     * a solid square, so any other shape repeats identically in every tile as evenly spaced
     * stripes or gaps across the horizon.
     *
     * Reads the CPU-side vertex/index copies and checks that the mesh is flat, square
     * (min/max extent ratio), and gap-free (summed triangle area matches the extents
     * rectangle). The decode is trusted only when it agrees with the mesh's own model bound:
     * engine-created water quads keep their rendered positions elsewhere (their CPU copy
     * decodes as zeros), so they - like meshes with no CPU copy at all - come back
     * kUnverifiable rather than kNotSolidWater. See DonorVerdict for how unverifiable
     * candidates are treated by the search.
     *
     * @param shape Candidate donor from the LOD water tree
     * @return DonorCheck The verdict plus a short reason string for logging
     */
    static auto classifyDonor(const RE::BSTriShape* shape) -> DonorCheck;

    /**
     * @brief Decodes an IEEE 754 half-precision value (how vertex layouts without the
     * full-precision flag store positions)
     *
     * @param half The 16-bit pattern to decode
     * @return float The decoded value
     */
    static auto halfToFloat(std::uint16_t half) -> float;

    /**
     * @brief Rebuilds the per-frame picture of where the game's real water renders (s_nearWater)
     *
     * Gathers live water rectangles from the water system (multibound AABBs first, sphere
     * bounds as fallback), rasterizes their union onto the near-zone coverage grid at cell
     * resolution - a cell is marked only when a box covers it completely - and records which
     * of the 3x3 near blocks touch water.
     *
     * @param centerX World X of the layout center
     * @param centerY World Y of the layout center
     */
    static void refreshNearWaterCoverage(float centerX,
                                         float centerY);

    /**
     * @brief Rebuilds s_nearMap: which near-zone cells the worldspace's map defines
     *
     * No-op while the worldspace and center block are unchanged. Walks to the land-owning
     * worldspace first (parentUseFlags kUseLandData, mirroring the engine's own land lookup),
     * then marks cells from the master files' OFST tables and from cellMap. Called from
     * updateSkirt only, so the reads run on the main thread - cellMap can rehash when the
     * engine materializes a cell, and that also happens on the main thread.
     */
    static void refreshNearMapCoverage();

    /**
     * @brief Whether a near tile must not draw this frame because the game has content there
     *
     * Implements the near-block representation choice (see buildLayout): a full near tile
     * draws only while its block is entirely free of content (no live water and no defined
     * map cells); subtiles draw otherwise, minus those fully inside the union of live water
     * and map cells. Edge-straddling subtiles keep drawing through the boundary - see the
     * implementation for why that overlap is safe.
     *
     * @param rel Layout entry of the tile
     * @return bool True when the tile must be hidden this frame
     */
    static auto isHiddenByNearCoverage(const RelTile& rel) -> bool;

    /**
     * @brief The configured skirt radius, clamped to a workable minimum of four tile lengths
     *
     * @return float Radius in game units used for layout
     */
    static auto effectiveRadius() -> float;

    /**
     * @brief Adds one candidate tile to s_layout, quad-splitting tiles that straddle the rim
     *
     * Tiles fully inside the radius are emitted as-is, tiles fully outside are dropped, and tiles
     * crossing the rim are split into four quadrants up to splitsLeft times so the skirt edge
     * approximates a circle instead of a coarse staircase.
     *
     * @param dx Tile center X offset from the layout center
     * @param dy Tile center Y offset from the layout center
     * @param size Tile edge length
     * @param splitsLeft Remaining subdivision budget (from iWaterSkirtRimQuality)
     * @param radius Skirt radius the layout is trimmed to
     */
    static void layoutTile(float dx,
                           float dy,
                           float size,
                           int splitsLeft,
                           float radius);

    /**
     * @brief Rebuilds s_layout: a disc of tiles out to the effective radius
     *
     * The central block is included: the depth stamp makes the inner tile lose everywhere
     * real water or terrain drew, while filling the hole in worldspaces whose LOD water
     * does not cover the player's whole block.
     */
    static void buildLayout();

    /**
     * @brief Moves and scales the existing tiles so the layout is centered on the given LOD32 block
     *
     * @param centerBx X index of the block the player is in
     * @param centerBy Y index of the block the player is in
     */
    static void placeTiles(int centerBx,
                           int centerBy);

    /**
     * @brief Full skirt refresh; runs on the main thread via queueUpdate
     *
     * Removes the skirt when no exterior worldspace is active, recycles the existing tiles when
     * only the player's block changed, and otherwise rebuilds everything: finds a template quad,
     * builds the layout, clones and attaches the tiles. Both non-removal paths also refresh the
     * near map coverage (refreshNearMapCoverage).
     */
    static void updateSkirt();
};

/**
 * @brief Event sink that schedules a skirt update whenever a cell is attached
 *
 * Cell attach is the signal that the loaded area moved (player travel, worldspace change, load),
 * i.e. that LOD water may now exist for a new worldspace or the skirt may need to follow.
 */
class CellAttachSink final : public RE::BSTEventSink<RE::TESCellAttachDetachEvent> {
public:
    /**
     * @brief Get the singleton instance
     *
     * @return CellAttachSink* The single instance of this sink
     */
    static auto getSingleton() -> CellAttachSink*
    {
        static CellAttachSink sink;
        return &sink;
    }

    auto ProcessEvent(const RE::TESCellAttachDetachEvent* event,
                      RE::BSTEventSource<RE::TESCellAttachDetachEvent>* /*a_eventSource*/)
        -> RE::BSEventNotifyControl override
    {
        if ((event != nullptr) && event->attached) {
            WaterSkirt::queueUpdate();
        }
        return RE::BSEventNotifyControl::kContinue;
    }
};

/**
 * @brief Event sink that hides the skirt while the map menu is open (see WaterSkirt::setMapMenuOpen)
 */
class MapMenuSink final : public RE::BSTEventSink<RE::MenuOpenCloseEvent> {
public:
    /**
     * @brief Get the singleton instance
     *
     * @return MapMenuSink* The single instance of this sink
     */
    static auto getSingleton() -> MapMenuSink*
    {
        static MapMenuSink sink;
        return &sink;
    }

    auto ProcessEvent(const RE::MenuOpenCloseEvent* event,
                      RE::BSTEventSource<RE::MenuOpenCloseEvent>* /*a_eventSource*/)
        -> RE::BSEventNotifyControl override
    {
        if ((event != nullptr) && event->menuName == RE::MapMenu::MENU_NAME) {
            WaterSkirt::setMapMenuOpen(event->opening);
        }
        return RE::BSEventNotifyControl::kContinue;
    }
};

} // namespace HorizonFix
