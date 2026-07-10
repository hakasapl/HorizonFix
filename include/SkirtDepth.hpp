#pragma once

namespace HorizonFix {

class SkirtDepth {
private:
    static constexpr std::uint32_t K_MAX_VIEWPORTS = 16; // D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE

    // Depth that beyond-far-plane fragments clamp to: eight quanta of a 24-bit
    // depth buffer below the far plane. Strictly closer than the 1.0 clear value
    // so the backdrop passes the depth test in the horizon gap, yet behind
    // everything more than a handful of units inside the far plane. Exactly
    // representable in both D24 and D32F buffers.
    static constexpr float K_BACKDROP_DEPTH = 1.0F - (8.0F / 16777216.0F);

    // State between a Setup/Restore pair; water passes render on a single thread.
    static inline std::array<REX::W32::D3D11_VIEWPORT, K_MAX_VIEWPORTS> s_savedViewports {};
    static inline std::uint32_t s_savedCount = 0;
    static inline REX::W32::D3D11_VIEWPORT s_adjustedViewport {}; // what Setup bound
    static inline REX::W32::ID3D11RasterizerState* s_savedRasterState = nullptr; // ref held via RSGetState
    static inline REX::W32::ID3D11RasterizerState* s_boundNoClip = nullptr; // what Setup bound, no ref held
    static inline bool s_active = false;

    struct NoClipEntry {
        REX::W32::ID3D11RasterizerState* source; // identity only, no ref held
        REX::W32::ID3D11RasterizerState* noClip;
    };
    static inline std::array<NoClipEntry, 8> s_noClipCache {};
    static inline std::size_t s_noClipCount = 0;

    static inline RE::BSFixedString s_tileName;

    struct SetupGeometryHook {
        static void thunk(RE::BSShader* shaderPtr,
                          RE::BSRenderPass* passPtr,
                          std::uint32_t renderFlags);

        static inline REL::Relocation<decltype(thunk)> s_func;
        static constexpr std::size_t K_INDEX = 0x6; // BSShader::SetupGeometry
    };

    struct RestoreGeometryHook {
        static void thunk(RE::BSShader* shaderPtr,
                          RE::BSRenderPass* passPtr,
                          std::uint32_t renderFlags);

        static inline REL::Relocation<decltype(thunk)> s_func;
        static constexpr std::size_t K_INDEX = 0x7; // BSShader::RestoreGeometry
    };

public:
    static void install();

private:
    static auto isSkirtPass(RE::BSRenderPass* passPtr) -> bool;
    static auto getContext() -> REX::W32::ID3D11DeviceContext*;
    static auto getNoClipState(REX::W32::ID3D11RasterizerState* currentPtr) -> REX::W32::ID3D11RasterizerState*;
};

}
