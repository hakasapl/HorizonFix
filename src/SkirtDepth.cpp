#include "SkirtDepth.hpp"

#include "WaterSkirt.hpp"

#include <array>
#include <cstdint>

using namespace HorizonFix;

void SkirtDepth::SetupGeometryHook::thunk(RE::BSShader* shaderPtr,
                                          RE::BSRenderPass* passPtr,
                                          std::uint32_t renderFlags)
{
    s_func(shaderPtr, passPtr, renderFlags);

    if (!isSkirtPass(passPtr)) {
        return;
    }
    auto* const context = getContext();
    if (context == nullptr) {
        return;
    }

    std::uint32_t count = K_MAX_VIEWPORTS;
    context->RSGetViewports(&count, s_savedViewports.data());
    if (count == 0) {
        return;
    }

    // Rasterize past the far clip plane; the viewport below decides what
    // depth those fragments clamp to.
    REX::W32::ID3D11RasterizerState* current = nullptr;
    context->RSGetState(&current);
    if (auto* const noClip = getNoClipState(current)) {
        context->RSSetState(noClip);
    }
    s_savedRasterState = current;

    auto adjusted = s_savedViewports[0];
    adjusted.maxDepth = K_BACKDROP_DEPTH;
    context->RSSetViewports(1, &adjusted);
    s_savedCount = count;
    s_active = true;
}

void SkirtDepth::RestoreGeometryHook::thunk(RE::BSShader* shaderPtr,
                                            RE::BSRenderPass* passPtr,
                                            std::uint32_t renderFlags)
{
    if (s_active) {
        if (auto* const context = getContext()) {
            context->RSSetViewports(s_savedCount, s_savedViewports.data());
            context->RSSetState(s_savedRasterState);
        }
        if (s_savedRasterState != nullptr) {
            s_savedRasterState->Release();
            s_savedRasterState = nullptr;
        }
        s_active = false;
    }

    s_func(shaderPtr, passPtr, renderFlags);
}

auto SkirtDepth::isSkirtPass(RE::BSRenderPass* passPtr) -> bool
{
    return (passPtr != nullptr) && (passPtr->geometry != nullptr) && passPtr->geometry->name == s_tileName;
}

auto SkirtDepth::getContext() -> REX::W32::ID3D11DeviceContext*
{
    auto* const rendererData = RE::BSGraphics::Renderer::GetRendererData();
    return rendererData != nullptr ? rendererData->context : nullptr;
}

auto SkirtDepth::getNoClipState(REX::W32::ID3D11RasterizerState* currentPtr) -> REX::W32::ID3D11RasterizerState*
{
    for (std::size_t i = 0; i < s_noClipCount; ++i) {
        if (s_noClipCache.at(i).source == currentPtr) {
            return s_noClipCache.at(i).noClip;
        }
    }

    auto* const device = RE::BSGraphics::Renderer::GetDevice();
    if (device == nullptr) {
        return nullptr;
    }

    REX::W32::D3D11_RASTERIZER_DESC desc {};
    if (currentPtr != nullptr) {
        currentPtr->GetDesc(&desc);
    } else {
        // D3D11 defaults, matching a null bound state.
        desc.fillMode = REX::W32::D3D11_FILL_SOLID;
        desc.cullMode = REX::W32::D3D11_CULL_BACK;
    }
    desc.depthClipEnable = 0;

    REX::W32::ID3D11RasterizerState* created = nullptr;
    if (device->CreateRasterizerState(&desc, &created) < 0 || created == nullptr) {
        return nullptr;
    }

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
    s_tileName = WaterSkirt::K_TILE_NAME;

    REL::Relocation<std::uintptr_t> vtbl {RE::VTABLE_BSWaterShader[0]};
    SetupGeometryHook::s_func = vtbl.write_vfunc(SetupGeometryHook::K_INDEX, SetupGeometryHook::thunk);
    RestoreGeometryHook::s_func = vtbl.write_vfunc(RestoreGeometryHook::K_INDEX, RestoreGeometryHook::thunk);

    spdlog::info("Hooked BSWaterShader::SetupGeometry/RestoreGeometry (vtable {:#x})", vtbl.address());
}
