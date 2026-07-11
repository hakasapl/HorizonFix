#include "WaterSkirt.hpp"

#include "ConfigLoader.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <numbers>
#include <vector>

using namespace HorizonFix;

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
    planes[0] = planeThrough(dir, pos + (dir * frustum.fNear));
    planes[1] = planeThrough(-dir, pos + (dir * frustum.fFar));

    // Orthographic: the side planes are parallel to the view direction, offset
    // by the frustum extents along the right/up axes
    if (frustum.bOrtho) {
        planes[2] = planeThrough(right, pos + (right * frustum.fLeft));
        planes[3] = planeThrough(-right, pos + (right * frustum.fRight));
        planes[4] = planeThrough(-up, pos + (up * frustum.fTop));
        planes[5] = planeThrough(up, pos + (up * frustum.fBottom)); // NOLINT(cppcoreguidelines-avoid-magic-numbers)
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
    planes[2] = slopePlane(right, frustum.fLeft, 1.0F);
    planes[3] = slopePlane(right, frustum.fRight, -1.0F);
    planes[4] = slopePlane(up, frustum.fTop, -1.0F);
    planes[5] = slopePlane(up, frustum.fBottom, 1.0F); // NOLINT(cppcoreguidelines-avoid-magic-numbers)
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
            search.best = shape;
            search.bestVertexCount = vertexCount;
            search.bestRadius = radius;
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

    const std::size_t count = std::min(s_tiles.size(), s_layout.size());
    for (std::size_t i = 0; i < count; ++i) {
        const auto& tile = s_tiles[i];
        if (!tile) {
            continue;
        }
        const auto& rel = s_layout[i];

        // True tile bound; the radius (half-diagonal is size * 0.71) carries
        // ~40% slack for the camera moving until the next update.
        RE::NiBound bound {};
        bound.center = RE::NiPoint3 {centerX + rel.dx, centerY + rel.dy, s_skirtHeight};
        bound.radius = rel.size;

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
        const bool visible = boundInFrustum(bound, planes);
        if (tile->flags.any(RE::NiAVObject::Flag::kHidden) == visible) {
            tile->SetAppCulled(!visible);
        }
    }
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

    // Walk the square of candidate blocks and let layoutTile trim it to a disc
    for (int ix = -span; ix <= span; ++ix) {
        for (int iy = -span; iy <= span; ++iy) {
            // Leave the central 3x3 blocks empty: the game's own LOD water
            // covers the area around the player
            if (std::abs(ix) <= 1 && std::abs(iy) <= 1) {
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
        const auto& tile = s_tiles[i];
        if (!tile) {
            continue;
        }
        const auto& rel = s_layout[i];

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

    // The tiles sit at the LOD water height (from whichever worldspace owns the
    // LOD data), pushed down by the configured Z offset to avoid z-fighting with
    // the real water
    auto* const lodWorldSpace = getLODWorldSpace(worldSpace);
    s_skirtHeight = lodWorldSpace->lodWaterHeight + ConfigLoader::getSkirtZOffset();

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
    if (ConfigLoader::getWaterDrawLast() && (tes->lodLandRoot != nullptr)) {
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

    spdlog::info("Water skirt built for {}: {} tiles, radius {}, (NAM4 {}), skirt height {}, template {} verts",
                 worldSpace->GetFormEditorID(),
                 s_tiles.size(),
                 effectiveRadius(),
                 lodWorldSpace->lodWaterHeight,
                 s_skirtHeight,
                 search.bestVertexCount);
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
