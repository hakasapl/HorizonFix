#pragma once

#include "PCH.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace HorizonFix {

/**
 * @brief Depth hack for water skirt tiles. The skirt is a set of cull-proof tiles that extend the water mesh beyond the
 * far clip plane, so the horizon gap is filled with water.
 *
 * Skirt geometry lies past the far clip plane, so by default the rasterizer would clip every
 * fragment. This class hooks BSWaterShader::SetupGeometry/RestoreGeometry and, for skirt passes
 * only, (1) swaps in a rasterizer state with depth clipping disabled and a constant depth bias
 * (K_SKIRT_DEPTH_BIAS, so coplanar real water always beats the skirt at any distance), and
 * (2) narrows the viewport depth range so clamped beyond-far fragments land at K_BACKDROP_DEPTH.
 * The result: skirt pixels pass the depth test only where nothing else rendered (the horizon
 * gap) and never overdraw real geometry - including the game's own water at the same height.
 */
class SkirtDepth {
private:
    //
    // Constants
    //
    static constexpr std::uint32_t K_MAX_VIEWPORTS = 16; // D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE
    static constexpr std::size_t K_NOCLIP_CACHE_SIZE
        = 8; /**< Max distinct rasterizer states to keep no-clip copies of */

    // Depth that beyond-far-plane fragments clamp to: eight quanta of a 24-bit
    // depth buffer below the far plane. Strictly closer than the 1.0 clear value
    // so the backdrop passes the depth test in the horizon gap, yet behind
    // everything more than a handful of units inside the far plane. Exactly
    // representable in both D24 and D32F buffers.
    static constexpr float K_BACKDROP_DEPTH = 1.0F - (8.0F / 16777216.0F);

    // Constant rasterizer depth bias for skirt passes, in depth-buffer quanta.
    // Skirt tiles are coplanar with the game's real water around the player; both
    // depth-test against each other, and two identical planes land within rounding
    // of the same depth, so which one wins varies per pixel and per frame
    // (flicker). A world-space height offset cannot fix this at range - any fixed
    // dip eventually shrinks below depth precision at distance (field-observed
    // from high viewpoints) and a larger dip shows as a seam. A bias in DEPTH
    // UNITS pushes every skirt fragment a guaranteed margin behind its true depth
    // at every distance, so the skirt reliably loses against coplanar water while
    // still winning the empty horizon (bias can only make the skirt lose more,
    // never win wrongly). 256 quanta ~= 1.5e-5 depth: far above coplanar rounding,
    // far below any real geometric separation.
    static constexpr std::int32_t K_SKIRT_DEPTH_BIAS = 256;

    // State between a Setup/Restore pair; water passes render on a single thread.
    static inline std::array<REX::W32::D3D11_VIEWPORT, K_MAX_VIEWPORTS>
        s_savedViewports {}; /**< Viewports bound before Setup adjusted them */
    static inline std::uint32_t s_savedCount = 0; /**< How many viewports were saved */
    static inline REX::W32::D3D11_VIEWPORT s_adjustedViewport {}; // what Setup bound
    static inline REX::W32::ID3D11RasterizerState* s_savedRasterState = nullptr; // ref held via RSGetState
    static inline REX::W32::ID3D11RasterizerState* s_boundNoClip = nullptr; // what Setup bound, no ref held
    static inline bool s_active = false; /**< True between a skirt Setup and its Restore */

    /**
     * @brief Cache entry mapping an engine rasterizer state to its no-depth-clip copy
     */
    struct NoClipEntry {
        REX::W32::ID3D11RasterizerState* source; // identity only, no ref held
        REX::W32::ID3D11RasterizerState* noClip; /**< Copy of source with depthClipEnable = 0; ref held by the cache */
    };
    static inline std::array<NoClipEntry, K_NOCLIP_CACHE_SIZE> s_noClipCache {};
    static inline std::size_t s_noClipCount = 0; /**< Number of populated cache entries */

    static inline RE::BSFixedString s_tileName; /**< Interned WaterSkirt::K_TILE_NAME for cheap per-pass comparison */

    /**
     * @brief Hook for BSWaterShader::SetupGeometry: applies the no-clip state and depth-clamped
     * viewport before a skirt tile draws
     */
    struct SetupGeometryHook {
        static void thunk(RE::BSShader* shaderPtr,
                          RE::BSRenderPass* passPtr,
                          std::uint32_t renderFlags);

        static inline REL::Relocation<decltype(thunk)> s_func; /**< Original function, called by the thunk */
        static constexpr std::size_t K_INDEX = 0x6; // BSShader::SetupGeometry
    };

    /**
     * @brief Hook for BSWaterShader::RestoreGeometry: puts the saved viewport and rasterizer state
     * back after a skirt tile draws
     */
    struct RestoreGeometryHook {
        static void thunk(RE::BSShader* shaderPtr,
                          RE::BSRenderPass* passPtr,
                          std::uint32_t renderFlags);

        static inline REL::Relocation<decltype(thunk)> s_func; /**< Original function, called by the thunk */
        static constexpr std::size_t K_INDEX = 0x7; // BSShader::RestoreGeometry
    };

public:
    /**
     * @brief Installs both hooks on the BSWaterShader vtable
     */
    static void install();

private:
    /**
     * @brief Whether a render pass draws a skirt tile (matched by geometry name)
     *
     * @param passPtr Render pass to inspect
     * @return true If the pass geometry is a skirt tile
     * @return false Otherwise
     */
    static auto isSkirtPass(RE::BSRenderPass* passPtr) -> bool;

    /**
     * @brief Get the renderer's immediate D3D11 device context
     *
     * @return REX::W32::ID3D11DeviceContext* The context, or nullptr if the renderer is not up
     */
    static auto getContext() -> REX::W32::ID3D11DeviceContext*;

    /**
     * @brief Returns a copy of the given rasterizer state with depth clipping disabled
     *
     * Copies are created once per distinct source state and cached (see s_noClipCache).
     *
     * @param currentPtr The rasterizer state currently bound (may be nullptr for D3D defaults)
     * @return REX::W32::ID3D11RasterizerState* The cached no-clip state, or nullptr on failure
     */
    static auto getNoClipState(REX::W32::ID3D11RasterizerState* currentPtr) -> REX::W32::ID3D11RasterizerState*;
};

}
