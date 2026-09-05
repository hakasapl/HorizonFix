#pragma once

#include "PCH.h"

#include <array>
#include <atomic>
#include <cstddef>
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
 * the seam line, and optionally held opaque for a configurable share of the height above
 * it - fHorizonBlendOpaquePercent - to blot out a thin bright sky strip that sits right on
 * the horizon line) and tinted every frame in the color the distant water renders in (see
 * s_bandColor). The band carries ONLY the water color: the opaque seam row covers the
 * hard line, and above it the fade lets the real sky show through, so the water dissolves
 * into whatever sky is actually behind it - right at every azimuth by construction, even
 * when a renderer paints the sky warm toward the sun and cool away from it. No sky color
 * is read or matched: a band that painted a sky color could only ever be one color around
 * the whole ring, while the sky it has to meet is not.
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
    static constexpr int K_ROWS
        = 15; /**< Vertex rows for the vertical gradient. MUST BE ODD so one row lands exactly on the seam
                 (t = 0) at full alpha; the rows above the seam are re-spaced when an opaque plateau is
                 configured (see buildArcGeometry). The profile between rows is linear, and the eye amplifies the slope
                 breaks at row joints into visible bands (Mach banding) - more rows push the joints below
                 visibility at typical blend angles. The remaining banding in very shallow ramps is 8-bit
                 render-target quantization (the same effect as the vanilla sky's banded gradients), which
                 vertex data cannot dither away. */
    static_assert(K_ROWS % 2 == 1, "a row must land exactly on the seam line");

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
    static_assert(K_SEGMENTS % K_ARCS == 0,
                  "arcs must tile the ring exactly");
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

    // Band tint, refreshed each frame: the color the distant water renders in. Its prior
    // is the sky's FOG FAR color - what distant water converges to as its fog saturates,
    // in vanilla and under CS Unified Water alike - trimmed by the closed-loop correction
    // below until the band matches the water it stands on. Every row carries this one
    // color; the vertical shape of the blend is the alpha gradient alone. Below the seam
    // the band paints matched water over water (invisible once converged, and what the
    // loop measures); above the seam it fades out over the real sky. The color rides the
    // vertex colors of a DYNAMIC vertex buffer: updateFrame (game side) computes
    // s_bandColor and bumps s_tintStamp; the SetupGeometry hook (context-owning thread,
    // the only place mapping the buffer is safe) rewrites each arc's buffer when its
    // stamp is stale. A torn read of a color float costs at most one frame of an
    // imperceptibly wrong hue, so only the stamp itself is atomic.
    static inline RE::NiColor s_bandColor {}; /**< Tint of every band vertex this frame */
    static inline std::atomic<std::uint32_t> s_tintStamp {0}; /**< Bumped when s_bandColor changes */
    static inline std::array<std::uint32_t, K_ARCS>
        s_arcTintStamps {}; /**< s_tintStamp value each arc's buffer was last written with */
    static inline RE::BSFixedString s_bandName; /**< Interned K_SHAPE_NAME for cheap per-pass comparison */

    // Automatic color matching of the band tint. The fog-far color is only a prior: what
    // the water ACTUALLY renders depends on the whole pipeline - fog clamp residuals, CS
    // Unified Water, ENB, per-surface transforms like CS Linear Lighting's gammas, and the
    // effect shader's own fog on the band itself. The render hooks measure reality
    // instead: right before the band draws, the framebuffer under its water row still
    // shows pure water - that is the color to match; right after, it shows the band's
    // blend. Small regions at sample rays on that row are copied around the band's draws
    // into a staging ring, read back a few frames later (no GPU stall), and the
    // per-channel differences feed an integral correction on the tint until the on-screen
    // difference is zero. Rays occluded by closer geometry self-cancel: the depth test
    // already kept the band from drawing there, so pre and post are identical.
    static constexpr int K_MATCH_SAMPLES = 5; /**< Sample rays, spread across the view's horizon */
    static constexpr float K_MATCH_AZIMUTH_STEP = 20.0F; /**< Degrees between adjacent sample rays */
    static constexpr float K_MATCH_ROW_T = -0.35F; /**< Sample row's band parameter: below the seam, alpha ~0.7 */
    static constexpr int K_MATCH_RING = 4; /**< Staging slots in flight; reads lag captures by 3 frames */
    static constexpr float K_MATCH_GAIN = 0.08F; /**< Per-frame integral gain */
    static constexpr float K_MATCH_MIN = 0.1F; /**< Correction clamps (gamma-scale headroom for LL) */
    static constexpr float K_MATCH_MAX = 3.0F;

    /**
     * @brief One in-flight capture: a K_MATCH_SAMPLES x 2 staging texture (row 0 = before
     * the band drew, row 1 = after) tagged with the frame it belongs to
     */
    struct MatchSlot {
        REX::W32::ID3D11Texture2D* staging = nullptr; /**< Ref held by the slot */
        std::uint32_t frame = 0; /**< s_tintStamp value of the captures */
        std::array<std::array<float, 2>, K_MATCH_SAMPLES>
            uv {}; /**< Snapshot of s_sampleUV at pre-capture, so pre, post, and readback
                      all use the same pixels even while the game thread moves the points */
        bool hasPre = false; /**< Row 0 holds this frame's pre-band copy */
        bool hasPost = false; /**< Row 1 holds this frame's post-band copy */
    };
    static inline std::array<MatchSlot, K_MATCH_RING> s_matchSlots {};
    static inline std::uint32_t s_matchFormat = 0; /**< DXGI format the staging textures match */
    static inline std::uint32_t s_matchCaptureFrame = 0; /**< Frame of the last pre-capture (render side only) */
    static inline bool s_matchDisabled = false; /**< Loop off for this session (VR, MSAA, exotic format) */
    static inline std::array<std::array<float, 2>, K_MATCH_SAMPLES>
        s_sampleUV {}; /**< Sample points in 0-1 viewport coordinates; x < 0 = invalid. Game side writes */
    static inline std::atomic<bool> s_samplesValid {false}; /**< Any sample point usable this frame */
    static inline std::array<std::atomic<float>, 3> s_waterCorrection {
        1.0F,
        1.0F,
        1.0F}; /**< Per-channel multiplier the loop applies to the fog-far prior */

    /**
     * @brief Hook for BSEffectShader::SetupGeometry: refreshes a band arc's vertex colors
     * and captures the pre-band framebuffer samples before it draws (see s_bandColor and
     * MatchSlot); every other effect pass is untouched
     */
    struct SetupGeometryHook {
        static void thunk(RE::BSShader* shaderPtr,
                          RE::BSRenderPass* passPtr,
                          std::uint32_t renderFlags);

        static inline REL::Relocation<decltype(thunk)> s_func; /**< Original function, called by the thunk */
        static constexpr std::size_t K_INDEX = 0x6; // BSShader::SetupGeometry
    };

    /**
     * @brief Hook for BSEffectShader::RestoreGeometry: captures the post-band framebuffer
     * samples after a band arc drew (the last arc's capture wins; see MatchSlot)
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
     * @brief Installs the vertex-color refresh hook on the BSEffectShader vtable
     *
     * Called at plugin load, only when fHorizonBlendDegrees > 0 - a disabled feature
     * leaves the effect shader untouched.
     */
    static void installHooks();

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
     * the tangent-ratio math), and publishes this frame's band color (the corrected
     * fog-far prior, see s_bandColor) for the render hook to upload.
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
     * lives in the vertex colors: smoothstep from 0 at the bottom rim to 255 on the seam
     * row, held at 255 for opaqueFraction of the height above the seam, then smoothstep to
     * 0 at the top rim. The arc's tight model bound is returned for the sort/cull behavior
     * described at K_ARCS.
     *
     * @param arcIndex Which of the K_ARCS arcs to build (0-based)
     * @param blendDegrees Blend angle in degrees (already validated > 0)
     * @param opaqueFraction Share (0-1) of the height above the seam kept fully opaque before the fade
     * @param boundOut Receives the arc's model-space bounding sphere
     * @return RE::BSGraphics::TriShape* Geometry carrying one reference (the caller's), or nullptr on failure
     */
    static auto buildArcGeometry(int arcIndex,
                                 float blendDegrees,
                                 float opaqueFraction,
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

    /**
     * @brief Whether a render pass draws the band (matched by geometry name)
     *
     * @param passPtr Render pass to inspect
     * @return bool True if the pass geometry is the band
     */
    static auto isBandPass(RE::BSRenderPass* passPtr) -> bool;

    /**
     * @brief Rewrites one arc's vertex colors from s_bandColor (see its comment)
     *
     * Runs from the SetupGeometry hook on the thread that owns the D3D context: rewrites
     * the RGB bytes of the CPU vertex copy (alpha profile untouched) and uploads the whole
     * buffer with a discard map. No-op while the arc's stamp matches s_tintStamp.
     *
     * @param arcShape The arc whose pass is being set up
     * @param arcIndex Index of the arc in s_arcs (for the per-arc stamp)
     */
    static void refreshArcColors(RE::BSTriShape* arcShape,
                                 std::size_t arcIndex);

    /**
     * @brief Get the renderer's immediate D3D11 device context
     *
     * @return REX::W32::ID3D11DeviceContext* The context, or nullptr if the renderer is not up
     */
    static auto getContext() -> REX::W32::ID3D11DeviceContext*;

    /**
     * @brief Projects the color-match sample points onto the screen (see MatchSlot)
     *
     * Runs game-side from updateFrame with the same camera the frame renders with. Sample
     * rays fan out across the camera's horizontal forward at the band's radius, on the
     * band row just below the seam (K_MATCH_ROW_T), where the framebuffer under the band
     * shows pure water. Points behind the camera or outside the safe viewport region are
     * marked invalid;
     * VR disables the loop entirely (two eyes, one projection).
     *
     * @param camera The world root camera
     * @param scale The band's world radius this frame
     * @param bandZ World Z of the band's center (seam) line
     * @param halfHeightWorld The band's world half-height this frame
     */
    static void computeSampleUVs(const RE::NiCamera* camera,
                                 float scale,
                                 float bandZ,
                                 float halfHeightWorld);

    /**
     * @brief Copies the framebuffer at the sample points into this frame's staging slot
     *
     * Render side. Pre-band captures (post = false) start a slot for s_matchCaptureFrame;
     * post-band captures fill its second row and only run when the pre capture happened.
     * Auxiliary renders (cubemap faces) are skipped by size; MSAA or an undecodable format
     * disables the loop for the session.
     *
     * @param post False before the band draws (row 0), true after (row 1)
     */
    static void captureMatchSamples(bool post);

    /**
     * @brief Reads back the oldest completed staging slot and updates s_waterCorrection
     *
     * Render side, once per frame. Maps with DO_NOT_WAIT (a still-busy copy is simply
     * skipped), averages the per-channel pre/post difference over the valid samples, and
     * integrates it into the correction with K_MATCH_GAIN.
     *
     * @param frame The current frame stamp; the slot from frame - (K_MATCH_RING - 1) is read
     */
    static void consumeMatchSlot(std::uint32_t frame);

    /**
     * @brief Decodes one texel of a supported render-target format to RGB
     *
     * @param texel Pointer to the texel bytes
     * @param format DXGI format of the texture
     * @param out Receives the decoded color
     * @return bool False when the format is not supported
     */
    static auto decodeTexel(const std::uint8_t* texel,
                            std::uint32_t format,
                            RE::NiColor& out) -> bool;

    /**
     * @brief Bytes per texel of a supported format, or 0 when unsupported
     *
     * @param format DXGI format to look up
     * @return std::uint32_t The texel size in bytes
     */
    static auto texelSize(std::uint32_t format) -> std::uint32_t;

    /**
     * @brief Permanently disables the color-match loop for this session (logged once)
     *
     * The correction freezes at its current value; the band keeps rendering the fog-far
     * prior with that frozen trim.
     *
     * @param reason Short reason for the log
     */
    static void disableMatch(const char* reason);
};

} // namespace HorizonFix
