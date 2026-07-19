#pragma once

#include "PCH.h"

#include <cstddef>

namespace HorizonFix::SkirtCull {

/**
 * @brief Hook for the Atmosphere::Update function
 *
 * Atmosphere::Update runs exactly once per rendered frame on the main thread, which makes it a
 * convenient tick to drive WaterSkirt::updateVisibility (the skirt's manual frustum culling).
 */
struct AtmosphereUpdateHook {
    /**
     * @brief Replacement vfunc: runs the vanilla update, then refreshes skirt tile visibility
     *
     * @param atmospherePtr The Atmosphere sky object being updated
     * @param skyPtr The owning Sky instance
     * @param deltaTime Frame delta time passed by the engine
     */
    static void thunk(RE::Atmosphere* atmospherePtr,
                      RE::Sky* skyPtr,
                      float deltaTime);

    /**
     * @brief Installs AtmosphereUpdateHook by patching the Atmosphere vtable
     */
    static void install();

    static inline REL::Relocation<decltype(thunk)> s_func; /**< Original function, called by the thunk */
    static constexpr std::size_t K_INDEX = 0x3; /**< vtable slot of SkyObject::Update, which Atmosphere overrides */
};

} // namespace HorizonFix::SkirtCull
