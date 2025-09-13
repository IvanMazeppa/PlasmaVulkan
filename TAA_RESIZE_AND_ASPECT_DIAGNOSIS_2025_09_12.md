# TAA Resize/Aspect Crash & Artifact Diagnosis — 2025-09-12

## Symptoms
- Window maximize or any increase in width/height: crash.
- When the window is made smaller, the image shows repeated/scaled copies constrained to the top‑left; visible aspect correction in a small region.

## Cause
The TAA render targets are created once at startup and are not recreated when the swapchain is resized. Code also mixes `swapchainExtent` with TAA image extents for rendering, sampling, and copy operations.

Consequences:
- On enlarge, `vkCmdBeginRendering` uses `renderArea = swapchainExtent` while the color attachment is `m_taaCurrentImage` with the old (smaller) extent → writes beyond bounds → driver/validation crash.
- The TAA shader uses `taa.screenSize = swapchainExtent`, but `currentFrame`/`historyFrame` still use the old sizes → UVs/texelSize mismatch, causing the tiled/misaligned top‑left output.
- `updateTAAHistory()` copies with `copyRegion.extent = swapchainExtent` regardless of actual TAA image size → out‑of‑bounds copy on resize.

## Fix checklist
1) Recreate TAA resources on swapchain resize
   - Add `VolumeRenderer::onSwapchainResized(VkExtent2D newExtent)`.
   - Recreate:
     - `m_taaCurrentImage` (+ view + sampler)
     - `m_taaHistoryImage` (+ view + sampler)
   - Initialize history to black again.
   - Set `m_taaFirstFrame = true` and reset `m_previousViewProjMatrix`.

2) Use TAA image extent consistently
   - In `renderToTAATarget()` and `renderTAA()`, use the TAA image extent (not `swapchainExtent`) for:
     - `VkRenderingInfo.renderArea.extent`
     - Push constant `screenSize`
     - `copyRegion.extent` in `updateTAAHistory()`
   - If you intend to run TAA at a reduced resolution (recommended), always pass and use that reduced extent consistently.

3) Barriers after resize
   - After recreating images, perform: UNDEFINED → TRANSFER_DST, clear to black, then → SHADER_READ_ONLY_OPTIMAL for history; and UNDEFINED → COLOR_ATTACHMENT_OPTIMAL for current.

4) Present path
   - If you blit/compose the TAA result to the swapchain, ensure you handle scaling explicitly (sampling with correct UVs in a composite pass or `vkCmdBlitImage` with matching src/dst rectangles). Do not assume same resolution.

5) Shader constants
   - Set `taa.screenSize` to the TAA current image size used for sampling (not the swapchain size) so `texelSize = 1/screen` matches the texture.
   - If you switch to jitter‑only reprojection, also scale jitter by this TAA size.

6) Safety
   - After resize, history is invalid → keep `m_taaFirstFrame = true` for one frame to bypass history.
   - Guard copies with asserts/validations that extents match.

## Minimal integration points
- `Application::recreateSwapChain()` → call `m_volumeRenderer->onSwapchainResized(m_vulkanContext->getSwapChainExtent())` after the new swapchain is ready.
- `VolumeRenderer::onSwapchainResized()` → destroys old TAA images/views/samplers; calls `createTAAResources()` with the new extent; resets flags/matrices; clears history.
- Replace all uses of `getSwapChainExtent()` in TAA passes with `getTAAExtent()` (store it when creating images).

## Validation checklist after changes
- No crashes on maximize; validation shows no OOB copy/render.
- The TAA image fills the entire window even when resized; no top‑left tiling.
- `taa.screenSize` equals `TAAExtent` in the shader.
- History bypasses one frame after resize and then resumes accumulation.

Following this, combine with the color‑space/reprojection fixes in `TAA_CORRECTION_AND_IMPLEMENTATION_GUIDE_2025_09_12.md` for a stable TAA pipeline.
