# TAA Correction and Implementation Guide — 2025-09-12

This document diagnoses the purple/green output and outlines a robust TAA pipeline for the volumetric renderer.

## Symptoms
- Enabling TAA yields a magenta/green output instead of red/orange volumetric result.
- TAA path renders volume into an intermediate image, then blends with a history image using reprojection and neighborhood clamping.

## Root causes (from code review)
1) Mixed color formats in the TAA path
   - `m_taaCurrentImage` uses the swapchain format (likely `VK_FORMAT_B8G8R8A8_SRGB`).
   - `m_taaHistoryImage` is `VK_FORMAT_R16G16B16A16_SFLOAT` (linear HDR).
   - You copy sRGB-encoded bytes into a float history and then sample both as linear in the shader. This causes nonlinear mixing and color shifts (magenta/green casts).

2) Reprojection matrix updated too early
   - `m_previousViewProjMatrix` is updated in `renderToTAATarget()` before `renderTAA()`. In `renderTAA()`, `currentToHistory = previous * inverse(current)` becomes identity every frame after the first, so reprojection is ineffective (and any jitter offset isn’t compensated).

3) Minor: current sampler reuse
   - `renderTAA()` binds the current frame with `m_taaHistorySampler`. It’s harmless if sampler settings match, but keep separate samplers if you need different filters.

## Required fixes

### A. Keep the whole TAA chain in linear HDR
- Use `VK_FORMAT_R16G16B16A16_SFLOAT` for both `m_taaCurrentImage` AND `m_taaHistoryImage`.
- Never use an sRGB-typed image inside the TAA pipeline. Tone-map/gamma only when writing to the swapchain.

### B. Correct reprojection for a full‑screen, depthless effect
- For volumetrics (no per-pixel depth), prefer jitter‑only reprojection instead of 4×4 clip reprojection:
  - Maintain per-frame camera jitter `j_curr, j_prev` in NDC/pixel space.
  - Compute `historyUV = fragCoord + (j_prev − j_curr) / screenSize`.
  - If you keep the matrix path, update `m_previousViewProjMatrix` only AFTER TAA has consumed it (i.e., at the end of the frame), so that `currentToHistory` uses last frame’s matrix.

### C. Frame order and barriers (Synchronization2 recommended)
1) Render volume to `taaCurrent (R16G16B16A16)` as color attachment.
2) Barrier: `COLOR_ATTACHMENT_OPTIMAL → SHADER_READ_ONLY_OPTIMAL` for `taaCurrent`.
3) Bind TAA pass (fullscreen): inputs = `taaCurrent` (binding 0), `taaHistory` (binding 1); output = swapchain or an HDR target prior to tone mapping.
4) After TAA, update history:
   - `taaHistory: SHADER_READ → TRANSFER_DST`
   - `taaCurrent: SHADER_READ → TRANSFER_SRC`
   - `vkCmdCopyImage(taaCurrent → taaHistory)`
   - Transition both back to `SHADER_READ`.

### D. Neighborhood clamping details
- Clamp the history sample to the min/max of a 3×3 window from the CURRENT frame around the current pixel (as you do).
- Optional: use luminance‑based or percentile clamp for more stability.
- Blend: `C_out = mix(history_clamped, current, alpha)` with `alpha ≈ 0.1`.

### E. Debug toggles
- Output current, history, clampedHistory, and blendedColor as separate debug modes to confirm color spaces and UV reprojection.
- Add an overlay to show `(j_prev − j_curr)` vectors.

## Likely changes in your code
- `createTAAPipeline()` is fine; keep the pipeline using dynamic rendering.
- `create TAA images`: make both current and history `VK_FORMAT_R16G16B16A16_SFLOAT`.
- `renderToTAATarget()`: move `m_previousViewProjMatrix = viewProj;` to AFTER `renderTAA()/updateTAAHistory()` (end of frame), or compute jitter-only reprojection and drop the matrix.
- `renderTAA()` descriptors: binding 0 uses `currentFrameView`; binding 1 uses `m_taaHistoryImageView`. Sampler reuse is OK if settings match.

## Jitter-only reprojection snippet (concept)
```
vec2 texel = 1.0 / screenSize;
vec2 historyUV = fragCoord + (prevJitter - currJitter) * texel; // clamp to [0,1]
```
- Pass `prevJitter` and `currJitter` via push constants; CP offsets you already compute can serve as jitter sequences.

## Parameter guidance
- α (blend): 0.08–0.12 (lower in noisy shots).
- CP jitter scale for start‑t: 0.5–1.0 × step; fade micro‑jitter in hot cores.
- Neighborhood clamp window: 3×3 (start), expand to 5×5 if needed.

## Validation checklist
- History and current are both linear HDR (float) — no sRGB formats in TAA path.
- Reprojection compensates frame‑to‑frame jitter (historyUV shifts correctly with jitter change).
- Barriers use correct stages/access masks for each transition (prefer `vkCmdPipelineBarrier2`).
- History update runs AFTER the TAA pass and before frame end.

## Useful spec anchors (MCP queries)
- search_vulkan_spec("vkCmdPipelineBarrier2") — Section 7.6 (Synchronization2 pipeline barriers)
- search_vulkan_spec("Blending") — Section 31.1 (tone mapping/bloom combine later)

---

## Common pitfalls causing magenta/green
- Mixing sRGB and linear formats inside the temporal path.
- Copying sRGB data into float history without conversion.
- Updating the previous matrix before consuming it (reprojection = identity).
- Binding the wrong view/sampler or using uninitialized history on frame 2+.

Following the steps above should restore correct color and yield a stable, banding‑free temporal accumulation while preserving the blue‑noise character in hot regions.
