#pragma once

namespace HorizonFix::SkirtCull {

struct AtmosphereUpdateHook {
    static void thunk(RE::Atmosphere* atmospherePtr,
                      RE::Sky* skyPtr,
                      float deltaTime);

    static void install();

    static inline REL::Relocation<decltype(thunk)> s_func;
    static constexpr std::size_t K_INDEX = 0x3; // SkyObject::Update
};

}
