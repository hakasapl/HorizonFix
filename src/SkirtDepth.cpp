#include "SkirtDepth.hpp"

#include "WaterSkirt.hpp"

#include <array>
#include <cstdint>

using namespace HorizonFix;

void SkirtDepth::SetupGeometryHook::thunk(RE::BSShader* shaderPtr,
                                          RE::BSRenderPass* passPtr,
                                          std::uint32_t renderFlags)
{
    // Let the water shader do its normal per-geometry setup first
    s_func(shaderPtr, passPtr, renderFlags);

    // Only skirt tiles get the depth hack; every other water pass is untouched
    if (!isSkirtPass(passPtr)) {
        return;
    }
    auto* const context = getContext();
    if (context == nullptr) {
        return;
    }

    // Save the currently bound viewports so Restore can put them back exactly
    std::uint32_t count = K_MAX_VIEWPORTS;
    context->RSGetViewports(&count, s_savedViewports.data());
    if (count == 0) {
        return;
    }

    // Rasterize past the far clip plane; the viewport below decides what
    // depth those fragments clamp to.
    REX::W32::ID3D11RasterizerState* current = nullptr;
    context->RSGetState(&current);
    auto* const noClip = getNoClipState(current);
    if (noClip != nullptr) {
        context->RSSetState(noClip);
    }
    s_savedRasterState = current;
    s_boundNoClip = noClip;

    // Narrow the viewport depth range so clamped beyond-far-plane fragments land
    // at K_BACKDROP_DEPTH: just inside the 1.0 clear value, behind everything else
    auto adjusted = s_savedViewports[0];
    adjusted.maxDepth = K_BACKDROP_DEPTH;
    context->RSSetViewports(1, &adjusted);
    s_adjustedViewport = adjusted;
    s_savedCount = count;
    s_active = true;
}

void SkirtDepth::RestoreGeometryHook::thunk(RE::BSShader* shaderPtr,
                                            RE::BSRenderPass* passPtr,
                                            std::uint32_t renderFlags)
{
    // Undo whatever the matching Setup changed before running the vanilla restore
    if (s_active) {
        if (auto* const context = getContext()) {
            // Restore the saved viewports, but only if ours is still bound —
            // if something else rebound viewports in between, leave theirs alone
            std::uint32_t count = 1;
            REX::W32::D3D11_VIEWPORT current {};
            context->RSGetViewports(&count, &current);
            if (count > 0 && current == s_adjustedViewport) {
                context->RSSetViewports(s_savedCount, s_savedViewports.data());
            }

            // Same idea for the rasterizer state: only swap back if our no-clip
            // state is still the one bound
            REX::W32::ID3D11RasterizerState* boundNow = nullptr;
            context->RSGetState(&boundNow);
            if (s_boundNoClip != nullptr && boundNow == s_boundNoClip) {
                context->RSSetState(s_savedRasterState);
            }
            if (boundNow != nullptr) {
                boundNow->Release(); // RSGetState added a reference
            }
        }

        // Drop the reference RSGetState took in Setup and clear the pair state
        if (s_savedRasterState != nullptr) {
            s_savedRasterState->Release();
            s_savedRasterState = nullptr;
        }
        s_boundNoClip = nullptr;
        s_active = false;
    }

    s_func(shaderPtr, passPtr, renderFlags);
}

auto SkirtDepth::isSkirtPass(RE::BSRenderPass* passPtr) -> bool
{
    // Skirt tiles are identified purely by the name WaterSkirt gave them;
    // BSFixedString comparison is a pointer compare, so this is cheap per pass
    return (passPtr != nullptr) && (passPtr->geometry != nullptr) && passPtr->geometry->name == s_tileName;
}

auto SkirtDepth::getContext() -> REX::W32::ID3D11DeviceContext*
{
    auto* const rendererData = RE::BSGraphics::Renderer::GetRendererData();
    return rendererData != nullptr ? rendererData->context : nullptr;
}

auto SkirtDepth::getNoClipState(REX::W32::ID3D11RasterizerState* currentPtr) -> REX::W32::ID3D11RasterizerState*
{
    // Cache hit: a no-clip copy of this exact source state already exists
    for (std::size_t i = 0; i < s_noClipCount; ++i) {
        if (s_noClipCache.at(i).source == currentPtr) {
            return s_noClipCache.at(i).noClip;
        }
    }

    auto* const device = RE::BSGraphics::Renderer::GetDevice();
    if (device == nullptr) {
        return nullptr;
    }

    // Copy the current state's description (or the D3D11 defaults when nothing
    // is bound), turn off depth clipping, and push the fragments a fixed margin
    // of depth-buffer quanta behind their true depth (see K_SKIRT_DEPTH_BIAS:
    // guarantees the skirt loses against the coplanar real water at any distance,
    // where a world-space height offset cannot)
    REX::W32::D3D11_RASTERIZER_DESC desc {};
    if (currentPtr != nullptr) {
        currentPtr->GetDesc(&desc);
    } else {
        // D3D11 defaults, matching a null bound state.
        desc.fillMode = REX::W32::D3D11_FILL_SOLID;
        desc.cullMode = REX::W32::D3D11_CULL_BACK;
    }
    desc.depthClipEnable = 0;
    desc.depthBias = K_SKIRT_DEPTH_BIAS;
    desc.depthBiasClamp = 0.0F;
    desc.slopeScaledDepthBias = 0.0F;

    REX::W32::ID3D11RasterizerState* created = nullptr;
    if (device->CreateRasterizerState(&desc, &created) < 0 || created == nullptr) {
        return nullptr;
    }

    // Store the new pair; the cache keeps the only reference to the copy
    if (s_noClipCount < s_noClipCache.size()) {
        s_noClipCache.at(s_noClipCount) = NoClipEntry {.source = currentPtr, .noClip = created};
        ++s_noClipCount;
    } else {
        // More distinct water rasterizer states than expected; recycle a slot
        // rather than growing without bound.
        s_noClipCache.at(0).noClip->Release();
        s_noClipCache.at(0) = NoClipEntry {.source = currentPtr, .noClip = created};
    }
    return created;
}

void SkirtDepth::install()
{
    // Intern the tile name once so isSkirtPass can compare by pointer
    s_tileName = WaterSkirt::K_TILE_NAME;

    // Patch both slots on the BSWaterShader vtable; they bracket every water
    // draw, giving us a per-draw window to swap GPU state in and out
    REL::Relocation<std::uintptr_t> vtbl {RE::VTABLE_BSWaterShader[0]};
    SetupGeometryHook::s_func = vtbl.write_vfunc(SetupGeometryHook::K_INDEX, SetupGeometryHook::thunk);
    RestoreGeometryHook::s_func = vtbl.write_vfunc(RestoreGeometryHook::K_INDEX, RestoreGeometryHook::thunk);

    spdlog::info("Hooked BSWaterShader::SetupGeometry/RestoreGeometry (vtable {:#x})", vtbl.address());
}
