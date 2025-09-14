# Deep Shadow Map (DSM) System — Deep Analysis & Fixes (2025-09-13)

This document analyzes the current DSM implementation across the following files and provides concrete fixes and validation probes:
- `shaders/dsm_build.comp`
- `shaders/volume.frag`
- `src/systems/VolumeRenderer.cpp` / `VolumeRenderer.h`
- `src/core/Application.cpp` / `Application.h`

MCP spec queries are included for each critical area to cross-check rules and valid usage.

---

## Executive Summary — Root Causes of “flat debug (white/black/red)”
- Compute dispatch and image barriers executed inside an active dynamic rendering instance in the non‑TAA path. This is invalid and causes sync/layout hazards and undefined results.
- DSM descriptors and LightMatrices UBO bindings are conditionally pushed only when RTX is supported in some paths. The shader always expects them; missing bindings = undefined sampling (flat visuals).
- First DSM build uses a barrier with wrong oldLayout (expects SHADER_READ, but image is actually GENERAL after initialization/clear). Layout mismatches create validation noise and unreliable results.
- Light/proj matrix mismatch is unlikely now because you push identical CPU matrices to both compute (push constants) and fragment (UBO), but the fragment shader should still perform perspective divide and frustum checks.
- Visualization path depends on `fragCoord` being a proper [0,1] UV from `volume.vert`.

---

## 1) Prohibit compute/barriers during dynamic rendering

Issue
- `VolumeRenderer::render()` calls `buildDSM(cmd)` while the application is inside `vkCmdBeginRendering` → `vkCmdEndRendering` (non‑TAA path).
- This records compute dispatch and image barriers during an active rendering instance. Vulkan disallows this and drivers can misbehave.

Fix
- Move DSM build outside of any active rendering instance:
  - In `Application::recordCommandBuffer`: before beginning the main rendering instance, call `buildDSM` (or end rendering temporarily, build DSM, begin rendering again) for all paths that need DSM.
  - OR: split the main render into two stages: (A) DSM compute stage (no rendering), (B) graphics rendering stage.

Why (MCP)
- search_vulkan_spec("vkCmdBeginRendering")
- search_vulkan_spec("vkCmdPipelineBarrier2")
- search_vulkan_spec("dynamic rendering commands allowed")

Actionable change (high level)
- Non‑TAA path:
  - End any active rendering (if begun).
  - Call `m_volumeRenderer->buildDSM(cmd)`. Ensure barriers complete.
  - Begin rendering and run `m_volumeRenderer->render(cmd, ...)`.
- TAA path already ends rendering before `renderToTAATarget()`, which also calls `buildDSM(cmd)` → this is valid; keep.

---

## 2) Descriptor bindings consistency (DSM + Light UBO)

Issue
- `volume.frag` always expects:
  - binding 3: DSM `sampler2D`
  - binding 4: `LightMatricesUBO`
- In some code paths, descriptors for binding 3/4 are only pushed if `supportsRayTracing()`. DSM does not require RTX; gating causes missing bindings and flat output.

Fix
- Always include binding 3 and binding 4 in both the descriptor set layout and the push writes for all paths. Do not gate by RTX.
- If you want to gate RTX exclusively, do it only for binding 5 (acceleration structure).

Why (MCP)
- search_vulkan_spec("Images")
- search_vulkan_spec("Image Layout Matching Rules")

Actionable change (high level)
- `createVolumeRenderPipeline()`
  - Keep bindings 0..4 unconditionally (density, STBN, LUT, DSM, Light UBO). Gate only binding 5 by RTX.
- `render()` and `renderToTAATarget()`
  - Unconditionally fill descriptorWrites for 0..4 (DSM + UBO always set).
  - Only append binding 5 (accel struct) if RTX supported.

---

## 3) DSM image layout lifecycle — use correct oldLayout per pass

Issue
- After DSM initialization and `clearDSMToWhite()`, the DSM is left in `GENERAL`.
- `buildDSM()` transitions from `SHADER_READ_ONLY_OPTIMAL` → `GENERAL` (wrong oldLayout on first usage), then writes, then `GENERAL` → `SHADER_READ_ONLY_OPTIMAL` for sampling.
- Mismatched oldLayout causes validation warnings and potential undefined behavior.

Fix
- Track current DSM layout or use the correct oldLayout for the first frame. Safe sequence:
  - After `clearDSMToWhite()`, leave DSM in `GENERAL`.
  - In `buildDSM()` first barrier, set `oldLayout = VK_IMAGE_LAYOUT_GENERAL`.
  - After compute, transition to `VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL`.
  - At the start of the NEXT `buildDSM()`, set `oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL`.

Why (MCP)
- search_vulkan_spec("vkCmdPipelineBarrier2 image layout transition synchronization2")
- search_vulkan_spec("Image Layouts")

Actionable change (high level)
- Maintain a small enum or bool in `VolumeRenderer` to know if DSM was ever sampled:
  - `m_dsmWasSampled`: false at creation. After first transition to SHADER_READ_ONLY_OPTIMAL, set true.
  - In `buildDSM()`, choose oldLayout accordingly.

---

## 4) Fragment shader sampling correctness (volume.frag)

Findings
- `sampleDSM(worldPos)` computes `lightClip = P * V * vec4(worldPos,1)` without perspective divide. Orthographic projection makes w=1 now, but doing the divide is correct and future‑proof.
- No check on `lightClip.z` to ensure point is within the light frustum depth range. While DSM is 2D transmittance, frustum rejection helps avoid sampling garbage.

Fix (minimal)
- Add perspective divide and depth check:
```glsl
vec4 lightClip = lightMatrices.lightProjMatrix * lightMatrices.lightViewMatrix * vec4(worldPos, 1.0);
if (lightClip.w == 0.0) return 1.0;
vec3 ndc = lightClip.xyz / lightClip.w;
if (any(greaterThan(abs(ndc.xy), vec2(1.0))) || ndc.z < -1.0 || ndc.z > 1.0) {
    return 1.0;
}
vec2 lightUV = ndc.xy * 0.5 + 0.5;
float transmittance = texture(dsmTexture, lightUV).r;
...
```
- Replace the hardcoded `lightDir` used in scattering with a direction derived from the light matrices if desired (optional), or keep it identical to CPU `updateLightMatrices()` constant to avoid mismatch.

---

## 5) Compute shader (dsm_build.comp) validation

Findings
- Ray setup uses inverse matrices correctly; AABB intersection clamps marching interval.
- Synthetic density sphere near world origin ensures visual structure if mapping is correct.
- Writes τ as grayscale in R channel (R16F) — good for debug.

Fixes/Checks
- Ensure `pc.stepSize ≈ voxelSize` (0.5–1.5×), `pc.maxDistance` ≈ AABB diagonal, and `densityScale` coherent with volume opacity.
- Keep miss case writing RED and early return. Keep τ grayscale for a few frames to visually assert variation. When validated, switch to `T = exp(-tau)`.

Why (MCP)
- search_vulkan_spec("Images") — storage image formats, writes

---

## 6) Visualization path sanity

Findings
- `renderDSMDebug()` sets `push.densityScale < 0` → `volume.frag` shows DSM texture.
- This depends on `fragCoord` being a valid [0,1] UV. If `volume.vert` does not generate that correctly, the debug view can appear flat.

Fix
- Temporary UV gradient test in debug branch of `volume.frag`:
```glsl
if (push.densityScale < 0.0) {
    vec3 uvTest = vec3(fragCoord, 0.0);
    // Uncomment to visualize uv gradient
    // fragColor = vec4(uvTest, 1.0); return;
    float dsmValue = texture(dsmTexture, fragCoord).r;
    fragColor = vec4(dsmValue, dsmValue, dsmValue, 1.0);
    return;
}
```
- If the gradient is not visible, fix `volume.vert` to output `fragCoord` in [0,1].

---

## 7) Resize handling

Findings
- DSM resources are not explicitly recreated on swapchain resize. While DSM is decoupled from swapchain dimensions (fixed 1024²), the light matrices and any dependent logic should be recomputed and DSM rebuilt after resize.

Fix
- On swapchain resize (`Application::recreateSwapChain` / renderer path):
  - Call `updateLightMatrices()` and a one‑time `clearDSMToWhite()`.
  - Rebuild DSM before the first render after resize.

---

## 8) Concrete validation probes (do these in order)

1) Move DSM build out of dynamic rendering (Section 1). Validate no more validation errors about illegal commands during rendering.
2) Always bind DSM (binding 3) and LightMatrices UBO (binding 4). Remove RTX gating for these (Section 2).
3) Fix DSM oldLayout selection in first frame (Section 3). Validation layer should quiet down.
4) Enable τ grayscale output in `dsm_build.comp` and toggle DSM debug view. You must see non‑flat structure (bright/dark regions). If not:
   - Toggle UV gradient check in `volume.frag` to confirm `fragCoord` correctness.
   - Force synthetic sphere density ON and verify circular imprint appears.
5) Switch back to transmittance `T = exp(-tau)` and verify shadows affect scene brightness via `sampleDSM`.
6) Add perspective divide + frustum check in `sampleDSM` (Section 4). Confirm sampling only within light frustum.

---

## 9) Parameter ranges to try (fast iteration)
- `pc.stepSize`: `voxelSize * {0.75, 1.0, 1.25}`
- `pc.maxDistance`: `length(diagonal(AABB))`
- `densityScale`: tuned so that typical τ in the dense ring is ~1–6
- In volume.frag: `shadowStrength` in [1.0, 4.0]

---

## 10) Optional quality/perf upgrades after landing DSM
- Epipolar/light‑aligned forward samples: 2–3 visibility taps along lightDir per view step; blend with DSM visibility to emphasize shafts.
- Ray‑query occluders (BH/disk geometry) and multiply visibility with DSM.

MCP
- search_vulkan_spec("VK_KHR_ray_query")
- search_vulkan_spec("VK_KHR_acceleration_structure")

---

## Appendix — MCP queries used
- search_vulkan_spec("vkCmdPipelineBarrier2 image layout transition synchronization2")
- search_vulkan_spec("Image Layouts")
- search_vulkan_spec("Images")
- search_vulkan_spec("vkCmdBeginRendering")
- search_vulkan_spec("dynamic rendering commands allowed")
- search_vulkan_spec("VK_KHR_ray_query")
- search_vulkan_spec("VK_KHR_acceleration_structure")

---

## Short checklist
- [ ] DSM compute called only outside dynamic rendering instances.
- [ ] DSM + LightMatrices UBO bound every frame (no RTX gating for 3/4).
- [ ] DSM layout transitions use correct oldLayout per pass.
- [ ] DSM debug shows τ grayscale variation; UV gradient verified.
- [ ] Fragment sampling uses perspective divide and frustum checks.
- [ ] Resize triggers light matrices update, DSM clear, and rebuild before draw.
