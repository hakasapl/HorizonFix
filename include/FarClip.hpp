#pragma once

namespace HorizonFix::FarClip {
struct GetFarDistanceHook {
    static auto thunk(RE::SceneGraph* sceneGraphPtr) -> float;

    static void install();

    static inline REL::Relocation<decltype(thunk)> s_func;
    static constexpr std::size_t K_INDEX = 0x3E; // SceneGraph::GetFarDistance

    // The engine's hardcoded exterior far clip (~86 cells). Every special
    // case handled by GetFarDistance — menu scene graphs, the interior fog
    // clamp, worldspaces without LOD, the bAutoViewDistance lerp — returns
    // a smaller value, so anything >= this constant is the exterior default
    // we want to replace, and anything below must keep vanilla behavior.
    static constexpr float K_VANILLA_EXTERIOR_FAR_CLIP = 353840.0F;
};
}
