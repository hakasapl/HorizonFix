#pragma once

namespace HorizonFix {

class WaterSkirt {
private:
    static constexpr const char* K_TILE_NAME = "HSF_WaterSkirt"; // Name of tiles
    static constexpr float K_TILE_SIZE = 131072.0F; // LOD32 size
    static constexpr int K_RIM_SUBDIVISIONS = 4;

    static constexpr float FARPLANE_CLAMP_FACTOR = 0.9F; // Clamp skirt radius to this fraction of far plane distance

    // One entry per tile: offset of the tile center from the window center,
    // plus the tile's side length. Computed once; the same layout is reused at
    // every window position, so recentering only re-translates the tile pool.
    struct RelTile {
        float dx;
        float dy;
        float size;
    };

    static inline std::vector<RE::NiPointer<RE::BSGeometry>> s_tiles;
    static inline std::vector<RelTile> s_layout;
    static inline RE::TESWorldSpace* s_skirtWorldSpace;
    static inline int s_centerBx;
    static inline int s_centerBy;
    static inline RE::NiPoint3 s_modelCenter;
    static inline float s_modelSide;
    static inline float s_skirtHeight;
    static inline std::atomic_bool s_taskPending;

    // BTR water meshes follow shorelines, so most are irregular; a 4-vertex tri
    // shape is a clean full rectangle, and the largest one is a fully submerged
    // ocean chunk. Prefer that as the clone template.
    struct TemplateSearch {
        RE::BSTriShape* best = nullptr;
        std::uint32_t bestVertexCount = 0;
        float bestRadius = 0.0F;
    };

public:
    static void queueUpdate();

private:
    // Mirrors the engine's LOD water lookup: walk parent worldspaces while the
    // child inherits LOD data.
    static auto getLODWorldSpace(RE::TESWorldSpace* worldSpacePtr) -> RE::TESWorldSpace*;

    static void searchTemplateQuad(RE::NiAVObject* objPtr,
                                   TemplateSearch& search);

    static void removeSkirt();

    static auto cloneTemplate(RE::BSTriShape* templatePtr) -> RE::NiPointer<RE::BSGeometry>;

    // The window must end in a real geometry edge before the far clip plane:
    // letting the far plane slice through the water produces a wobbling
    // waterline (depth precision at that range is thousands of units, so the
    // cut moves with every camera matrix change), while a mesh edge is stable
    // to a fraction of a unit.
    static auto effectiveRadius() -> float;

    static void buildLayout();

    // Position every pooled tile for a window centered on the given block.
    static void placeTiles(int centerBx,
                           int centerBy);

    static void updateSkirt();
};

// Build the window layout relative to its center: a circular disk of coarse
// tiles with a finely subdivided rim. Identical for every window position.

class CellAttachSink final : public RE::BSTEventSink<RE::TESCellAttachDetachEvent> {
public:
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

} // namespace HorizonFix
