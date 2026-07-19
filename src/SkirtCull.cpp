#include "SkirtCull.hpp"

#include "WaterSkirt.hpp"

#include "PCH.h"

#include <spdlog/spdlog.h>

#include <cstdint>

using namespace HorizonFix::SkirtCull;

void AtmosphereUpdateHook::thunk(RE::Atmosphere* atmospherePtr,
                                 RE::Sky* skyPtr,
                                 float deltaTime)
{
    // Run the vanilla Atmosphere::Update function
    s_func(atmospherePtr, skyPtr, deltaTime);

    // Re-run the skirt's manual frustum culling; the tiles are cull-proof to the engine,
    // so this per-frame pass is the only thing hiding off-screen tiles
    WaterSkirt::updateVisibility();
}

void AtmosphereUpdateHook::install()
{
    // Overwrite the Update slot in the Atmosphere vtable; the engine calls it once per frame,
    // and the thunk chains to the saved original so vanilla behavior is preserved
    REL::Relocation<std::uintptr_t> vtbl {RE::VTABLE_Atmosphere.at(0)};
    s_func = vtbl.write_vfunc(K_INDEX, thunk);

    spdlog::info("Hooked Atmosphere::Update (vtable {:#x}, original {:#x})", vtbl.address(), s_func.address());
}
