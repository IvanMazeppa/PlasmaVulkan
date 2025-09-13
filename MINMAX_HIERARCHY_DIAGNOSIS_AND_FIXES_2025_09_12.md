# Min/Max Hierarchy — Diagnosis & Fixes (2025-09-12)

## What you see
- Enabling volumetric → volume invisible but GPU time suggests work done.
- Validation floods on enable/resize.

## Root problems found
1) Storage image view covers ALL mips; GLSL cannot select LOD for `image3D`
- In `createMinMaxDownsamplePipeline()` bindings always use `m_minMaxImageView` with `levelCount = m_densityMipLevels`.
- For storage images the view’s `baseMipLevel` determines the mip accessed; `imageLoad` has no LOD argument.
- Result: reads/writes hit mip 0 for every dispatch → hierarchy never built; skip tests think everything is empty → invisible volume.

2) First-level source format mismatch
- Shader `minmax_downsample.comp` declares `layout(binding=0, r16f) readonly image3D sourceImage;`
- Density is `VK_FORMAT_R32_SFLOAT`. Reading density through `r16f` is undefined.

3) Usage/layout mismatches and barriers inside dynamic rendering
- TAA/volume paths transition images with `vkCmdPipelineBarrier` while inside `vkCmdBeginRendering` (VUID 09553/09554/09556/01181).
- TAA current image lacks `TRANSFER_SRC` usage but is copied (VUID 06662).
- Descriptors are written with layouts that don’t match actual image layouts at use (VUID 09600/08114).

4) Extent mismatches on resize
- TAA/volume code uses `swapchainExtent` while intermediate images keep old sizes → OOB and tiling in top-left.

## Fix plan (min/max hierarchy)
A) Bind per‑mip views for source and target
- Create/cached views per mip (levelCount=1):
  - `getStorageView(image, mip)` → `baseMipLevel=mip, levelCount=1`.
- For mip N→N+1 pass:
  - Source view = density (mip 0) for first pass, or minmax (mip N) afterward.
  - Target view = minmax (mip N+1).
- Push descriptors for those two views for each dispatch.

B) Correct shader interfaces
- Use two bindings to avoid format aliasing:
```glsl
layout(binding=0, r32f) readonly  image3D densityImage;   // only for sourceMipLevel==0
layout(binding=1, rg16f) readonly  image3D minMaxSrc;     // source for mip>0
layout(binding=2, rg16f) writeonly image3D minMaxDst;     // target
```
- Select between `densityImage` and `minMaxSrc` based on `sourceMipLevel`.

C) Image creation/usage
- Min/Max image: `VK_FORMAT_R16G16_SFLOAT`, usage: `STORAGE | SAMPLED | TRANSFER_DST` (if you clear), optional `TRANSFER_SRC` if you plan to blit.
- Views for storage must have `levelCount=1`.

D) Barriers (Synchronization2 preferred)
- Between mip levels (compute→compute):
  - `srcStage=COMPUTE_SHADER`, `srcAccess=SHADER_WRITE`; `dstStage=COMPUTE_SHADER`, `dstAccess=SHADER_READ` on the source mip.
  - Transition only the specific subresource (mip N) to `GENERAL` before read/write; keep target mip in `GENERAL` for write.
- After building all mips and before graphics sampling:
  - Transition the whole min/max image to `SHADER_READ_ONLY_OPTIMAL`.

E) Volume shader usage
- When testing emptiness, sample max from the min/max texture at the cone LOD; if `max<threshold`, skip ahead; otherwise descend.
- Add a debug view to show `max` as grayscale to confirm hierarchy is populated.

## Fix plan (validation errors you appended)
- Move ALL layout transitions and copies OUTSIDE any `vkCmdBeginRendering`/`vkCmdEndRendering` block (or enable `VK_KHR_dynamic_rendering_local_read`).
- Give TAA current image `VK_IMAGE_USAGE_TRANSFER_SRC_BIT` if you copy from it.
- Ensure descriptor `imageLayout` matches the real layout at use:
  - storage images: `GENERAL` during compute
  - sampled images: `SHADER_READ_ONLY_OPTIMAL` during graphics
- On swapchain resize, recreate TAA/minmax images to the new extent, reset history, and use that extent consistently for render areas, push constants, and copy regions.

## Quick checklist
- [ ] Per‑mip storage views used for source/target mip in compute
- [ ] Shader bindings: density r32f (mip 0), minmax rg16f for others
- [ ] Min/Max image usage includes STORAGE|SAMPLED (+TRANSFER_* if needed)
- [ ] Barriers use COMPUTE↔COMPUTE and occur outside dynamic rendering when needed
- [ ] After compute: min/max image → SHADER_READ before sampling
- [ ] Resize path recreates TAA/minmax images and clears history
- [ ] Debug: visualize `max` at several mips; verify nonzero population

This set resolves the invisible volume (empty max) and removes the validation cascade tied to dynamic rendering barriers and usage/layout mismatches.
