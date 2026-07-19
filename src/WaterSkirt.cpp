#include "WaterSkirt.hpp"

#include "ConfigLoader.hpp"

#include "PCH.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <numbers>
#include <span>
#include <utility>
#include <vector>

using namespace HorizonFix;

auto WaterSkirt::halfToFloat(std::uint16_t half) -> float
{
    // IEEE 754 binary16 (half) and binary32 (float) field layouts
    constexpr std::uint32_t HALF_MANTISSA_BITS = 10;
    constexpr std::uint32_t HALF_EXPONENT_BITS = 5;
    constexpr std::int32_t HALF_EXPONENT_BIAS = 15;
    constexpr std::uint32_t FLOAT_MANTISSA_BITS = 23;
    constexpr std::uint32_t FLOAT_EXPONENT_BITS = 8;
    constexpr std::int32_t FLOAT_EXPONENT_BIAS = 127;

    // Masks and shifts derived from the layouts
    constexpr std::uint32_t HALF_SIGN_MASK = 1U << (HALF_EXPONENT_BITS + HALF_MANTISSA_BITS);
    constexpr std::uint32_t HALF_EXPONENT_MASK = (1U << HALF_EXPONENT_BITS) - 1U;
    constexpr std::uint32_t HALF_MANTISSA_MASK = (1U << HALF_MANTISSA_BITS) - 1U;
    constexpr std::uint32_t HALF_IMPLICIT_ONE = 1U << HALF_MANTISSA_BITS;
    constexpr std::uint32_t SIGN_SHIFT
        = (FLOAT_EXPONENT_BITS + FLOAT_MANTISSA_BITS) - (HALF_EXPONENT_BITS + HALF_MANTISSA_BITS);
    constexpr std::uint32_t MANTISSA_SHIFT = FLOAT_MANTISSA_BITS - HALF_MANTISSA_BITS;
    constexpr std::uint32_t FLOAT_EXPONENT_ALL_ONES = ((1U << FLOAT_EXPONENT_BITS) - 1U) << FLOAT_MANTISSA_BITS;
    constexpr std::int32_t BIAS_DELTA = FLOAT_EXPONENT_BIAS - HALF_EXPONENT_BIAS;

    const std::uint32_t sign = static_cast<std::uint32_t>(half & HALF_SIGN_MASK) << SIGN_SHIFT;
    auto exponent = static_cast<std::int32_t>((half >> HALF_MANTISSA_BITS) & HALF_EXPONENT_MASK);
    std::uint32_t mantissa = half & HALF_MANTISSA_MASK;

    // An all-ones exponent field encodes infinity / NaN
    if (std::cmp_equal(exponent, HALF_EXPONENT_MASK)) {
        return std::bit_cast<float>(sign | FLOAT_EXPONENT_ALL_ONES | (mantissa << MANTISSA_SHIFT));
    }
    if (exponent == 0) {
        if (mantissa == 0) { // signed zero
            return std::bit_cast<float>(sign);
        }
        // Subnormal: shift the implicit leading 1 into place, adjusting the exponent
        while ((mantissa & HALF_IMPLICIT_ONE) == 0) {
            mantissa <<= 1U;
            --exponent;
        }
        ++exponent;
        mantissa &= HALF_MANTISSA_MASK;
    }
    // Rebias the raw exponent from half to float: value = raw - HALF_EXPONENT_BIAS,
    // so the float's raw field is value + FLOAT_EXPONENT_BIAS = raw + BIAS_DELTA
    return std::bit_cast<float>(sign | (static_cast<std::uint32_t>(exponent + BIAS_DELTA) << FLOAT_MANTISSA_BITS)
                                | (mantissa << MANTISSA_SHIFT));
}

auto WaterSkirt::rotationColumn(const RE::NiMatrix3& rot,
                                int col) -> RE::NiPoint3
{
    // split into cases to avoid out-of-bounds access on the matrix; the default case returns a zero vector
    switch (col) {
    case 0:
        return RE::NiPoint3 {rot.entry[0][0], rot.entry[1][0], rot.entry[2][0]};
    case 1:
        return RE::NiPoint3 {rot.entry[0][1], rot.entry[1][1], rot.entry[2][1]};
    case 2:
        return RE::NiPoint3 {rot.entry[0][2], rot.entry[1][2], rot.entry[2][2]};
    default:
        return RE::NiPoint3 {0.0F, 0.0F, 0.0F};
    }
}

void WaterSkirt::buildFrustumPlanes(const RE::NiCamera* camera,
                                    FrustumPlanes& planes)
{
    // Extract the camera basis in world space: NiCamera stores forward/up/right
    // as the columns of its world rotation
    const auto& world = camera->world;
    const auto& frustum = camera->viewFrustum;
    const RE::NiPoint3 dir = rotationColumn(world.rotate, 0);
    const RE::NiPoint3 up = rotationColumn(world.rotate, 1);
    const RE::NiPoint3 right = rotationColumn(world.rotate, 2);
    const RE::NiPoint3& pos = world.translate;

    // A plane with the given inward normal passing through the given point
    const auto planeThrough = [](const RE::NiPoint3& normal, const RE::NiPoint3& point) -> FrustumPlane {
        return FrustumPlane {.normal = normal, .d = normal.Dot(point)};
    };

    // Near and far planes are perpendicular to the view direction in both projections
    const FrustumPlane nearPlane = planeThrough(dir, pos + (dir * frustum.fNear));
    const FrustumPlane farPlane = planeThrough(-dir, pos + (dir * frustum.fFar));

    // Orthographic: the side planes are parallel to the view direction, offset
    // by the frustum extents along the right/up axes. Assigned as a whole array
    // in near, far, left, right, top, bottom order.
    if (frustum.bOrtho) {
        planes = FrustumPlanes {
            nearPlane,
            farPlane,
            planeThrough(right, pos + (right * frustum.fLeft)),
            planeThrough(-right, pos + (right * frustum.fRight)),
            planeThrough(-up, pos + (up * frustum.fTop)),
            planeThrough(up, pos + (up * frustum.fBottom)),
        };
        return;
    }

    // Perspective: fLeft/fRight/fTop/fBottom are slopes at unit distance. Each side
    // plane passes through the camera position; its inward normal is the axis tilted
    // against the view direction by the slope, normalized by 1/sqrt(slope^2 + 1).
    const auto slopePlane = [&](const RE::NiPoint3& axis, float slope, float sign) -> FrustumPlane {
        const float k = 1.0F / std::sqrt((slope * slope) + 1.0F);
        const RE::NiPoint3 normal = ((axis * sign) - (dir * (sign * slope))) * k;
        return FrustumPlane {.normal = normal, .d = normal.Dot(pos)};
    };
    planes = FrustumPlanes {
        nearPlane,
        farPlane,
        slopePlane(right, frustum.fLeft, 1.0F),
        slopePlane(right, frustum.fRight, -1.0F),
        slopePlane(up, frustum.fTop, -1.0F),
        slopePlane(up, frustum.fBottom, 1.0F),
    };
}

auto WaterSkirt::boundInFrustum(const RE::NiBound& bound,
                                const FrustumPlanes& planes) -> bool
{
    // Visible unless the sphere sits entirely on the outside of some plane
    // (signed distance from plane less than -radius)
    return std::ranges::all_of(
        planes, [&](const auto& plane) -> auto { return plane.normal.Dot(bound.center) - plane.d > -bound.radius; });
}

auto WaterSkirt::getWorldSpace(RE::TES* tesPtr) -> RE::TESWorldSpace*
{
#ifdef SKYRIM_SUPPORT_AE
    // Game patch 1.6.1130 inserted 8 bytes into TES ahead of worldSpace, and
    // CommonLib's compiled AE layout only matches 1.6.1130+; pre-1130 AE
    // runtimes (1.6.318-1.6.678) keep the field at 0x140, where the compiled
    // member read would dereference deadCount instead. The SE build's layout
    // (and this constant's namespace) doesn't cover 1.6.x, hence the ifdef.
    if (REL::Module::get().version() < SKSE::RUNTIME_SSE_1_6_1130) {
        constexpr std::uintptr_t OFFSET_640 = 0x140;
        return *reinterpret_cast<RE::TESWorldSpace**>(reinterpret_cast<std::uintptr_t>(tesPtr) + OFFSET_640);
    }
#endif
    return tesPtr->worldSpace;
}

auto WaterSkirt::getLODWorldSpace(RE::TESWorldSpace* worldSpacePtr) -> RE::TESWorldSpace*
{
    // Climb to whichever ancestor actually owns the LOD data (e.g. Tamriel for
    // city worldspaces); that record's lodWaterHeight is the one in effect
    while ((worldSpacePtr->parentWorld != nullptr)
           && worldSpacePtr->parentUseFlags.any(RE::TESWorldSpace::ParentUseFlag::kUseLODData)) {
        worldSpacePtr = worldSpacePtr->parentWorld;
    }
    return worldSpacePtr;
}

void WaterSkirt::searchTemplateQuad(RE::NiAVObject* objPtr,
                                    TemplateSearch& search)
{
    // Skip empty slots and our own previously created tiles (never clone a clone)
    if ((objPtr == nullptr) || objPtr->name == K_TILE_NAME) {
        return;
    }

    // Leaf geometry: score it. Fewest vertices wins (a 4-vertex shape is a clean
    // rectangle), with the largest world bound as tie-breaker (a fully submerged
    // ocean chunk); degenerate shapes are ignored.
    if (auto* const shape = objPtr->AsTriShape()) {
        const float radius = shape->worldBound.radius;
        const std::uint32_t vertexCount = shape->vertexCount;
        const bool better = (search.best == nullptr) || vertexCount < search.bestVertexCount
            || (vertexCount == search.bestVertexCount && radius > search.bestRadius);
        if (better && vertexCount > 0 && radius > 0.0F) {
            // Only solid sheets of water may seed the skirt; a narrow river rectangle or a
            // shoreline mesh with holes repeats in every tile as stripes across the horizon.
            // Measured only for shapes that would win, so the work stays bounded.
            const DonorCheck check = classifyDonor(shape);
            // Unmeasurable meshes are trusted only with the exact topology of an
            // engine-built water quad (see DonorVerdict)
            const bool trustedUnverifiable = vertexCount == 4 && shape->triangleCount == 2;
            if (check.verdict == DonorVerdict::K_NOT_SOLID_WATER
                || (check.verdict == DonorVerdict::K_UNVERIFIABLE && !trustedUnverifiable)) {
                ++search.rejectedCount;
                return;
            }
            search.best = shape;
            search.bestVertexCount = vertexCount;
            search.bestRadius = radius;
            search.bestCheck = check;
        }
        return;
    }

    // Interior node: recurse into all children
    if (auto* const node = objPtr->AsNode()) {
        for (const auto& child : node->children) {
            searchTemplateQuad(child.get(), search);
        }
    }
}

void WaterSkirt::removeSkirt()
{
    // Detach every tile from the scene graph; the NiPointers in s_tiles still
    // hold the last references until the clear below
    for (const auto& tile : s_tiles) {
        if (tile) {
            if (auto* const parent = tile->parent) {
                parent->DetachChild(tile.get());
            }
        }
    }

    // Detach and release the root node itself
    if (s_skirtRoot) {
        if (auto* const parent = s_skirtRoot->parent) {
            parent->DetachChild(s_skirtRoot.get());
        }
        s_skirtRoot.reset();
    }

    // Reset all cached state; the sentinel center forces a full rebuild next time
    s_tiles.clear();
    s_layout.clear();
    s_skirtWorldSpace = nullptr;
    s_centerBx = std::numeric_limits<int>::max();
    s_centerBy = std::numeric_limits<int>::max();
}

auto WaterSkirt::cloneTemplate(RE::BSTriShape* templatePtr) -> RE::NiPointer<RE::BSGeometry>
{
    RE::NiCloningProcess cloning {};
    cloning.scale = RE::NiPoint3 {1.0F, 1.0F, 1.0F};
    // Keep a reference alive until the returned NiPointer takes its own;
    // CreateClone returns a refcount-0 object.
    const RE::NiPointer<RE::NiObject> cloneBase {templatePtr->CreateClone(cloning)};
    templatePtr->ProcessClone(cloning);
    return RE::NiPointer<RE::BSGeometry> {cloneBase ? cloneBase->AsGeometry() : nullptr};
}

auto WaterSkirt::classifyDonor(const RE::BSTriShape* shape) -> DonorCheck
{
    // Measuring needs the CPU copies of the buffers; the engine keeps them for meshes
    // loaded from disk, but not for everything
    auto* const data = shape->rendererData;
    if ((data == nullptr) || (data->rawVertexData == nullptr) || (data->rawIndexData == nullptr)) {
        return DonorCheck {.verdict = DonorVerdict::K_UNVERIFIABLE, .detail = "no CPU copy"};
    }

    // Dynamic tri shapes keep their rendered positions in per-frame dynamic data, not
    // in the static vertex stream, so the CPU copy says nothing about the rendered
    // shape - and cloning a dynamic variant would copy an object whose layout the
    // rest of this pipeline does not expect. Reject outright (never trust these
    // through the engine-quad topology gate).
    if (shape->type == RE::BSGeometry::Type::kDynamicTriShape
        || shape->type == RE::BSGeometry::Type::kParticleShaderDynamicTriShape) {
        return DonorCheck {.verdict = DonorVerdict::K_NOT_SOLID_WATER, .detail = "dynamic tri shape"};
    }

    // Vertex stride: the low nibble of the descriptor is the size in dwords (the engine's
    // convention). Positions sit at offset 0, as three halfs or, with the full-precision
    // flag, three floats. One field-verified exception: engine-built water quads use a
    // bare layout - descriptor 0x0000100000000004, flags word holding only VF_VERTEX,
    // stride 16 - whose positions are four FLOATS even though the full-precision flag is
    // clear (confirmed on 1.6.1170: rendering half-encoded positions through that
    // descriptor's input layout produced float-reinterpreted garbage). Decoding such
    // quads as halfs used to misread every engine quad as inconsistent with its bound.
    constexpr std::uint64_t K_BARE_FLOAT4_DESC = 0x0000100000000004ULL;
    const auto descBits = std::bit_cast<std::uint64_t>(data->vertexDesc);
    const std::size_t stride = (descBits & 0xFU) * 4;
    const bool floatPositions
        = data->vertexDesc.HasFlag(RE::BSGraphics::Vertex::VF_FULLPREC) || descBits == K_BARE_FLOAT4_DESC;
    const std::size_t positionSize = floatPositions ? 3 * sizeof(float) : 3 * sizeof(std::uint16_t);
    const std::uint32_t vertexCount = shape->vertexCount;
    const std::uint32_t triangleCount = shape->triangleCount;
    constexpr std::size_t MAX_STRIDE = 64;
    if (vertexCount == 0 || triangleCount == 0 || stride < positionSize || stride > MAX_STRIDE) {
        return DonorCheck {.verdict = DonorVerdict::K_UNVERIFIABLE, .detail = "unexpected vertex layout"};
    }

    // Decode every vertex position, tracking the model-space extents
    std::vector<RE::NiPoint3> positions;
    positions.reserve(vertexCount);
    RE::NiPoint3 minPos {
        std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
    RE::NiPoint3 maxPos {std::numeric_limits<float>::lowest(),
                         std::numeric_limits<float>::lowest(),
                         std::numeric_limits<float>::lowest()};
    const std::span<const std::uint8_t> vertexData {data->rawVertexData,
                                                    static_cast<std::size_t>(vertexCount) * stride};
    for (std::uint32_t i = 0; i < vertexCount; ++i) {
        const std::uint8_t* const vertex = vertexData.subspan(static_cast<std::size_t>(i) * stride).data();
        RE::NiPoint3 pos;
        if (floatPositions) {
            std::memcpy(&pos.x, vertex, 3 * sizeof(float));
        } else {
            std::array<std::uint16_t, 3> halves {};
            std::memcpy(halves.data(), vertex, sizeof(halves));
            pos = RE::NiPoint3 {halfToFloat(halves.at(0)), halfToFloat(halves.at(1)), halfToFloat(halves.at(2))};
        }
        minPos.x = std::min(minPos.x, pos.x);
        minPos.y = std::min(minPos.y, pos.y);
        minPos.z = std::min(minPos.z, pos.z);
        maxPos.x = std::max(maxPos.x, pos.x);
        maxPos.y = std::max(maxPos.y, pos.y);
        maxPos.z = std::max(maxPos.z, pos.z);
        positions.push_back(pos);
    }

    // Sum the triangle areas projected on XY. For a gap-free sheet the total matches the
    // extents rectangle; holes and slivers fall short of it.
    double area2 = 0.0; // twice the summed area
    const std::span<const std::uint16_t> indices {data->rawIndexData, static_cast<std::size_t>(triangleCount) * 3};
    for (std::uint32_t t = 0; t < triangleCount; ++t) {
        // Copy the triangle's three indices into an array (std::span has no
        // bounds-checked accessor here); subspan carries the bounds knowledge
        std::array<std::uint16_t, 3> triangle {};
        std::memcpy(triangle.data(),
                    indices.subspan(static_cast<std::size_t>(t) * 3, triangle.size()).data(),
                    sizeof(triangle));
        const std::uint16_t idx0 = triangle.at(0);
        const std::uint16_t idx1 = triangle.at(1);
        const std::uint16_t idx2 = triangle.at(2);
        if (idx0 >= vertexCount || idx1 >= vertexCount || idx2 >= vertexCount) {
            // not the index data this mesh renders with
            return DonorCheck {.verdict = DonorVerdict::K_UNVERIFIABLE, .detail = "corrupt index data"};
        }
        const auto& p0 = positions.at(idx0);
        const auto& p1 = positions.at(idx1);
        const auto& p2 = positions.at(idx2);
        area2 += std::abs((static_cast<double>(p1.x - p0.x) * static_cast<double>(p2.y - p0.y))
                          - (static_cast<double>(p1.y - p0.y) * static_cast<double>(p2.x - p0.x)));
    }

    const float extentX = maxPos.x - minPos.x;
    const float extentY = maxPos.y - minPos.y;
    const float maxExtent = std::max(extentX, extentY);
    const double extentArea = static_cast<double>(extentX) * static_cast<double>(extentY);
    const float coverage = extentArea > 0.0 ? static_cast<float>(area2 * 0.5 / extentArea) : 0.0F;
    const float squareness = maxExtent > 0.0F ? std::min(extentX, extentY) / maxExtent : 0.0F;

    // The decode is only believable when it agrees with the mesh's own model bound.
    // (Engine-built water quads used to trip this gate when their bare-float4 layout
    // was decoded as halfs - see K_BARE_FLOAT4_DESC above; with the correct decode they
    // measure properly. The gate remains for meshes whose CPU copy is genuinely stale.)
    const float decodedHalfDiagonal = 0.5F * std::hypot(extentX, extentY);
    const float modelRadius = shape->modelBound.radius;
    const bool decodeConsistent
        = modelRadius > 0.0F && decodedHalfDiagonal >= 0.5F * modelRadius && decodedHalfDiagonal <= 2.0F * modelRadius;
    if (!decodeConsistent) {
        return DonorCheck {.verdict = DonorVerdict::K_UNVERIFIABLE, .detail = "CPU copy inconsistent with bound"};
    }

    // Solid water: one flat, gap-free, square sheet
    constexpr float MIN_SQUARENESS = 0.98F;
    constexpr float MIN_COVERAGE = 0.98F;
    const bool flat = (maxPos.z - minPos.z) <= std::max(1.0F, 0.001F * maxExtent);
    const bool solid = maxExtent > 1.0F && flat && squareness >= MIN_SQUARENESS && coverage >= MIN_COVERAGE;
    if (solid) {
        return DonorCheck {.verdict = DonorVerdict::K_SOLID_WATER, .detail = "measured solid"};
    }
    return DonorCheck {.verdict = DonorVerdict::K_NOT_SOLID_WATER, .detail = "measured not solid"};
}

void WaterSkirt::updateVisibility()
{
    if (s_tiles.empty()) {
        return;
    }
    if (s_mapMenuOpen) {
        return; // skirt is hidden at the root while the map is up
    }

    // Use the main world camera; bail while it isn't up (loading, menus)
    auto* const camera = RE::Main::WorldRootCamera();
    if (camera == nullptr) {
        return;
    }
    const auto camPos = camera->world.translate;
    const float farClip = camera->viewFrustum.fFar;
    if (farClip <= 0.0F) {
        return;
    }
    const float proxyDist = 0.5F * farClip;

    FrustumPlanes planes {};
    buildFrustumPlanes(camera, planes);

    // World position of the layout center (midpoint of the player's LOD32 block)
    const float centerX = (static_cast<float>(s_centerBx) + 0.5F) * K_TILE_SIZE;
    const float centerY = (static_cast<float>(s_centerBy) + 0.5F) * K_TILE_SIZE;

    // Where the game's real water renders, the skirt must not draw: two translucent
    // surfaces at the same height composite by draw order, which varies per frame
    // (multithreaded pass accumulation) and flickers from any angle. Whatever hides
    // a piece of skirt is water rendering those pixels, so hiding cannot open a
    // hole - and in worldspaces without water coverage (the small-map void case)
    // nothing overlaps, so everything fills.
    refreshNearWaterCoverage(centerX, centerY);

    const std::size_t count = std::min(s_tiles.size(), s_layout.size());
    for (std::size_t i = 0; i < count; ++i) {
        const auto& tile = s_tiles.at(i);
        if (!tile) {
            continue;
        }
        const auto& rel = s_layout.at(i);

        // True tile bound; the radius (half-diagonal is size * 0.71) carries
        // ~40% slack for the camera moving until the next update.
        RE::NiBound bound {};
        bound.center = RE::NiPoint3 {centerX + rel.dx, centerY + rel.dy, s_skirtHeight};
        bound.radius = rel.size;

        // Near-block representation choice (see buildLayout and isHiddenByNearWater)
        const bool hiddenByWater = isHiddenByNearWater(rel);

        // Every tile lies past the far plane, so testing the true bound would fail
        // the far-plane check for all of them. Project the bound toward the camera
        // onto a sphere at half the far clip; angular extent is preserved, so the
        // side-plane tests still reject tiles that are off-screen.
        const auto toTile = bound.center - camPos;
        const float dist = toTile.Length();
        if (dist > proxyDist) {
            const float shrink = proxyDist / dist;
            bound.center = camPos + (toTile * shrink);
            bound.radius *= shrink;
        }

        // Only touch the cull flag when the state actually changes, to avoid
        // needless scene-graph dirtying
        const bool visible = !hiddenByWater && boundInFrustum(bound, planes);
        if (tile->flags.any(RE::NiAVObject::Flag::kHidden) == visible) {
            tile->SetAppCulled(!visible);
        }
    }
}

void WaterSkirt::refreshNearWaterCoverage(float centerX,
                                          float centerY)
{
    s_nearWater.footprints.clear();
    for (auto& row : s_nearWater.covered) {
        row.fill(false);
    }
    s_nearWater.blockHasWater = {};

    // Footprints are XY rectangles: the engine's own multibound AABBs when present
    // (exact), the shape's sphere bound as a fallback (see WaterFootprint)
    if (auto* const waterSystem = RE::TESWaterSystem::GetSingleton(); waterSystem != nullptr) {
        const RE::BSSpinLockGuard locker(waterSystem->lock);
        for (const auto& waterObject : waterSystem->waterObjects) {
            if (!waterObject) {
                continue;
            }
            bool hasBox = false;
            for (const auto& box : waterObject->multiBounds) {
                if (box && box->size.x > 0.0F && box->size.y > 0.0F) {
                    hasBox = true;
                    s_nearWater.footprints.push_back(WaterFootprint {
                        .x = box->center.x, .y = box->center.y, .halfX = box->size.x, .halfY = box->size.y});
                }
            }
            if (!hasBox && waterObject->shape && waterObject->shape->worldBound.radius > 0.0F) {
                const auto& waterBound = waterObject->shape->worldBound;
                s_nearWater.footprints.push_back(WaterFootprint {.x = waterBound.center.x,
                                                                 .y = waterBound.center.y,
                                                                 .halfX = waterBound.radius,
                                                                 .halfY = waterBound.radius});
            }
        }
    }

    // Union coverage of live water over the near zone, rasterized at cell
    // resolution: a cell is marked only when a water box covers it COMPLETELY, so
    // a subtile whose cells are all marked is fully inside the union of water -
    // bodies assembled from many cell-sized planes (rivers, seas) mark their
    // interior correctly, and partially-covered edge cells stay unmarked.
    const float coverOriginX = centerX - (1.5F * K_TILE_SIZE);
    const float coverOriginY = centerY - (1.5F * K_TILE_SIZE);
    for (const auto& footprint : s_nearWater.footprints) {
        const int firstX
            = std::max(0,
                       static_cast<int>(std::ceil(((footprint.x - footprint.halfX - coverOriginX) / K_COVERAGE_CELL)
                                                  - K_COVERAGE_EPSILON)));
        const int endX
            = std::min(K_COVERAGE_GRID,
                       static_cast<int>(std::floor(((footprint.x + footprint.halfX - coverOriginX) / K_COVERAGE_CELL)
                                                   + K_COVERAGE_EPSILON)));
        const int firstY
            = std::max(0,
                       static_cast<int>(std::ceil(((footprint.y - footprint.halfY - coverOriginY) / K_COVERAGE_CELL)
                                                  - K_COVERAGE_EPSILON)));
        const int endY
            = std::min(K_COVERAGE_GRID,
                       static_cast<int>(std::floor(((footprint.y + footprint.halfY - coverOriginY) / K_COVERAGE_CELL)
                                                   + K_COVERAGE_EPSILON)));
        for (int cellX = firstX; cellX < endX; ++cellX) {
            for (int cellY = firstY; cellY < endY; ++cellY) {
                s_nearWater.covered.at(cellX).at(cellY) = true;
            }
        }
    }

    // Which of the 3x3 near blocks currently touch live water. Water-free blocks
    // render as their single full tile (subtiles hidden - no extra draw calls);
    // water-touched blocks render as subtiles with the fully-covered ones hidden.
    for (int blockX = -1; blockX <= 1; ++blockX) {
        for (int blockY = -1; blockY <= 1; ++blockY) {
            const float blockCenterX = centerX + (static_cast<float>(blockX) * K_TILE_SIZE);
            const float blockCenterY = centerY + (static_cast<float>(blockY) * K_TILE_SIZE);
            constexpr float HALF_BLOCK = K_TILE_SIZE * 0.5F;
            for (const auto& footprint : s_nearWater.footprints) {
                if (std::fabs(footprint.x - blockCenterX) <= HALF_BLOCK + footprint.halfX
                    && std::fabs(footprint.y - blockCenterY) <= HALF_BLOCK + footprint.halfY) {
                    s_nearWater.blockHasWater.at(blockX + 1).at(blockY + 1) = true;
                    break;
                }
            }
        }
    }
}

auto WaterSkirt::isHiddenByNearWater(const RelTile& rel) -> bool
{
    const bool nearFullTile
        = rel.size == K_TILE_SIZE && std::fabs(rel.dx) <= K_TILE_SIZE && std::fabs(rel.dy) <= K_TILE_SIZE;
    if (!nearFullTile && rel.size != K_NEAR_SUBTILE_SIZE) {
        return false;
    }

    // rim tiles can match the subtile size at high rim quality; they sit outside
    // the 3x3 blocks and are excluded by the range check
    const int blockX = static_cast<int>(std::lround(rel.dx / K_TILE_SIZE));
    const int blockY = static_cast<int>(std::lround(rel.dy / K_TILE_SIZE));
    if (blockX < -1 || blockX > 1 || blockY < -1 || blockY > 1) {
        return false;
    }

    const bool blockHasWater = s_nearWater.blockHasWater.at(blockX + 1).at(blockY + 1);
    if (nearFullTile) {
        return blockHasWater; // the subtiles represent a water-touched block
    }
    if (!blockHasWater) {
        return true; // the full tile represents a water-free block
    }

    // Hide only subtiles FULLY inside the water union. Edge-straddling subtiles
    // keep drawing through the boundary - hiding them whole left a sliver of
    // uncovered void along water edges. The overlap they create is safe: over
    // void the water reads far depth and renders opaque (order cannot matter),
    // and at shorelines the shallow bed is opaque and the depth bias drops skirt
    // fragments behind it before any compositing question arises.
    const float half = rel.size * 0.5F;
    const int firstCellX = static_cast<int>(std::lround((rel.dx - half + (1.5F * K_TILE_SIZE)) / K_COVERAGE_CELL));
    const int firstCellY = static_cast<int>(std::lround((rel.dy - half + (1.5F * K_TILE_SIZE)) / K_COVERAGE_CELL));
    constexpr int CELLS_PER_SUBTILE = static_cast<int>(K_NEAR_SUBTILE_SIZE / K_COVERAGE_CELL);
    for (int cellX = firstCellX; cellX < firstCellX + CELLS_PER_SUBTILE; ++cellX) {
        for (int cellY = firstCellY; cellY < firstCellY + CELLS_PER_SUBTILE; ++cellY) {
            if (cellX < 0 || cellX >= K_COVERAGE_GRID || cellY < 0 || cellY >= K_COVERAGE_GRID
                || !s_nearWater.covered.at(cellX).at(cellY)) {
                return false;
            }
        }
    }
    return true;
}

void WaterSkirt::setMapMenuOpen(bool open)
{
    s_mapMenuOpen = open;

    // One cull flag on the root hides every tile at once
    if (s_skirtRoot) {
        s_skirtRoot->SetAppCulled(open);
    }

    // On close, re-run the full update in case the player fast-traveled from the map
    if (!open) {
        queueUpdate();
    }
}

auto WaterSkirt::effectiveRadius() -> float
{
    // Never let the configured radius shrink below four tile lengths; a smaller
    // skirt would not reach past the game's own LOD water
    constexpr float CLAMP = 4.0F;
    return std::max(ConfigLoader::getSkirtRadius(), CLAMP * K_TILE_SIZE);
}

void WaterSkirt::layoutTile(float dx,
                            float dy,
                            float size,
                            int splitsLeft,
                            float radius)
{
    const float half = size * 0.5F;

    // Distance from the layout center to the tile's closest point; if even that
    // is beyond the radius, the whole tile is outside — drop it
    const float nearCorner = std::hypot(std::max(std::fabs(dx) - half, 0.0F), std::max(std::fabs(dy) - half, 0.0F));
    if (nearCorner > radius) {
        return; // entire tile outside the window
    }

    // Distance to the tile's farthest corner; if that is within the radius, the
    // tile is fully inside — emit it whole
    const float farCorner = std::hypot(std::fabs(dx) + half, std::fabs(dy) + half);
    if (farCorner <= radius) {
        s_layout.push_back(RelTile {.dx = dx, .dy = dy, .size = size});
        return;
    }

    // Tile straddles the rim. Out of subdivision budget: keep or drop it whole
    // based on which side its center falls on
    if (splitsLeft <= 0) {
        if (std::hypot(dx, dy) <= radius) {
            s_layout.push_back(RelTile {.dx = dx, .dy = dy, .size = size});
        }
        return;
    }

    // Otherwise quad-split into four half-size tiles and recurse, refining the
    // circular edge one level further
    const float quarter = size * 0.25F;
    for (const float sx : {-quarter, quarter}) {
        for (const float sy : {-quarter, quarter}) {
            layoutTile(dx + sx, dy + sy, half, splitsLeft - 1, radius);
        }
    }
}

void WaterSkirt::buildLayout()
{
    s_layout.clear();

    const float radius = effectiveRadius();
    const int quality = ConfigLoader::getRimQuality();

    // Number of whole tiles needed to reach the radius in each direction,
    // plus one so rim tiles that only partially overlap are still considered
    const int span = static_cast<int>(std::ceil(radius / K_TILE_SIZE)) + 1;

    // Walk the square of candidate blocks and let layoutTile trim it to a disc.
    // The central block is included: SkirtDepth stamps skirt fragments behind
    // everything, so wherever the game's own water or terrain drew, the inner tile
    // loses every pixel (early-Z, near free) - but in worldspaces whose LOD water
    // does not span the player's whole block (small custom maps), it fills what
    // would otherwise be a grid-aligned hole in the horizon (field-observed as a
    // missing quadrant when the player stands near a block corner).
    for (int ix = -span; ix <= span; ++ix) {
        for (int iy = -span; iy <= span; ++iy) {
            // The 3x3 blocks around the player carry two interchangeable
            // representations: the full tile, drawn while no live water touches
            // the block (one draw call), and an 8x8 grid of subtiles used when
            // water overlaps the block so that exactly the overlapped pieces can
            // be hidden. Two translucent surfaces at the same height composite by
            // draw order, which varies per frame (multithreaded pass accumulation)
            // and flickers; depth tricks cannot fix compositing order, so the
            // skirt simply must not rasterize where live water renders. Hiding an
            // overlapped subtile never opens a hole: whatever hid it is water
            // rendering those very pixels. updateVisibility shows exactly one
            // representation per frame.
            if (ix >= -1 && ix <= 1 && iy >= -1 && iy <= 1) {
                s_layout.push_back(RelTile {.dx = static_cast<float>(ix) * K_TILE_SIZE,
                                            .dy = static_cast<float>(iy) * K_TILE_SIZE,
                                            .size = K_TILE_SIZE});
                constexpr int DIVISIONS = static_cast<int>(K_TILE_SIZE / K_NEAR_SUBTILE_SIZE);
                for (int sx = 0; sx < DIVISIONS; ++sx) {
                    for (int sy = 0; sy < DIVISIONS; ++sy) {
                        const float offset = (static_cast<float>(DIVISIONS) - 1.0F) * 0.5F;
                        s_layout.push_back(RelTile {.dx = (static_cast<float>(ix) * K_TILE_SIZE)
                                                        + ((static_cast<float>(sx) - offset) * K_NEAR_SUBTILE_SIZE),
                                                    .dy = (static_cast<float>(iy) * K_TILE_SIZE)
                                                        + ((static_cast<float>(sy) - offset) * K_NEAR_SUBTILE_SIZE),
                                                    .size = K_NEAR_SUBTILE_SIZE});
                    }
                }
                continue;
            }

            layoutTile(static_cast<float>(ix) * K_TILE_SIZE,
                       static_cast<float>(iy) * K_TILE_SIZE,
                       K_TILE_SIZE,
                       quality,
                       radius);
        }
    }
}

void WaterSkirt::placeTiles(int centerBx,
                            int centerBy)
{
    // Midpoint of the player's LOD32 block; all RelTile offsets are relative to it
    const float centerX = (static_cast<float>(centerBx) + 0.5F) * K_TILE_SIZE;
    const float centerY = (static_cast<float>(centerBy) + 0.5F) * K_TILE_SIZE;

    RE::NiUpdateData updateData {};
    const std::size_t count = std::min(s_tiles.size(), s_layout.size());
    for (std::size_t i = 0; i < count; ++i) {
        const auto& tile = s_tiles.at(i);
        if (!tile) {
            continue;
        }
        const auto& rel = s_layout.at(i);

        // Scale the template to the tile's edge length, then translate so the
        // template's model-space center lands on the tile's world position
        // (the skirt root sits at the origin, so local == world here)
        const float scale = rel.size / s_modelSide;
        const RE::NiPoint3 tileCenter {centerX + rel.dx, centerY + rel.dy, s_skirtHeight};
        tile->local.scale = scale;
        tile->local.translate = tileCenter - s_modelCenter * scale;
        tile->Update(updateData);
    }
}

void WaterSkirt::updateSkirt()
{
    auto* const tes = RE::TES::GetSingleton();
    if (tes == nullptr) {
        return;
    }

    // TES::worldSpace can keep pointing at the last exterior worldspace while
    // an interior cell is loaded, so check interiorCell too; otherwise the
    // skirt survives interior transitions and keeps rendering there.
    auto* const worldSpace = getWorldSpace(tes);
    if (worldSpace == nullptr || tes->interiorCell != nullptr) {
        removeSkirt();
        return;
    }

    // Honor the user's per-worldspace blocklist: never build here, and tear down
    // anything carried over from a previous worldspace. Logged once per worldspace
    // so travel within a blocked world doesn't spam the log.
    if (ConfigLoader::isWorldSpaceBlocked(worldSpace->GetFormEditorID())) {
        static const RE::TESWorldSpace* lastBlockedLogged = nullptr;
        if (lastBlockedLogged != worldSpace) {
            lastBlockedLogged = worldSpace;
            spdlog::info("Water skirt disabled for {}: worldspace is on sWorldSpaceBlocklist",
                         worldSpace->GetFormEditorID());
        }
        removeSkirt();
        return;
    }

    // Small World worldspaces (cities, pocket worlds) get no skirt by default:
    // their NAM4 is unreliable and their local water sits at street-level heights,
    // which paints skirt water where it does not belong (field-observed in
    // Whiterun). Legitimate small worlds opt back in via sSmallWorldAllowlist.
    if (worldSpace->flags.any(RE::TESWorldSpace::Flag::kSmallWorld)
        && !ConfigLoader::isSmallWorldAllowed(worldSpace->GetFormEditorID())) {
        static const RE::TESWorldSpace* lastSmallWorldLogged = nullptr;
        if (lastSmallWorldLogged != worldSpace) {
            lastSmallWorldLogged = worldSpace;
            spdlog::info("Water skirt disabled for {}: Small World worldspace (add to sSmallWorldAllowlist to enable)",
                         worldSpace->GetFormEditorID());
        }
        removeSkirt();
        return;
    }

    // The scene-graph root the game hangs its LOD water under; the template
    // search runs below it and the skirt normally attaches to it
    auto* const root = tes->objLODWaterRoot;
    if (root == nullptr) {
        return;
    }

    // Which LOD32 block the player stands in — the skirt layout is centered there
    auto* const player = RE::PlayerCharacter::GetSingleton();
    if (player == nullptr) {
        return;
    }
    const auto playerPos = player->GetPosition();
    const int centerBx = static_cast<int>(std::floor(playerPos.x / K_TILE_SIZE));
    const int centerBy = static_cast<int>(std::floor(playerPos.y / K_TILE_SIZE));

    // Fast path: skirt already exists for this worldspace. Nothing to do if the
    // player is still in the same block.
    if (s_skirtWorldSpace == worldSpace && !s_tiles.empty()) {
        if (centerBx == s_centerBx && centerBy == s_centerBy) {
            return;
        }
        // Same worldspace, player crossed into another block: recycle the
        // existing tiles at the new window position.
        placeTiles(centerBx, centerBy);
        s_centerBx = centerBx;
        s_centerBy = centerBy;
        return;
    }

    // Full rebuild path. First find a clean water quad to clone from the game's
    // own LOD water for this worldspace.
    TemplateSearch search;
    searchTemplateQuad(root, search);
    if (search.best == nullptr) {
        // No LOD water attached yet (or the worldspace has none to clone);
        // retried on the next cell attach event.
        if (search.rejectedCount > 0) {
            spdlog::info("Water skirt: no usable donor yet ({} candidates rejected: not verifiably solid water); "
                         "waiting for the next cell attach",
                         search.rejectedCount);
        }
        return;
    }
    auto* const templateQuad = search.best;

    // Tear down any skirt left over from the previous worldspace
    removeSkirt();

    // Template extents in its own model space; BTR water quads have identity
    // rotation and are axis-aligned squares, so the bound radius is the
    // half-diagonal.
    const float worldScale = templateQuad->world.scale > 0.0F ? templateQuad->world.scale : 1.0F;
    s_modelCenter = (templateQuad->worldBound.center - templateQuad->world.translate) * (1.0F / worldScale);
    s_modelSide = (templateQuad->worldBound.radius / worldScale) * std::numbers::sqrt2_v<float>;
    if (s_modelSide < 1.0F) {
        return;
    }

    // The tiles sit exactly at the LOD water height (from whichever worldspace
    // owns the LOD data); coplanarity with the game's real water is handled by
    // the near-water yielding and the skirt depth bias, not by a height offset.
    // Small Worlds are skipped by default (see above), and allowlisted ones are
    // expected to define a proper LOD water height of their own.
    auto* const lodWorldSpace = getLODWorldSpace(worldSpace);
    s_skirtHeight = lodWorldSpace->lodWaterHeight;

    // Compute the disc of tile positions/sizes around the player's block
    buildLayout();

    // All tiles live under a dedicated root node, in true world coordinates.
    const auto childHint = static_cast<std::uint16_t>(std::min<std::size_t>(s_layout.size(), 0xFFFF));
    s_skirtRoot = RE::NiPointer<RE::NiNode> {RE::NiNode::Create(childHint)};
    if (!s_skirtRoot) {
        return;
    }
    s_skirtRoot->name = K_ROOT_NAME;

    // Attaching under the LOD land root instead makes the skirt sort with the
    // terrain pass, so water draws after (on top of) distant land
    auto* attachRoot = root;
    if (tes->lodLandRoot != nullptr) {
        attachRoot = tes->lodLandRoot;
    }
    attachRoot->AttachChild(s_skirtRoot.get(), true);

    // Clone one tile per layout entry. The inflated model bound plus kAlwaysDraw
    // makes the engine's frustum culling always pass (tiles beyond the far plane
    // would otherwise be culled); updateVisibility culls them properly instead.
    RE::NiUpdateData updateData {};
    s_tiles.reserve(s_layout.size());
    for (std::size_t i = 0; i < s_layout.size(); ++i) {
        auto tile = cloneTemplate(templateQuad);
        if (!tile) {
            break;
        }
        tile->name = K_TILE_NAME;
        tile->SetAppCulled(false);
        tile->flags.set(RE::NiAVObject::Flag::kAlwaysDraw);
        tile->modelBound.radius = K_CULL_PROOF_RADIUS;
        s_skirtRoot->AttachChild(tile.get(), true);
        tile->Update(updateData);
        s_tiles.emplace_back(std::move(tile));
    }

    // Move the fresh tiles into place and remember what the skirt was built for
    s_skirtWorldSpace = worldSpace;
    placeTiles(centerBx, centerBy);
    s_centerBx = centerBx;
    s_centerBy = centerBy;

    // If the map opened while we were building, honor the hidden state immediately
    if (s_mapMenuOpen) {
        s_skirtRoot->SetAppCulled(true);
    }

    spdlog::info("Water skirt built for {}: {} tiles, radius {}, (NAM3 {:08X}, NAM4 {}, flags {:#x}), skirt height {}, "
                 "template {} verts, donor {} [{}] ({} candidates rejected)",
                 worldSpace->GetFormEditorID(),
                 s_tiles.size(),
                 effectiveRadius(),
                 lodWorldSpace->lodWater != nullptr ? lodWorldSpace->lodWater->GetFormID() : 0,
                 lodWorldSpace->lodWaterHeight,
                 worldSpace->flags.underlying(),
                 s_skirtHeight,
                 search.bestVertexCount,
                 search.bestCheck.verdict == DonorVerdict::K_SOLID_WATER ? "verified solid water"
                                                                         : "trusted engine quad",
                 search.bestCheck.detail,
                 search.rejectedCount);
}

void WaterSkirt::queueUpdate()
{
    // Coalesce: only queue a task if one isn't already pending. The actual work
    // runs on the main thread via the SKSE task queue, where scene-graph access
    // is safe.
    if (!s_taskPending.exchange(true)) {
        SKSE::GetTaskInterface()->AddTask([]() -> void {
            s_taskPending = false;
            updateSkirt();
        });
    }
}
