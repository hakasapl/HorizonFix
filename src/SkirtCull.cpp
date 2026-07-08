#include "SkirtCull.hpp"

#include "WaterSkirt.hpp"

using namespace HorizonFix::SkirtCull;

void AtmosphereUpdateHook::thunk(RE::Atmosphere* atmospherePtr,
                                 RE::Sky* skyPtr,
                                 float deltaTime)
{
    s_func(atmospherePtr, skyPtr, deltaTime);
    WaterSkirt::updateVisibility();
}

void AtmosphereUpdateHook::install()
{
    REL::Relocation<std::uintptr_t> vtbl {RE::VTABLE_Atmosphere[0]};
    s_func = vtbl.write_vfunc(K_INDEX, thunk);

    spdlog::info("Hooked Atmosphere::Update (vtable {:#x}, original {:#x})", vtbl.address(), s_func.address());
}
