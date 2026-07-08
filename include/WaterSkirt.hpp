#pragma once

namespace HorizonFix {

class WaterSkirt {
public:
    static constexpr const char* K_TILE_NAME = "HSF_WaterSkirt"; // Name of tiles
    static constexpr const char* K_ROOT_NAME = "HSF_WaterSkirtRoot"; // Name of the tile parent node

private:
    static constexpr float K_TILE_SIZE = 131072.0F; // LOD32 size
    static constexpr int K_RIM_SUBDIVISIONS = 4;
    static constexpr float K_CULL_PROOF_RADIUS = 1.0e9F;

    struct RelTile {
        float dx;
        float dy;
        float size;
    };

    static inline std::vector<RE::NiPointer<RE::BSGeometry>> s_tiles;
    static inline std::vector<RelTile> s_layout;
    static inline RE::NiPointer<RE::NiNode> s_skirtRoot;
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

    static void updateVisibility();

private:
    static auto getLODWorldSpace(RE::TESWorldSpace* worldSpacePtr) -> RE::TESWorldSpace*;

    static void searchTemplateQuad(RE::NiAVObject* objPtr,
                                   TemplateSearch& search);

    static void removeSkirt();

    static auto cloneTemplate(RE::BSTriShape* templatePtr) -> RE::NiPointer<RE::BSGeometry>;

    static auto effectiveRadius() -> float;

    static void buildLayout();

    static void placeTiles(int centerBx,
                           int centerBy);

    static void updateSkirt();
};

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
