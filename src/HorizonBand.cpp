#include "HorizonBand.hpp"

#include "ConfigLoader.hpp"
#include "WaterSkirt.hpp"

#include "PCH.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <numbers>
#include <vector>

using namespace HorizonFix;

namespace {

/**
 * @brief One ring vertex in the band's layout: full-precision position, UV, color
 *
 * Must match K_RING_VERTEX_DESC below (and the descriptor the donor NIF declares): position
 * as four floats (the fourth is the unused bitangent-X slot of layouts without tangents),
 * one half-precision UV pair, and a byte RGBA color carrying the alpha gradient.
 */
struct RingVertex {
    float x;
    float y;
    float z;
    float unusedW;
    std::uint16_t u;
    std::uint16_t v;
    std::array<std::uint8_t, 4> rgba;
};
static_assert(sizeof(RingVertex) == 24);

// BSVertexDesc for the ring: stride 6 dwords, UV at dword 4, color at dword 5, attribute
// flags VF_VERTEX | VF_UV | VF_COLORS | VF_FULLPREC (nibble-packed engine convention:
// bits 0-3 stride in dwords, per-attribute dword offsets in the following nibbles, the
// attribute flag word at bit 44).
constexpr std::uint64_t K_RING_VERTEX_DESC = (0x423ULL << 44U) | (5ULL << 24U) | (4ULL << 8U) | 6ULL;

} // namespace

auto HorizonBand::isBandPass(RE::BSRenderPass* passPtr) -> bool
{
    // The band is identified purely by the name the donor NIF gave it; BSFixedString
    // comparison is a pointer compare, so this is cheap per pass
    return (passPtr != nullptr) && (passPtr->geometry != nullptr) && passPtr->geometry->name == s_bandName;
}

auto HorizonBand::getContext() -> REX::W32::ID3D11DeviceContext*
{
    auto* const renderer = RE::BSGraphics::Renderer::GetSingleton();
    return renderer != nullptr ? renderer->GetRuntimeData().context : nullptr;
}

void HorizonBand::refreshArcColors(RE::BSTriShape* arcShape,
                                   std::size_t arcIndex)
{
    const std::uint32_t stamp = s_tintStamp.load(std::memory_order_acquire);
    if (s_arcTintStamps.at(arcIndex) == stamp) {
        return; // this arc's buffer already carries the current gradient
    }

    auto* const data = arcShape->GetGeometryRuntimeData().rendererData;
    if (data == nullptr || data->rawVertexData == nullptr || data->vertexBuffer == nullptr) {
        return;
    }
    auto* const context = getContext();
    if (context == nullptr) {
        return;
    }

    // Rewrite the RGB bytes of every vertex in the CPU copy from the band color; the
    // alpha byte keeps carrying the opacity profile untouched
    constexpr std::uint32_t COLUMNS = (K_SEGMENTS / K_ARCS) + 1;
    constexpr std::uint32_t VERTEX_COUNT = COLUMNS * K_ROWS;
    const auto toByte = [](float channel) -> std::uint8_t {
        return static_cast<std::uint8_t>(std::lround(std::clamp(channel, 0.0F, 1.0F) * 255.0F));
    };
    const RE::NiColor color = s_bandColor;
    const std::uint8_t red = toByte(color.red);
    const std::uint8_t green = toByte(color.green);
    const std::uint8_t blue = toByte(color.blue);
    for (std::uint32_t vertex = 0; vertex < VERTEX_COUNT; ++vertex) {
        std::uint8_t* const colorBytes = data->rawVertexData + (static_cast<std::size_t>(vertex) * sizeof(RingVertex))
            + offsetof(RingVertex, rgba);
        colorBytes[0] = red;
        colorBytes[1] = green;
        colorBytes[2] = blue;
    }

    // Full-buffer discard upload; the buffer object identity is unchanged, so nothing
    // downstream needs rebinding
    auto* const buffer = reinterpret_cast<REX::W32::ID3D11Buffer*>(data->vertexBuffer);
    REX::W32::D3D11_MAPPED_SUBRESOURCE mapped {};
    if (context->Map(buffer, 0, REX::W32::D3D11_MAP_WRITE_DISCARD, 0, &mapped) < 0 || mapped.data == nullptr) {
        return;
    }
    std::memcpy(mapped.data, data->rawVertexData, static_cast<std::size_t>(VERTEX_COUNT) * sizeof(RingVertex));
    context->Unmap(buffer, 0);
    s_arcTintStamps.at(arcIndex) = stamp;
}

void HorizonBand::computeSampleUVs(const RE::NiCamera* camera,
                                   float scale,
                                   float bandZ,
                                   float halfHeightWorld)
{
    // One projection cannot serve two VR eyes; the loop stands down there and the
    // fog-far prior alone drives the band
    if (REL::Module::IsVR()) {
        s_samplesValid.store(false, std::memory_order_release);
        return;
    }

    const auto& world = camera->world;
    const auto& frustum = camera->GetRuntimeData2().viewFrustum;
    const RE::NiPoint3 forward {world.rotate.entry[0][0], world.rotate.entry[1][0], world.rotate.entry[2][0]};
    const RE::NiPoint3 up {world.rotate.entry[0][1], world.rotate.entry[1][1], world.rotate.entry[2][1]};
    const RE::NiPoint3 right {world.rotate.entry[0][2], world.rotate.entry[1][2], world.rotate.entry[2][2]};

    const float horizontalWidth = frustum.fRight - frustum.fLeft;
    const float verticalHeight = frustum.fTop - frustum.fBottom;
    RE::NiPoint3 forwardHorizontal {forward.x, forward.y, 0.0F};
    const float forwardLength = forwardHorizontal.Length();
    bool anyValid = false;
    if (frustum.bOrtho || horizontalWidth <= 0.0F || verticalHeight <= 0.0F || forwardLength < 0.05F) {
        // Looking straight up/down (or an exotic camera): nothing sensible to sample
        for (auto& sample : s_sampleUV) {
            sample.at(0) = -1.0F;
        }
        s_samplesValid.store(false, std::memory_order_release);
        return;
    }
    forwardHorizontal /= forwardLength;

    // Rays fan out around the camera's horizontal heading on the band row just below the
    // seam, inside the band's strong-alpha zone, where the framebuffer under the band is
    // pure water
    const float sampleZOffset = (bandZ + (K_MATCH_ROW_T * halfHeightWorld)) - world.translate.z;
    for (int i = 0; i < K_MATCH_SAMPLES; ++i) {
        auto& sample = s_sampleUV.at(static_cast<std::size_t>(i));
        sample.at(0) = -1.0F;
        const float azimuth = static_cast<float>(i - ((K_MATCH_SAMPLES - 1) / 2)) * K_MATCH_AZIMUTH_STEP
            * std::numbers::pi_v<float> / 180.0F;
        const float sinAz = std::sin(azimuth);
        const float cosAz = std::cos(azimuth);
        const RE::NiPoint3 toSample {((cosAz * forwardHorizontal.x) - (sinAz * forwardHorizontal.y)) * scale,
                                     ((sinAz * forwardHorizontal.x) + (cosAz * forwardHorizontal.y)) * scale,
                                     sampleZOffset};
        const float depth = toSample.Dot(forward);
        if (depth < 1.0F) {
            continue;
        }
        const float u = ((toSample.Dot(right) / depth) - frustum.fLeft) / horizontalWidth;
        const float v = (frustum.fTop - (toSample.Dot(up) / depth)) / verticalHeight;
        // Keep a safety margin from the viewport edges (dynamic resolution, TAA jitter)
        constexpr float MARGIN = 0.03F;
        if (u < MARGIN || u > 1.0F - MARGIN || v < MARGIN || v > 1.0F - MARGIN) {
            continue;
        }
        sample.at(0) = u;
        sample.at(1) = v;
        anyValid = true;
    }
    s_samplesValid.store(anyValid, std::memory_order_release);
}

void HorizonBand::disableMatch(const char* reason)
{
    if (!s_matchDisabled) {
        s_matchDisabled = true;
        spdlog::info("Horizon blend color matching disabled: {} (keeping the fog-far tint as-is)", reason);
    }
}

auto HorizonBand::texelSize(std::uint32_t format) -> std::uint32_t
{
    switch (format) {
    case REX::W32::DXGI_FORMAT_R8G8B8A8_UNORM:
    case REX::W32::DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case REX::W32::DXGI_FORMAT_B8G8R8A8_UNORM:
    case REX::W32::DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case REX::W32::DXGI_FORMAT_R11G11B10_FLOAT:
    case REX::W32::DXGI_FORMAT_R10G10B10A2_UNORM:
        return 4;
    case REX::W32::DXGI_FORMAT_R16G16B16A16_FLOAT:
        return 8;
    default:
        return 0;
    }
}

auto HorizonBand::decodeTexel(const std::uint8_t* texel,
                              std::uint32_t format,
                              RE::NiColor& out) -> bool
{
    // The loop only needs a consistent difference signal, so sRGB-encoded values are
    // read as-is rather than linearized - the integral controller nulls the difference
    // in whatever space both rows share
    constexpr float BYTE_SCALE = 1.0F / 255.0F;
    switch (format) {
    case REX::W32::DXGI_FORMAT_R8G8B8A8_UNORM:
    case REX::W32::DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        out = RE::NiColor {static_cast<float>(texel[0]) * BYTE_SCALE,
                           static_cast<float>(texel[1]) * BYTE_SCALE,
                           static_cast<float>(texel[2]) * BYTE_SCALE};
        return true;
    case REX::W32::DXGI_FORMAT_B8G8R8A8_UNORM:
    case REX::W32::DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        out = RE::NiColor {static_cast<float>(texel[2]) * BYTE_SCALE,
                           static_cast<float>(texel[1]) * BYTE_SCALE,
                           static_cast<float>(texel[0]) * BYTE_SCALE};
        return true;
    case REX::W32::DXGI_FORMAT_R16G16B16A16_FLOAT: {
        std::array<std::uint16_t, 3> halves {};
        std::memcpy(halves.data(), texel, sizeof(halves));
        out = RE::NiColor {WaterSkirt::halfToFloat(halves.at(0)),
                           WaterSkirt::halfToFloat(halves.at(1)),
                           WaterSkirt::halfToFloat(halves.at(2))};
        return true;
    }
    case REX::W32::DXGI_FORMAT_R11G11B10_FLOAT: {
        std::uint32_t packed = 0;
        std::memcpy(&packed, texel, sizeof(packed));
        // 5-bit exponent (bias 15) with 6/6/5-bit mantissas, no sign
        const auto decodeSmallFloat = [](std::uint32_t bits, std::uint32_t mantissaBits) -> float {
            const std::uint32_t mantissaMask = (1U << mantissaBits) - 1U;
            const auto exponent = static_cast<std::int32_t>((bits >> mantissaBits) & 0x1FU);
            const float mantissa = static_cast<float>(bits & mantissaMask) / static_cast<float>(1U << mantissaBits);
            if (exponent == 0) {
                return mantissa * std::exp2(-14.0F);
            }
            if (exponent == 31) {
                return 0.0F; // treat inf/NaN as no signal
            }
            return (1.0F + mantissa) * std::exp2(static_cast<float>(exponent - 15));
        };
        out = RE::NiColor {decodeSmallFloat(packed & 0x7FFU, 6),
                           decodeSmallFloat((packed >> 11U) & 0x7FFU, 6),
                           decodeSmallFloat((packed >> 22U) & 0x3FFU, 5)};
        return true;
    }
    case REX::W32::DXGI_FORMAT_R10G10B10A2_UNORM: {
        std::uint32_t packed = 0;
        std::memcpy(&packed, texel, sizeof(packed));
        constexpr float TEN_BIT_SCALE = 1.0F / 1023.0F;
        out = RE::NiColor {static_cast<float>(packed & 0x3FFU) * TEN_BIT_SCALE,
                           static_cast<float>((packed >> 10U) & 0x3FFU) * TEN_BIT_SCALE,
                           static_cast<float>((packed >> 20U) & 0x3FFU) * TEN_BIT_SCALE};
        return true;
    }
    default:
        return false;
    }
}

void HorizonBand::captureMatchSamples(bool post)
{
    if (s_matchDisabled || !s_samplesValid.load(std::memory_order_acquire)) {
        return;
    }
    auto* const context = getContext();
    if (context == nullptr) {
        return;
    }

    // The render target bound for this band draw is where the water (and afterwards the
    // band) pixels live
    REX::W32::ID3D11RenderTargetView* rtv = nullptr;
    context->OMGetRenderTargets(1, &rtv, nullptr);
    if (rtv == nullptr) {
        return;
    }
    REX::W32::ID3D11Resource* resource = nullptr;
    rtv->GetResource(&resource);
    rtv->Release();
    if (resource == nullptr) {
        return;
    }
    auto* const sourceTexture = reinterpret_cast<REX::W32::ID3D11Texture2D*>(resource);
    REX::W32::D3D11_TEXTURE2D_DESC sourceDesc {};
    sourceTexture->GetDesc(&sourceDesc);

    // Auxiliary renders (cubemap faces and other small targets) are skipped, not
    // disabled; MSAA cannot be region-copied to staging and disables the loop
    if (sourceDesc.sampleDesc.count > 1) {
        disableMatch("multisampled render target");
        resource->Release();
        return;
    }
    if (sourceDesc.width < 1000) {
        resource->Release();
        return;
    }
    if (texelSize(static_cast<std::uint32_t>(sourceDesc.format)) == 0) {
        disableMatch("unsupported render target format");
        resource->Release();
        return;
    }

    // Recreate the staging ring when the render target format changes
    if (s_matchFormat != static_cast<std::uint32_t>(sourceDesc.format)) {
        for (auto& staleSlot : s_matchSlots) {
            if (staleSlot.staging != nullptr) {
                staleSlot.staging->Release();
            }
            staleSlot = MatchSlot {};
        }
        s_matchFormat = static_cast<std::uint32_t>(sourceDesc.format);
    }

    const std::uint32_t frame = s_matchCaptureFrame;
    auto& slot = s_matchSlots.at(frame % K_MATCH_RING);
    if (post && (slot.frame != frame || !slot.hasPre)) {
        resource->Release();
        return; // no pre capture to pair with this frame
    }
    if (slot.staging == nullptr) {
        auto* const device = RE::BSGraphics::Renderer::GetDevice();
        if (device == nullptr) {
            resource->Release();
            return;
        }
        REX::W32::D3D11_TEXTURE2D_DESC stagingDesc {};
        stagingDesc.width = K_MATCH_SAMPLES;
        stagingDesc.height = 2;
        stagingDesc.mipLevels = 1;
        stagingDesc.arraySize = 1;
        stagingDesc.format = sourceDesc.format;
        stagingDesc.sampleDesc.count = 1;
        stagingDesc.usage = REX::W32::D3D11_USAGE_STAGING;
        stagingDesc.cpuAccessFlags = REX::W32::D3D11_CPU_ACCESS_READ;
        if (device->CreateTexture2D(&stagingDesc, nullptr, &slot.staging) < 0 || slot.staging == nullptr) {
            slot.staging = nullptr;
            resource->Release();
            return;
        }
    }

    // Viewport-relative pixel coordinates handle dynamic resolution
    std::uint32_t viewportCount = 1;
    REX::W32::D3D11_VIEWPORT viewport {};
    context->RSGetViewports(&viewportCount, &viewport);
    if (viewportCount == 0 || viewport.width < 1.0F || viewport.height < 1.0F) {
        resource->Release();
        return;
    }

    if (!post) {
        slot.uv = s_sampleUV; // freeze the points for this capture pair and its readback
    }
    for (int i = 0; i < K_MATCH_SAMPLES; ++i) {
        const float u = slot.uv.at(i).at(0);
        const float v = slot.uv.at(i).at(1);
        if (u < 0.0F) {
            continue;
        }
        const auto px = static_cast<std::uint32_t>(
            std::clamp(viewport.topLeftX + (u * viewport.width), viewport.topLeftX, viewport.topLeftX + viewport.width - 1.0F));
        const auto py = static_cast<std::uint32_t>(
            std::clamp(viewport.topLeftY + (v * viewport.height), viewport.topLeftY, viewport.topLeftY + viewport.height - 1.0F));
        const REX::W32::D3D11_BOX box {px, py, 0, px + 1, py + 1, 1};
        context->CopySubresourceRegion(
            slot.staging, 0, static_cast<std::uint32_t>(i), post ? 1 : 0, 0, resource, 0, &box);
    }
    if (post) {
        slot.hasPost = true;
    } else {
        slot.frame = frame;
        slot.hasPre = true;
        slot.hasPost = false;
    }
    resource->Release();
}

void HorizonBand::consumeMatchSlot(std::uint32_t frame)
{
    if (s_matchDisabled) {
        return;
    }
    // The slot the ring is about to recycle next frame is the oldest completed capture
    auto& slot = s_matchSlots.at((frame + 1) % K_MATCH_RING);
    if (slot.staging == nullptr || !slot.hasPre || !slot.hasPost
        || slot.frame != frame - (K_MATCH_RING - 1)) {
        return;
    }
    auto* const context = getContext();
    if (context == nullptr) {
        return;
    }

    // The copy is K_MATCH_RING-1 frames old, so this map should never block; if the GPU
    // is somehow still busy, skip rather than stall the frame
    REX::W32::D3D11_MAPPED_SUBRESOURCE mapped {};
    if (context->Map(slot.staging, 0, REX::W32::D3D11_MAP_READ, REX::W32::D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped) < 0
        || mapped.data == nullptr) {
        return;
    }

    const std::uint32_t stride = texelSize(s_matchFormat);
    const auto* const preRow = static_cast<const std::uint8_t*>(mapped.data);
    const auto* const postRow = preRow + mapped.rowPitch;
    std::array<float, 3> errorSum {};
    std::array<float, 3> preSum {};
    int used = 0;
    for (int i = 0; i < K_MATCH_SAMPLES; ++i) {
        if (slot.uv.at(i).at(0) < 0.0F) {
            continue;
        }
        RE::NiColor pre;
        RE::NiColor postColor;
        if (!decodeTexel(preRow + (static_cast<std::size_t>(i) * stride), s_matchFormat, pre)
            || !decodeTexel(postRow + (static_cast<std::size_t>(i) * stride), s_matchFormat, postColor)) {
            continue;
        }
        errorSum.at(0) += pre.red - postColor.red;
        errorSum.at(1) += pre.green - postColor.green;
        errorSum.at(2) += pre.blue - postColor.blue;
        preSum.at(0) += pre.red;
        preSum.at(1) += pre.green;
        preSum.at(2) += pre.blue;
        ++used;
    }
    context->Unmap(slot.staging, 0);
    slot.hasPre = false;
    slot.hasPost = false;

    // Integrate the observed on-screen difference into the tint. Occluded samples
    // contribute zero (the band never drew there), so they only dilute the gain, never
    // bias the color.
    if (used > 0) {
        for (int channel = 0; channel < 3; ++channel) {
            const float error = errorSum.at(channel) / static_cast<float>(used);
            const float reference = std::max(preSum.at(channel) / static_cast<float>(used), 0.05F);
            auto& correction = s_waterCorrection.at(channel);
            const float updated
                = std::clamp(correction.load(std::memory_order_relaxed) + (K_MATCH_GAIN * error / reference),
                             K_MATCH_MIN,
                             K_MATCH_MAX);
            correction.store(updated, std::memory_order_relaxed);
        }
    }

    // Periodic diagnostic so field reports carry the loop's state (roughly every 10s)
    static std::uint32_t consumedCount = 0;
    if (++consumedCount % 600 == 0) {
        spdlog::info("Horizon blend match: water corr ({:.3f}, {:.3f}, {:.3f}) x{}",
                     s_waterCorrection.at(0).load(std::memory_order_relaxed),
                     s_waterCorrection.at(1).load(std::memory_order_relaxed),
                     s_waterCorrection.at(2).load(std::memory_order_relaxed),
                     used);
    }
}

void HorizonBand::SetupGeometryHook::thunk(RE::BSShader* shaderPtr,
                                           RE::BSRenderPass* passPtr,
                                           std::uint32_t renderFlags)
{
    // Let the effect shader do its normal per-geometry setup first
    s_func(shaderPtr, passPtr, renderFlags);

    // Only band passes get the special handling; every other effect pass is untouched.
    // Done here rather than in updateFrame because mapping GPU resources must happen on
    // the thread that owns the D3D context - the thread this hook runs on.
    if (!isBandPass(passPtr)) {
        return;
    }
    for (std::size_t i = 0; i < s_arcs.size(); ++i) {
        if (s_arcs[i].get() == passPtr->geometry) {
            refreshArcColors(s_arcs[i].get(), i);
            break;
        }
    }

    // Once per frame, before the first band arc draws: the framebuffer still shows pure
    // water under the band - capture it, and harvest the capture pair from 3 frames ago
    const std::uint32_t frame = s_tintStamp.load(std::memory_order_acquire);
    if (frame != s_matchCaptureFrame) {
        s_matchCaptureFrame = frame;
        captureMatchSamples(false);
        consumeMatchSlot(frame);
    }
}

void HorizonBand::RestoreGeometryHook::thunk(RE::BSShader* shaderPtr,
                                             RE::BSRenderPass* passPtr,
                                             std::uint32_t renderFlags)
{
    // After a band arc drew, the framebuffer shows the band's blend at the sample
    // points; every arc overwrites the row, so the last arc's state wins
    if (isBandPass(passPtr)) {
        captureMatchSamples(true);
    }
    s_func(shaderPtr, passPtr, renderFlags);
}

void HorizonBand::installHooks()
{
    // Intern the band name once so isBandPass can compare by pointer
    s_bandName = K_SHAPE_NAME;

    REL::Relocation<std::uintptr_t> vtbl {RE::VTABLE_BSEffectShader.at(0)};
    SetupGeometryHook::s_func = vtbl.write_vfunc(SetupGeometryHook::K_INDEX, SetupGeometryHook::thunk);
    RestoreGeometryHook::s_func = vtbl.write_vfunc(RestoreGeometryHook::K_INDEX, RestoreGeometryHook::thunk);

    spdlog::info("Hooked BSEffectShader::SetupGeometry/RestoreGeometry for the horizon band tint (vtable {:#x})",
                 vtbl.address());
}

auto HorizonBand::floatToHalf(float value) -> std::uint16_t
{
    // Truncating float-to-half for the UV channel; band UVs stay in [0, 1]
    const auto bits = std::bit_cast<std::uint32_t>(value);
    const std::uint32_t sign = (bits >> 16U) & 0x8000U;
    const auto exponent = static_cast<std::int32_t>((bits >> 23U) & 0xFFU) - 127 + 15;
    const std::uint32_t mantissa = bits & 0x7FFFFFU;
    if (exponent <= 0) {
        return static_cast<std::uint16_t>(sign); // flush tiny values to signed zero
    }
    if (exponent >= 31) {
        return static_cast<std::uint16_t>(sign | 0x7C00U); // overflow to infinity
    }
    return static_cast<std::uint16_t>(sign | (static_cast<std::uint32_t>(exponent) << 10U) | (mantissa >> 13U));
}

void HorizonBand::releaseGeometryData(RE::BSGraphics::TriShape* dataPtr)
{
    if (dataPtr == nullptr) {
        return;
    }

    // Mirror of the engine's release path: the last reference Releases the D3D
    // buffers and frees the CPU copies and the struct through the game allocator
    if (_InterlockedDecrement(reinterpret_cast<volatile long*>(&dataPtr->refCount)) == 0) {
        if (dataPtr->vertexBuffer != nullptr) {
            reinterpret_cast<REX::W32::ID3D11Buffer*>(dataPtr->vertexBuffer)->Release();
        }
        if (dataPtr->indexBuffer != nullptr) {
            reinterpret_cast<REX::W32::ID3D11Buffer*>(dataPtr->indexBuffer)->Release();
        }
        if (dataPtr->rawVertexData != nullptr) {
            RE::free(dataPtr->rawVertexData);
        }
        if (dataPtr->rawIndexData != nullptr) {
            RE::free(dataPtr->rawIndexData);
        }
        RE::free(dataPtr);
    }
}

auto HorizonBand::buildArcGeometry(int arcIndex,
                                   float blendDegrees,
                                   float opaqueFraction,
                                   RE::NiBound& boundOut) -> RE::BSGraphics::TriShape*
{
    auto* const device = RE::BSGraphics::Renderer::GetDevice();
    if (device == nullptr) {
        return nullptr;
    }

    // Unit-radius cylinder wall: half-height tan(angle) so the blend seen from the camera
    // spans exactly the configured angle above and below the seam line at any world scale
    // (radius and height stay proportional under the uniform per-frame scale)
    const float halfHeight = std::tan(blendDegrees * std::numbers::pi_v<float> / 180.0F);

    constexpr std::uint32_t ARC_SEGMENTS = K_SEGMENTS / K_ARCS;
    constexpr std::uint32_t COLUMNS = ARC_SEGMENTS + 1; // shared edge column keeps arcs seamless
    constexpr std::uint32_t VERTEX_COUNT = COLUMNS * K_ROWS;
    constexpr std::uint32_t TRIANGLE_COUNT = ARC_SEGMENTS * (K_ROWS - 1) * 2;
    static_assert(VERTEX_COUNT <= 0xFFFF, "arc must stay indexable with 16-bit indices");

    RE::NiPoint3 minPos {std::numeric_limits<float>::max(),
                         std::numeric_limits<float>::max(),
                         std::numeric_limits<float>::max()};
    RE::NiPoint3 maxPos {std::numeric_limits<float>::lowest(),
                         std::numeric_limits<float>::lowest(),
                         std::numeric_limits<float>::lowest()};
    std::vector<RingVertex> vertices;
    vertices.reserve(VERTEX_COUNT);
    // Row placement and alpha profile. The row parameter t runs from -1 (bottom rim) to +1
    // (top rim) with the center row exactly on the seam (t = 0). Below the seam the rows
    // are uniform and the alpha smoothsteps from 0 at the rim to 1 at the seam: the band
    // there paints the far water's color over nearer, less fogged water, and that soft
    // fade-in is what keeps its lower edge from drawing a line of its own. Above the seam
    // the alpha stays at 1 up to the configured share of the upper half (so a thin bright
    // sky strip sitting right on the horizon line is covered outright), then smoothsteps
    // to 0 at the top rim over the remaining height. With a plateau the upper rows are
    // re-spaced so one row lands exactly on the plateau's end and the rest resolve the
    // fade; without one they stay uniform like the lower half.
    constexpr std::uint32_t SEAM_ROW = (K_ROWS - 1) / 2;
    constexpr std::uint32_t UPPER_ROWS = K_ROWS - 1 - SEAM_ROW; // rows strictly above the seam
    const float plateau = std::clamp(opaqueFraction, 0.0F, 1.0F);
    const auto smoothstep = [](float x) -> float { return x * x * (3.0F - (2.0F * x)); };
    for (std::uint32_t row = 0; row < K_ROWS; ++row) {
        float t = 0.0F;
        float alpha = 1.0F;
        if (row <= SEAM_ROW) {
            t = (static_cast<float>(row) / static_cast<float>(SEAM_ROW)) - 1.0F;
            alpha = smoothstep(1.0F + t);
        } else {
            const std::uint32_t k = row - SEAM_ROW; // 1 .. UPPER_ROWS
            if (plateau <= 0.0F) {
                t = static_cast<float>(k) / static_cast<float>(UPPER_ROWS);
            } else if (k == 1) {
                t = plateau; // the plateau's end, still fully opaque
            } else {
                t = plateau + ((1.0F - plateau) * static_cast<float>(k - 1) / static_cast<float>(UPPER_ROWS - 1));
            }
            const float fadeHeight = 1.0F - plateau;
            alpha = (t <= plateau || fadeHeight <= 0.0F) ? 1.0F : smoothstep((1.0F - t) / fadeHeight);
        }
        const auto alphaByte = static_cast<std::uint8_t>(std::lround(alpha * 255.0F));
        for (std::uint32_t column = 0; column < COLUMNS; ++column) {
            const auto ringColumn = (static_cast<std::uint32_t>(arcIndex) * ARC_SEGMENTS) + column;
            const float angle
                = 2.0F * std::numbers::pi_v<float> * static_cast<float>(ringColumn) / static_cast<float>(K_SEGMENTS);
            const RingVertex vertex {
                .x = std::cos(angle),
                .y = std::sin(angle),
                .z = halfHeight * t,
                .unusedW = 0.0F,
                .u = floatToHalf(static_cast<float>(ringColumn) / static_cast<float>(K_SEGMENTS)),
                .v = floatToHalf(0.5F - (t * 0.5F)),
                .rgba = {255, 255, 255, alphaByte},
            };
            minPos.x = std::min(minPos.x, vertex.x);
            minPos.y = std::min(minPos.y, vertex.y);
            minPos.z = std::min(minPos.z, vertex.z);
            maxPos.x = std::max(maxPos.x, vertex.x);
            maxPos.y = std::max(maxPos.y, vertex.y);
            maxPos.z = std::max(maxPos.z, vertex.z);
            vertices.push_back(vertex);
        }
    }

    // Tight bound of just this arc: its center sits out on the ring, not at the camera,
    // which is what makes the translucent-pass depth sort treat the band as far (K_ARCS)
    const RE::NiPoint3 boundCenter = (minPos + maxPos) * 0.5F;
    const RE::NiPoint3 halfExtent = (maxPos - minPos) * 0.5F;
    boundOut.center = boundCenter;
    boundOut.radius = 1.01F * halfExtent.Length();

    std::vector<std::uint16_t> indices;
    indices.reserve(static_cast<std::size_t>(TRIANGLE_COUNT) * 3);
    for (std::uint32_t row = 0; row + 1 < K_ROWS; ++row) {
        for (std::uint32_t segment = 0; segment < ARC_SEGMENTS; ++segment) {
            const auto v00 = static_cast<std::uint16_t>((row * COLUMNS) + segment);
            const auto v01 = static_cast<std::uint16_t>(v00 + 1);
            const auto v10 = static_cast<std::uint16_t>(v00 + COLUMNS);
            const auto v11 = static_cast<std::uint16_t>(v10 + 1);
            // Facing is not load-bearing: the shader property is double-sided, since the
            // camera always sits inside the ring
            indices.insert(indices.end(), {v00, v01, v10, v10, v01, v11});
        }
    }

    // The struct and the CPU copies must come from the game heap: the engine's release
    // path (releaseGeometryData's twin) frees them there when the last holder lets go
    const std::size_t vertexBytes = vertices.size() * sizeof(RingVertex);
    const std::size_t indexBytes = indices.size() * sizeof(std::uint16_t);
    auto* const rawVerts = static_cast<std::uint8_t*>(RE::malloc(vertexBytes));
    auto* const rawIndices = static_cast<std::uint16_t*>(RE::malloc(indexBytes));
    auto* const dataPtr = static_cast<RE::BSGraphics::TriShape*>(RE::malloc(sizeof(RE::BSGraphics::TriShape)));
    if (rawVerts == nullptr || rawIndices == nullptr || dataPtr == nullptr) {
        RE::free(rawVerts);
        RE::free(rawIndices);
        RE::free(dataPtr);
        return nullptr;
    }
    std::memcpy(rawVerts, vertices.data(), vertexBytes);
    std::memcpy(rawIndices, indices.data(), indexBytes);

    // GPU side, seeded from the CPU copies. The vertex buffer is DYNAMIC because the
    // per-frame tint rewrites the vertex colors as the water color changes
    // (refreshArcColors); the index buffer never changes.
    REX::W32::D3D11_BUFFER_DESC vertexBufferDesc {};
    vertexBufferDesc.byteWidth = static_cast<std::uint32_t>(vertexBytes);
    vertexBufferDesc.usage = REX::W32::D3D11_USAGE_DYNAMIC;
    vertexBufferDesc.bindFlags = REX::W32::D3D11_BIND_VERTEX_BUFFER;
    vertexBufferDesc.cpuAccessFlags = REX::W32::D3D11_CPU_ACCESS_WRITE;
    REX::W32::D3D11_SUBRESOURCE_DATA vertexInit {};
    vertexInit.sysMem = rawVerts;

    REX::W32::D3D11_BUFFER_DESC indexBufferDesc {};
    indexBufferDesc.byteWidth = static_cast<std::uint32_t>(indexBytes);
    indexBufferDesc.usage = REX::W32::D3D11_USAGE_IMMUTABLE;
    indexBufferDesc.bindFlags = REX::W32::D3D11_BIND_INDEX_BUFFER;
    REX::W32::D3D11_SUBRESOURCE_DATA indexInit {};
    indexInit.sysMem = rawIndices;

    REX::W32::ID3D11Buffer* vertexBuffer = nullptr;
    REX::W32::ID3D11Buffer* indexBuffer = nullptr;
    if (device->CreateBuffer(&vertexBufferDesc, &vertexInit, &vertexBuffer) < 0
        || device->CreateBuffer(&indexBufferDesc, &indexInit, &indexBuffer) < 0 || vertexBuffer == nullptr
        || indexBuffer == nullptr) {
        if (vertexBuffer != nullptr) {
            vertexBuffer->Release();
        }
        if (indexBuffer != nullptr) {
            indexBuffer->Release();
        }
        RE::free(rawVerts);
        RE::free(rawIndices);
        RE::free(dataPtr);
        return nullptr;
    }

    dataPtr->vertexBuffer = reinterpret_cast<RE::ID3D11Buffer*>(vertexBuffer);
    dataPtr->indexBuffer = reinterpret_cast<RE::ID3D11Buffer*>(indexBuffer);
    dataPtr->vertexDesc = std::bit_cast<RE::BSGraphics::VertexDesc>(K_RING_VERTEX_DESC);
    dataPtr->refCount = 1; // the caller's reference
    dataPtr->pad1C = 0;
    dataPtr->rawVertexData = rawVerts;
    dataPtr->rawIndexData = rawIndices;
    return dataPtr;
}

void HorizonBand::installArcOnShape(RE::BSTriShape* shape,
                                    RE::BSGraphics::TriShape* arcData,
                                    const RE::NiBound& bound)
{
    // The geometry's own descriptor and counts are what the renderer binds the stride,
    // input layout, and draw ranges from; whatever renderer geometry the shape carried
    // (the donor's placeholder quad, or the reference a clone arrived with) goes back first
    auto& geometryData = shape->GetGeometryRuntimeData();
    releaseGeometryData(geometryData.rendererData);
    geometryData.rendererData = arcData;
    geometryData.vertexDesc = arcData->vertexDesc;

    constexpr int ARC_SEGMENTS = K_SEGMENTS / K_ARCS;
    shape->GetTrishapeRuntimeData().vertexCount = static_cast<std::uint16_t>((ARC_SEGMENTS + 1) * K_ROWS);
    shape->GetTrishapeRuntimeData().triangleCount = static_cast<std::uint16_t>(ARC_SEGMENTS * (K_ROWS - 1) * 2);
    shape->GetModelData().modelBound = bound;
}

void HorizonBand::configureArc(RE::BSTriShape* arcShape)
{
    using Flag = RE::BSShaderProperty::EShaderPropertyFlag;

    auto& geometryData = arcShape->GetGeometryRuntimeData();
    auto* const property = static_cast<RE::BSEffectShaderProperty*>(geometryData.shaderProperty.get());
    if (property == nullptr) {
        return;
    }

    // Unlit, double-sided, vertex-color/alpha driven, depth-tested but never
    // depth-written, exempt from distance fade, and free of every decal/refraction/
    // grayscale path. The donor NIF carries the same settings; asserting them here keeps
    // the shipped file non-tuning-critical.
    property->flags.set(Flag::kZBufferTest,
                        Flag::kVertexColors,
                        Flag::kVertexAlpha,
                        Flag::kTwoSided,
                        Flag::kNoFade);
    property->flags.reset(Flag::kZBufferWrite,
                          Flag::kSoftEffect,
                          Flag::kFalloff,
                          Flag::kEffectLighting,
                          Flag::kExternalEmittance,
                          Flag::kGrayscaleToPaletteColor,
                          Flag::kGrayscaleToPaletteAlpha,
                          Flag::kDecal,
                          Flag::kDynamicDecal,
                          Flag::kOwnEmit);
    property->alpha = 1.0F;

    // The loader may hand shared/pooled materials to identical properties; force a unique
    // copy so the band's material settings can never leak into someone else's effect
    if (auto* const material = property->GetMaterial(); material != nullptr) {
        property->SetMaterial(material, true);
    }
    if (auto* const material = property->GetMaterial(); material != nullptr) {
        material->baseColor = RE::NiColorA {1.0F, 1.0F, 1.0F, 1.0F};
        material->baseColorScale = 1.0F;
        material->falloffStartAngle = 1.0F;
        material->falloffStopAngle = 1.0F;
        material->falloffStartOpacity = 1.0F;
        material->falloffStopOpacity = 1.0F;
        // No textures at all: the effect technique's texture bits are gated on these PATH
        // strings (verified in 1.7.99: SetupTechnique tests the path, and the greyscale
        // texture pointer is dereferenced UNCHECKED when the grayscale bits are on), so a
        // truly null path both avoids that hazard and gives the pure vertex-color * tint
        // shading the band wants
        material->sourceTexture.reset();
        material->greyscaleTexture.reset();
        material->sourceTexturePath = RE::BSFixedString {};
        material->greyscaleTexturePath = RE::BSFixedString {};
    }

    // Straight alpha compositing against whatever is behind the band, no alpha testing
    if (auto* const alphaProperty = geometryData.alphaProperty.get(); alphaProperty != nullptr) {
        alphaProperty->SetAlphaBlending(true);
        alphaProperty->SetSrcBlendMode(RE::NiAlphaProperty::AlphaFunction::kSrcAlpha);
        alphaProperty->SetDestBlendMode(RE::NiAlphaProperty::AlphaFunction::kInvSrcAlpha);
        alphaProperty->SetAlphaTesting(false);
        alphaProperty->alphaThreshold = 0;
    }
}

auto HorizonBand::loadModel() -> bool
{
    // The donor NIF exists so the engine's own loader constructs the effect-shader object
    // graph (works on every runtime); its placeholder quad is replaced with the ring below
    RE::BSModelDB::DBTraits::ArgsType args {};
    RE::NiPointer<RE::NiNode> model;
    const auto error = RE::BSModelDB::Demand(K_MODEL_PATH, model, args);
    if (error != RE::BSResource::ErrorCode::kNone || !model) {
        s_loadFailed = true;
        spdlog::warn("Horizon blend disabled: donor model Meshes/{} failed to load (error {})",
                     K_MODEL_PATH,
                     static_cast<std::uint32_t>(error));
        return false;
    }

    auto* const object = model->GetObjectByName(K_SHAPE_NAME);
    auto* const shape = object != nullptr ? object->AsTriShape() : nullptr;
    if (shape == nullptr || !shape->GetGeometryRuntimeData().shaderProperty) {
        s_loadFailed = true;
        spdlog::warn("Horizon blend disabled: donor model has no usable {} tri shape", K_SHAPE_NAME);
        return false;
    }

    // Arc 0 replaces the donor shape's placeholder quad; the remaining arcs are engine
    // clones of it. Cloning deep-copies the shader/alpha properties and material (each
    // arc needs its own render-pass list anyway), so every arc is configured and tinted
    // individually - configuring only the donor left the clones on the raw NIF material,
    // which showed as per-segment color seams.
    const float degrees = ConfigLoader::getHorizonBlendDegrees();
    const float opaquePercent = ConfigLoader::getHorizonBlendOpaquePercent();
    s_arcs.clear();
    for (int arc = 0; arc < K_ARCS; ++arc) {
        RE::NiBound bound {};
        auto* const arcData = buildArcGeometry(arc, degrees, opaquePercent / 100.0F, bound);
        if (arcData == nullptr) {
            s_arcs.clear();
            s_loadFailed = true;
            spdlog::warn("Horizon blend disabled: building arc {} geometry failed", arc);
            return false;
        }

        RE::BSTriShape* target = shape;
        if (arc > 0) {
            RE::NiCloningProcess cloning {};
            cloning.scale = RE::NiPoint3 {1.0F, 1.0F, 1.0F};
            // Keep a reference alive until AttachChild takes its own; CreateClone
            // returns a refcount-0 object
            const RE::NiPointer<RE::NiObject> cloneBase {shape->CreateClone(cloning)};
            shape->ProcessClone(cloning);
            auto* const cloneGeometry = cloneBase ? cloneBase->AsGeometry() : nullptr;
            target = cloneGeometry != nullptr ? cloneGeometry->AsTriShape() : nullptr;
            if (target == nullptr) {
                releaseGeometryData(arcData);
                s_arcs.clear();
                s_loadFailed = true;
                spdlog::warn("Horizon blend disabled: cloning arc {} failed", arc);
                return false;
            }
            // Same interned name on every arc so the tint-refresh hook matches them all
            target->name = K_SHAPE_NAME;
            model->AttachChild(target, true);
        }
        installArcOnShape(target, arcData, bound);
        s_arcs.emplace_back(target);
    }

    s_model = model;
    s_builtDegrees = degrees;
    for (const auto& arcShape : s_arcs) {
        configureArc(arcShape.get());
    }

    spdlog::info("Horizon blend band built: {} degrees, {}% opaque above the seam, {} arcs x {} segments x {} rows, "
                 "rim at {} of far clip",
                 degrees,
                 opaquePercent,
                 K_ARCS,
                 K_SEGMENTS / K_ARCS,
                 K_ROWS,
                 K_FARCLIP_FRACTION);
    return true;
}

void HorizonBand::ensure(RE::NiNode* skirtRoot,
                         float waterHeight)
{
    if (ConfigLoader::getHorizonBlendDegrees() <= 0.0F || skirtRoot == nullptr) {
        return; // feature disabled (the default)
    }
    if (s_loadFailed) {
        return; // donor NIF is missing/broken; already logged once
    }
    if (!s_model && !loadModel()) {
        return;
    }

    s_waterHeight = waterHeight;

    // Attach under the skirt root: the map-menu hide and the worldspace teardown of the
    // skirt then cover the band with no extra bookkeeping
    if (s_model->parent != skirtRoot) {
        if (s_model->parent != nullptr) {
            s_model->parent->DetachChild(s_model.get());
        }
        skirtRoot->AttachChild(s_model.get(), true);
        RE::NiUpdateData updateData {};
        s_model->Update(updateData);
        spdlog::info("Horizon blend band attached (waterline Z {})", s_waterHeight);
    }
}

void HorizonBand::detach()
{
    if (s_model && s_model->parent != nullptr) {
        s_model->parent->DetachChild(s_model.get());
    }
    // s_model and s_arcs stay cached: the next ensure() only re-attaches
}

void HorizonBand::updateFrame(const RE::NiCamera* camera)
{
    if (!s_model || s_arcs.empty() || s_model->parent == nullptr) {
        return;
    }

    const float farClip = camera->GetRuntimeData2().viewFrustum.fFar;
    if (farClip <= 0.0F) {
        return;
    }

    // Uniform scale from the live far clip: the ring is unit-radius with half-height
    // tan(angle), so scaling by fraction * farClip * cos(angle) puts its farthest points
    // (the rim corners) exactly at fraction * farClip - always inside the frustum
    const float radians = s_builtDegrees * std::numbers::pi_v<float> / 180.0F;
    const float scale = K_FARCLIP_FRACTION * farClip * std::cos(radians);

    // Center the band vertically on the water-sky seam as SEEN from the camera. The seam
    // is the skirt's outer rim, so from camera height h it sits at depression angle
    // tan(d) = (h - water) / skirtRadius; the band at radius r must center at
    // z = h - r * (h - water) / skirtRadius to appear on that same line (exact tangent
    // ratio, no small-angle approximation). Fixing the band at the waterline instead made
    // it sag below the seam from any elevated viewpoint - field-observed from a mountain
    // top, where the haze painted a stripe well under an untouched hard seam - because the
    // band sits ~15x closer than the rim, so its waterline drops ~15x the seam's angle.
    // At water level the two placements coincide.
    const auto& cameraPos = camera->world.translate;
    const float seamDrop = (cameraPos.z - s_waterHeight) * (scale / WaterSkirt::effectiveRadius());
    s_model->local.translate = RE::NiPoint3 {cameraPos.x, cameraPos.y, cameraPos.z - seamDrop};
    s_model->local.scale = scale;
    RE::NiUpdateData updateData {};
    s_model->Update(updateData);

    // Keep the color-match sample points tracking this frame's camera and band placement
    computeSampleUVs(camera, scale, cameraPos.z - seamDrop, scale * std::tan(radians));

    // Re-tint in the far water's color: distant water converges to the sky's FOG FAR
    // color as its fog saturates (vanilla and CS Unified Water alike), and the closed-loop
    // correction (see captureMatchSamples) trims that prior until the band renders exactly
    // what the framebuffer showed beneath it - whatever water mod or renderer produced
    // that color. The band carries no sky color: above the seam its alpha fade lets the
    // real sky through, so the sky side of the blend is right at every azimuth without
    // ever being known (see s_bandColor). The actual vertex rewrite happens in the render
    // hook; here only the target is computed.
    auto* const sky = RE::Sky::GetSingleton();
    if (sky != nullptr) {
        const auto& rawFogFar = sky->skyColor[RE::TESWeather::ColorTypes::kFogFar];
        s_bandColor = RE::NiColor {
            std::clamp(rawFogFar.red * s_waterCorrection.at(0).load(std::memory_order_relaxed), 0.0F, 1.0F),
            std::clamp(rawFogFar.green * s_waterCorrection.at(1).load(std::memory_order_relaxed), 0.0F, 1.0F),
            std::clamp(rawFogFar.blue * s_waterCorrection.at(2).load(std::memory_order_relaxed), 0.0F, 1.0F)};
        s_tintStamp.fetch_add(1, std::memory_order_release);
    }

    if (!s_loggedFirstFrame && sky != nullptr) {
        s_loggedFirstFrame = true;
        const auto& fogFarColor = sky->skyColor[RE::TESWeather::ColorTypes::kFogFar];
        spdlog::info("Horizon blend band first frame: far clip {}, world radius {}, camera Z {}, waterline Z {}, "
                     "seam drop {}, fog far color ({}, {}, {})",
                     farClip,
                     scale,
                     cameraPos.z,
                     s_waterHeight,
                     seamDrop,
                     fogFarColor.red,
                     fogFarColor.green,
                     fogFarColor.blue);
    }
}
