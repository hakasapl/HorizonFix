#include "FarClip.hpp"

#include "ConfigLoader.hpp"

#include <atomic>

using namespace HorizonFix::FarClip;

auto GetFarDistanceHook::thunk(RE::SceneGraph* sceneGraphPtr) -> float
{
    const float original = s_func(sceneGraphPtr);

    // Menu scene graphs and every reduced-distance special case (interiors,
    // worldspaces without LOD) keep vanilla behavior; only the exterior
    // default gets extended.
    if (sceneGraphPtr->menuSceneGraph || original < K_VANILLA_EXTERIOR_FAR_CLIP) {
        return original;
    }

    static std::atomic_bool logged {false};
    if (!logged.exchange(true)) {
        spdlog::info("Extending far clip: {} -> {}", original, ConfigLoader::getFarPlaneDistance());
    }

    return ConfigLoader::getFarPlaneDistance();
}

void GetFarDistanceHook::install()
{
    REL::Relocation<std::uintptr_t> vtbl {RE::VTABLE_SceneGraph[0]};
    GetFarDistanceHook::s_func = vtbl.write_vfunc(GetFarDistanceHook::K_INDEX, GetFarDistanceHook::thunk);

    spdlog::info("Hooked SceneGraph::GetFarDistance (vtable {:#x}, original {:#x})",
                 vtbl.address(),
                 GetFarDistanceHook::s_func.address());
}