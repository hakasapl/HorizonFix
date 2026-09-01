#pragma once

#include "PCH.h"

#include <cstdint>
#include <vector>

namespace HorizonFix {

/**
 * @brief Blends the water into the sky at the horizon, independent of the active weather
 *
 * Weather mods traditionally hide the hard water-sky seam with a horizon cloud layer, which
 * only works for weathers that ship one. This class fabricates the same effect natively: a
 * camera-following ring of translucent unlit geometry just inside the far clip plane,
 * split into arcs so the translucent pass sorts it as the far backdrop it is (see K_ARCS),
 * vertically centered each frame on the seam line as seen from the camera (the skirt's
 * outer rim), with a vertical alpha gradient (transparent at the top and bottom, opaque on
 * the seam line) and its color re-tinted every frame from the sky's live horizon color -
 * so it matches any weather mod, time of day, or transition automatically.
 *
 * The object graph (BSTriShape + BSEffectShaderProperty + NiAlphaProperty) comes from a
 * shipped donor NIF so the engine's own model loader constructs it correctly on every
 * runtime; only the renderer vertex/index buffers are replaced at runtime with the ring,
 * built once per session at unit radius. Per frame the ring is repositioned to the camera,
 * uniformly scaled from the live far clip distance (radius and height are both linear in
 * it), and tinted. It renders in the effect pass after water and sky with depth test on and
 * depth write off, so it hazes exactly what lies behind it at the horizon.
 *
 * Lifecycle is tied to the water skirt: ensure() is called whenever the skirt exists (so
 * worldspace gating is inherited), the band attaches under the skirt root (so the map-menu
 * hide and teardown come for free), and updateFrame() runs from the same per-frame hook as
 * the skirt's culling. The whole feature is off unless fHorizonBlendDegrees > 0.
 */
class HorizonBand {
public:
    static constexpr const char* K_SHAPE_NAME = "HSF_HorizonBand"; /**< Tri shape name inside the donor NIF */

private:
    static constexpr const char* K_MODEL_PATH
        = "HorizonFix\\HorizonBand.nif"; /**< Donor NIF path, relative to Data/Meshes */

    static constexpr int K_SEGMENTS = 96; /**< Ring segments; radius error of the polygon is ~0.05% */
    static constexpr int K_ROWS = 7; /**< Vertex rows for the vertical alpha gradient */

    // The ring is split into this many arc tri shapes, each with a tight bound centered on
    // the arc itself. As one object the band's bound center would ride the camera (the ring
    // is camera-centered), and the engine's translucent pass sorts back-to-front by bound
    // distance - a camera-centered band sorts as the CLOSEST translucent in the scene and
    // draws over snow drifts, precipitation, and mist, with the winner flipping as the
    // camera height changes the tiny distances involved (field-observed as a color step
    // when crossing a certain height, amplified by drift effects crossing the camera).
    // Per-arc bounds sit ~0.9x the band radius from the camera, so every arc honestly
    // sorts as the farthest translucent and everything nearer composites over it.
    static constexpr int K_ARCS = 8; /**< Arc tri shapes forming the ring; divides K_SEGMENTS evenly */
    static_assert(K_SEGMENTS % K_ARCS == 0, "arcs must tile the ring exactly");
    static constexpr float K_FARCLIP_FRACTION
        = 0.9F; /**< The band's farthest point (its top/bottom rim) sits at this fraction of the far clip */

    static inline RE::NiPointer<RE::NiNode> s_model; /**< Demanded donor model root; loaded once per session */
    static inline std::vector<RE::NiPointer<RE::BSTriShape>>
        s_arcs; /**< The ring's arc tri shapes: the donor shape plus its clones. Each carries its OWN shader/alpha
                   property and material (geometry cloning deep-copies them - which is also what lets every arc hold
                   its own render-pass list), so configuration and the per-frame tint must be applied per arc */
    static inline float s_waterHeight = 0.0F; /**< World Z the band's waterline sits at */
    static inline float s_builtDegrees = 0.0F; /**< Blend angle the current ring geometry was built for */
    static inline bool s_loadFailed = false; /**< Donor NIF failed to load; stop retrying for this session */
    static inline bool s_loggedFirstFrame = false; /**< One-shot diagnostic log on the first frame update */

public:
    /**
     * @brief Builds and attaches the band, or re-attaches it after a skirt rebuild
     *
     * Called from WaterSkirt::updateSkirt whenever the skirt exists, on the main thread.
     * No-op when fHorizonBlendDegrees is 0 (the default). The first call demands the donor
     * NIF and swaps in the fabricated ring geometry; later calls only re-attach the model
     * under the (possibly recreated) skirt root and update the waterline height.
     *
     * @param skirtRoot The skirt's root node; the band attaches under it
     * @param waterHeight World Z of the water plane the band stands on
     */
    static void ensure(RE::NiNode* skirtRoot,
                       float waterHeight);

    /**
     * @brief Detaches the band from the scene (skirt teardown); the loaded model is kept for reuse
     */
    static void detach();

    /**
     * @brief Per-frame follow, scale, and tint; called from WaterSkirt::updateVisibility
     *
     * Moves the band to the camera's XY, scales it from the live far clip distance, centers
     * it vertically on the water-sky seam's depression angle (see the implementation for
     * the tangent-ratio math), and writes the sky's current horizon color into every arc's
     * effect material.
     *
     * @param camera The world root camera for this frame
     */
    static void updateFrame(const RE::NiCamera* camera);

private:
    /**
     * @brief Loads the donor NIF and swaps in the fabricated ring geometry (once per session)
     *
     * @return bool True when s_model/s_arcs are ready to attach
     */
    static auto loadModel() -> bool;

    /**
     * @brief Re-asserts every shader, alpha, and material setting one arc relies on
     *
     * The donor NIF carries the same settings, but asserting them from code keeps the NIF
     * non-tuning-critical and survives edits to the shipped file. Runs on EVERY arc: the
     * clones deep-copied their properties and materials, so nothing configured on the donor
     * reaches them (field-observed as per-segment color seams when only the donor was
     * configured). Also makes each material unique so the per-frame tint can never leak
     * into a pooled material shared with other effect geometry.
     *
     * @param arcShape The arc whose property, material, and alpha settings to assert
     */
    static void configureArc(RE::BSTriShape* arcShape);

    /**
     * @brief Builds the renderer geometry of one ring arc for the given blend angle
     *
     * The ring has radius 1 and half-height tan(angle) in model space, so a single uniform
     * world scale derived from the far clip sizes both together and the blend angle seen
     * from the camera stays exactly the configured value. Vertex layout is full-precision
     * position + UV + color (the same descriptor the donor NIF declares); the alpha gradient
     * lives in the vertex colors, smoothstep-shaped from 0 at both rims to 255 at the
     * center row. The arc's tight model bound is returned for the sort/cull behavior
     * described at K_ARCS.
     *
     * @param arcIndex Which of the K_ARCS arcs to build (0-based)
     * @param blendDegrees Blend angle in degrees (already validated > 0)
     * @param boundOut Receives the arc's model-space bounding sphere
     * @return RE::BSGraphics::TriShape* Geometry carrying one reference (the caller's), or nullptr on failure
     */
    static auto buildArcGeometry(int arcIndex,
                                 float blendDegrees,
                                 RE::NiBound& boundOut) -> RE::BSGraphics::TriShape*;

    /**
     * @brief Installs arc renderer geometry on a tri shape, releasing what it carried
     *
     * @param shape Tri shape to point at the arc (the donor shape or a clone of it)
     * @param arcData Renderer geometry from buildArcGeometry; the shape takes over its reference
     * @param bound Model-space bound of the arc
     */
    static void installArcOnShape(RE::BSTriShape* shape,
                                  RE::BSGraphics::TriShape* arcData,
                                  const RE::NiBound& bound);

    /**
     * @brief Drops one reference on renderer geometry, freeing it when the count hits zero
     *
     * Mirror of the engine's own release path: the last holder Releases both D3D buffers and
     * frees the CPU copies and the struct through the game allocator.
     *
     * @param dataPtr Geometry to release (tolerates nullptr)
     */
    static void releaseGeometryData(RE::BSGraphics::TriShape* dataPtr);

    /**
     * @brief Encodes a float as IEEE 754 half precision (for the UV vertex attribute)
     *
     * @param value Value to encode; the band only stores values in [0, 1]
     * @return std::uint16_t The half bit pattern
     */
    static auto floatToHalf(float value) -> std::uint16_t;
};

} // namespace HorizonFix
