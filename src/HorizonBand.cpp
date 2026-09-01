#include "HorizonBand.hpp"

#include "ConfigLoader.hpp"
#include "WaterSkirt.hpp"

#include "PCH.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
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
    for (std::uint32_t row = 0; row < K_ROWS; ++row) {
        // Row parameter from -1 (bottom rim) to +1 (top rim); the alpha gradient is a
        // smoothstep of the distance from the rims, peaking fully opaque at the center
        // row so the seam is completely covered
        const float t = (2.0F * static_cast<float>(row) / (K_ROWS - 1)) - 1.0F;
        const float rimDistance = 1.0F - std::fabs(t);
        const float alpha = rimDistance * rimDistance * (3.0F - (2.0F * rimDistance));
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

    // GPU side: immutable buffers seeded from the CPU copies
    REX::W32::D3D11_BUFFER_DESC vertexBufferDesc {};
    vertexBufferDesc.byteWidth = static_cast<std::uint32_t>(vertexBytes);
    vertexBufferDesc.usage = REX::W32::D3D11_USAGE_IMMUTABLE;
    vertexBufferDesc.bindFlags = REX::W32::D3D11_BIND_VERTEX_BUFFER;
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
    // copy so the per-frame horizon tint can never leak into someone else's effect
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
    s_arcs.clear();
    for (int arc = 0; arc < K_ARCS; ++arc) {
        RE::NiBound bound {};
        auto* const arcData = buildArcGeometry(arc, degrees, bound);
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
            // Same name on every arc so the ring reads as one object in scene dumps
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

    spdlog::info("Horizon blend band built: {} degrees, {} arcs x {} segments x {} rows, rim at {} of far clip",
                 degrees,
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

    // Re-tint from the sky's live horizon color: this is what makes the blend weather-mod
    // independent - whatever fed the sky this frame (any weather mod, transition, time of
    // day) is what the water fades into. Every arc carries its own material (see s_arcs),
    // so the tint is written into each.
    auto* const sky = RE::Sky::GetSingleton();
    if (sky != nullptr) {
        const auto& horizon = sky->skyColor[RE::TESWeather::ColorTypes::kHorizon];
        const RE::NiColorA tint {horizon.red, horizon.green, horizon.blue, 1.0F};
        for (const auto& arcShape : s_arcs) {
            auto* const property
                = static_cast<RE::BSEffectShaderProperty*>(arcShape->GetGeometryRuntimeData().shaderProperty.get());
            auto* const material = property != nullptr ? property->GetMaterial() : nullptr;
            if (material != nullptr) {
                material->baseColor = tint;
            }
        }
    }

    if (!s_loggedFirstFrame && sky != nullptr) {
        s_loggedFirstFrame = true;
        const auto& horizonColor = sky->skyColor[RE::TESWeather::ColorTypes::kHorizon];
        spdlog::info("Horizon blend band first frame: far clip {}, world radius {}, camera Z {}, waterline Z {}, "
                     "seam drop {}, horizon color ({}, {}, {})",
                     farClip,
                     scale,
                     cameraPos.z,
                     s_waterHeight,
                     seamDrop,
                     horizonColor.red,
                     horizonColor.green,
                     horizonColor.blue);
    }
}
