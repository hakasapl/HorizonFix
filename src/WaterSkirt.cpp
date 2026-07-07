#include "WaterSkirt.hpp"

#include "ConfigLoader.hpp"

#include <atomic>
#include <cmath>
#include <limits>
#include <numbers>
#include <vector>

using namespace HorizonFix;

auto WaterSkirt::getLODWorldSpace(RE::TESWorldSpace* worldSpacePtr) -> RE::TESWorldSpace*
{
    while ((worldSpacePtr->parentWorld != nullptr)
           && worldSpacePtr->parentUseFlags.any(RE::TESWorldSpace::ParentUseFlag::kUseLODData)) {
        worldSpacePtr = worldSpacePtr->parentWorld;
    }
    return worldSpacePtr;
}

void WaterSkirt::searchTemplateQuad(RE::NiAVObject* objPtr,
                                    TemplateSearch& search)
{
    if ((objPtr == nullptr) || objPtr->name == K_TILE_NAME) {
        return;
    }

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

    if (auto* const node = objPtr->AsNode()) {
        for (const auto& child : node->children) {
            searchTemplateQuad(child.get(), search);
        }
    }
}

void WaterSkirt::removeSkirt()
{
    for (const auto& tile : s_tiles) {
        if (tile) {
            if (auto* const parent = tile->parent) {
                parent->DetachChild(tile.get());
            }
        }
    }
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

auto WaterSkirt::effectiveRadius() -> float
{
    // clamp to 90% of far plane
    float radius = ConfigLoader::getSkirtRadius();
    if (ConfigLoader::getFarPlaneDistance() > 0) {
        radius = std::min(radius, ConfigLoader::getFarPlaneDistance() * FARPLANE_CLAMP_FACTOR);
    }
    constexpr float CLAMP = 4.0F;
    return std::max(radius, CLAMP * K_TILE_SIZE);
}

void WaterSkirt::buildLayout()
{
    s_layout.clear();

    const float radius = effectiveRadius();
    const float rimStart = radius - (1.5F * K_TILE_SIZE);
    const float fineSize = K_TILE_SIZE / static_cast<float>(K_RIM_SUBDIVISIONS);
    const int span = static_cast<int>(std::ceil(radius / K_TILE_SIZE)) + 1;

    for (int ix = -span; ix <= span; ++ix) {
        for (int iy = -span; iy <= span; ++iy) {
            // Never place tiles around the player: the carpet below the water
            // plane is invisible at distance but not from up close, and the
            // loaded-cell area has real water anyway.
            if (std::abs(ix) <= 1 && std::abs(iy) <= 1) {
                continue;
            }

            const float dx = static_cast<float>(ix) * K_TILE_SIZE;
            const float dy = static_cast<float>(iy) * K_TILE_SIZE;

            const float nearCorner = std::hypot(std::max(std::fabs(dx) - (K_TILE_SIZE * 0.5F), 0.0F),
                                                std::max(std::fabs(dy) - (K_TILE_SIZE * 0.5F), 0.0F));
            if (nearCorner > radius) {
                continue; // entire tile outside the window
            }

            const float farCorner
                = std::hypot(std::fabs(dx) + (K_TILE_SIZE * 0.5F), std::fabs(dy) + (K_TILE_SIZE * 0.5F));
            if (farCorner <= rimStart) {
                s_layout.push_back(RelTile {.dx = dx, .dy = dy, .size = K_TILE_SIZE});
                continue;
            }

            // Rim band: subdivide so the circular edge is smooth; tiles that the
            // boundary itself crosses get one more level of subdivision.
            const float microSize = fineSize / static_cast<float>(K_RIM_SUBDIVISIONS);
            for (int fx = 0; fx < K_RIM_SUBDIVISIONS; ++fx) {
                for (int fy = 0; fy < K_RIM_SUBDIVISIONS; ++fy) {
                    const float fdx = dx - (K_TILE_SIZE * 0.5F) + ((static_cast<float>(fx) + 0.5F) * fineSize);
                    const float fdy = dy - (K_TILE_SIZE * 0.5F) + ((static_cast<float>(fy) + 0.5F) * fineSize);

                    const float fineNear = std::hypot(std::max(std::fabs(fdx) - (fineSize * 0.5F), 0.0F),
                                                      std::max(std::fabs(fdy) - (fineSize * 0.5F), 0.0F));
                    if (fineNear > radius) {
                        continue; // fully outside
                    }
                    const float fineFar
                        = std::hypot(std::fabs(fdx) + (fineSize * 0.5F), std::fabs(fdy) + (fineSize * 0.5F));
                    if (fineFar <= radius) {
                        s_layout.push_back(RelTile {.dx = fdx, .dy = fdy, .size = fineSize});
                        continue;
                    }

                    // Boundary-crossing tile: fill up to the circle with micro tiles.
                    for (int mx = 0; mx < K_RIM_SUBDIVISIONS; ++mx) {
                        for (int my = 0; my < K_RIM_SUBDIVISIONS; ++my) {
                            const float mdx = fdx - (fineSize * 0.5F) + ((static_cast<float>(mx) + 0.5F) * microSize);
                            const float mdy = fdy - (fineSize * 0.5F) + ((static_cast<float>(my) + 0.5F) * microSize);
                            if (std::hypot(mdx, mdy) <= radius) {
                                s_layout.push_back(RelTile {.dx = mdx, .dy = mdy, .size = microSize});
                            }
                        }
                    }
                }
            }
        }
    }
}

void WaterSkirt::placeTiles(int centerBx,
                            int centerBy)
{
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

    auto* const worldSpace = tes->worldSpace;
    if (worldSpace == nullptr) {
        removeSkirt();
        return;
    }

    auto* const root = tes->objLODWaterRoot;
    if (root == nullptr) {
        return;
    }

    auto* const player = RE::PlayerCharacter::GetSingleton();
    if (player == nullptr) {
        return;
    }
    const auto playerPos = player->GetPosition();
    const int centerBx = static_cast<int>(std::floor(playerPos.x / K_TILE_SIZE));
    const int centerBy = static_cast<int>(std::floor(playerPos.y / K_TILE_SIZE));

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

    TemplateSearch search;
    searchTemplateQuad(root, search);
    if (search.best == nullptr) {
        // No LOD water attached yet (or the worldspace has none to clone);
        // retried on the next cell attach event.
        return;
    }
    auto* const templateQuad = search.best;

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

    // Carpet depth below the detected LOD ocean plane. The template quad's
    // world height is the authoritative ocean level of the CURRENT worldspace
    // (Tamriel: 0 — its NAM4 record lies and says -14000), so modded
    // worldspaces with different sea levels get the same relative depth
    // instead of an absolute height that could land above their water.
    const float oceanHeight = templateQuad->worldBound.center.z;
    s_skirtHeight = oceanHeight + ConfigLoader::getSkirtZOffset();

    buildLayout();

    RE::NiUpdateData updateData {};
    s_tiles.reserve(s_layout.size());
    for (std::size_t i = 0; i < s_layout.size(); ++i) {
        auto tile = cloneTemplate(templateQuad);
        if (!tile) {
            break;
        }
        tile->name = K_TILE_NAME;
        tile->SetAppCulled(false);
        root->AttachChild(tile.get(), true);
        tile->Update(updateData);
        s_tiles.emplace_back(std::move(tile));
    }

    s_skirtWorldSpace = worldSpace;
    placeTiles(centerBx, centerBy);
    s_centerBx = centerBx;
    s_centerBy = centerBy;

    auto* const lodWorldSpace = getLODWorldSpace(worldSpace);
    spdlog::info("Water skirt built for {}: {} tiles, radius {}, ocean height "
                 "{} (NAM4 {}), skirt height {}, template {} verts",
                 worldSpace->GetFormEditorID(),
                 s_tiles.size(),
                 effectiveRadius(),
                 oceanHeight,
                 lodWorldSpace->lodWaterHeight,
                 s_skirtHeight,
                 search.bestVertexCount);
}

void WaterSkirt::queueUpdate()
{
    if (!s_taskPending.exchange(true)) {
        SKSE::GetTaskInterface()->AddTask([]() -> void {
            s_taskPending = false;
            updateSkirt();
        });
    }
}
