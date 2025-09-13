# Volumetric Troubleshooting v2 — 2025-09-12

Symptoms observed
- Volume invisible unless particles drift to volume bounds; when visible, volume appears squarish (aligned to volume AABB), heavily distorted, and sometimes bleeds into negative space.
- Validation cascade on enable/resize (see appended VUIDs in your log).

Likely combined causes
1) Min/Max hierarchy not populated correctly
- Storage image reads/writes always target mip 0 because a single view is bound across all mips; `image3D` has no LOD argument.
- Source interface mismatch (`r16f` for density while the real density is `R32_SFLOAT`).
- Result: skip heuristic sees `max≈0` almost everywhere → aggressive skipping and black output.

2) World→texture mapping mismatch with changed grid params
- Volume bounds in shader: `boxMin=gridOrigin`, `boxMax=gridOrigin+voxelSize*gridDimensions`.
- If CPU side changed `VolumeParams` (e.g., grid expanded, voxel size altered) without updating push constants, rays will intersect one box but sample a different coordinate space → apparent “outside-in” sampling and negative-space drawing.

3) Layout/extent issues during TAA/minmax passes
- Transitions and copies issued inside dynamic rendering (forbidden without `dynamicRenderingLocalRead`) cause undefined sampling order.
- TAA/minmax image extents not matching swapchain on resize → OOB writes/copies and tiled top-left artifacts.

What to check (fast)
- In `render()` and `renderToTAATarget()` dump the pushed `gridOrigin/voxelSize/gridDimensions` and compare to the density image creation parameters.
- Enable a debug mode in `volume.frag` to render:
  - AABB intersection mask (white inside [t0,t1], black outside)
  - texCoord (worldToTexture) as RGB to detect out-of-range/clamping
  - `maxDensity` from min/max texture at a few LODs
- Visualize STBN, LUT sampling, and continuous LOD to ensure inputs sane.

Concrete fixes
A) Min/Max hierarchy passes
- Create per‑mip storage views: `baseMipLevel=m, levelCount=1` for both source and target.
- Shader interfaces (avoid format aliasing):
```
layout(binding=0, r32f)  readonly  image3D densityImage;   // only used when sourceMipLevel==0
layout(binding=1, rg16f) readonly  image3D minMaxSrc;      // used when sourceMipLevel>0
layout(binding=2, rg16f) writeonly image3D minMaxDst;      // target (m+1)
```
- Dispatch loop (CPU): for m in [0..mipCount-2]
  - source = (m==0) density view @ mip 0; else minmax view @ mip m
  - target = minmax view @ mip m+1
  - bind those two views via push descriptors, set `imageLayout=GENERAL`
  - barrier for previous write: COMPUTE→COMPUTE, SHADER_WRITE→SHADER_READ (on the specific mip)
- After building all mips: transition full minmax image → `SHADER_READ_ONLY_OPTIMAL` for graphics.

B) Volume sampling and skip
- In `skipEmptySpaceAlongRay()`, clamp computed `lod` to `[0, log2(max(gridDimensions))-1]` based on the ACTUAL min/max mip count.
- Validate `maxDensity` > 0 near known dense areas; if not, the hierarchy is empty.
- Temporarily disable skip and confirm that the volume renders correctly using density alone.

C) World→texture and bounds
- Ensure CPU push constants match the density/minmax images:
  - `gridOrigin` equals the world-space origin used in splatting
  - `voxelSize` equals density image world spacing
  - `gridDimensions` equals the 3D image extent
- If you changed volume params for TAA path (`renderToTAATarget`), propagate the same push constants.

D) Barriers & dynamic rendering
- Move all `vkCmdPipelineBarrier` layout transitions and all image copies outside any `vkCmdBeginRendering`/`vkCmdEndRendering` region (or enable `VK_KHR_dynamic_rendering_local_read`).
- Ensure descriptor `imageLayout` matches the real layout at use time (GENERAL for compute images, READ_ONLY for sampled images).

E) Resize correctness
- On swapchain resize, recreate TAA and min/max images to new extent; clear history; set `m_taaFirstFrame=true` for one frame.
- Pass the TAA/minmax image extent (not swapchain extent) into shaders and copy regions.

Sanity tests to add
- Debug LOD heatmap and `maxDensity` grayscale. If the hierarchy is working, max should fade smoothly across mips.
- Render `worldToTexture` X/Y/Z as RGB to verify mapping and catch flipped axes or wrong extents.
- Draw the computed `boxMin/boxMax` AABB edges in screen space to confirm intersection setup.

MCP spec anchors to reference
- search_vulkan_spec("vkCmdPipelineBarrier2") — Synchronization2, place transitions out of dynamic rendering
- search_vulkan_spec("Images") — Subresource ranges and views for per‑mip storage access

If after these checks the volume is still invisible with skip enabled, restore rendering by forcing:
- `skipDistance = 0`, `lod = 0` in skip path, then enable stepwise: first correct min/max population, then reintroduce skip, then continuous LOD.

These steps isolate whether the distortion comes from the hierarchy (empty or wrong mips), world→texture mapping, or pipeline state/layout issues during the passes.

## New clue: duplicated smaller ring inside the larger one
This is a strong indicator of a TAA reprojection scale mismatch (history being sampled at a different scale/extent than the current frame) or double application/mismatch of jitter.

How to confirm quickly
- Toggle TAA off → if the duplicate inner ring disappears, the issue is entirely in the TAA pass.
- In the TAA shader, render `historyUV` as color (e.g., `fragColor=vec4(historyUV,0,1)`) to see if it maps 0..1 across the screen; a shrunken mapping reveals a scale error.

Typical causes and fixes
1) Using a 4×4 `currentToHistory` reprojection without depth
   - With z=0 and no per-pixel depth, the matrix can introduce unintended scale/rotation. Use jitter‑only reprojection instead:
   ```
   vec2 texel = 1.0 / taa.screenSize;
   vec2 historyUV = fragCoord + (prevJitter - currJitter) * texel; // clamp
   ```
   - Pass `prevJitter` and `currJitter` as push constants; they must be the SAME jitter used in the volume pass.

2) Extent mismatch (half‑res TAA or stale sizes)
   - If TAA runs at a different resolution than the swapchain, set `taa.screenSize` to the TAA image extent and compute texel size from that. Do not use swapchain size unless TAA is 1:1.
   - On resize, recreate TAA images and pass the new extent to shaders and copy regions.

3) Double/incorrect jitter application
   - Ensure you apply CP jitter once in the volume pass (ray start), and in TAA you only subtract the delta `(prev−curr)`; do NOT add CP jitter again in TAA independently.
   - If projection‑based jitter is used, both `prevViewProj` and `currViewProj` must include their respective jitter; update `prev` only after TAA uses it.

4) Address mode and wrapping
   - Set history sampler to `CLAMP_TO_EDGE` to avoid wrapped repetitions that can look like nested rings.

5) Blend sanity
   - For debugging, set TAA blend `alpha=0.0` (pure history) and `alpha=1.0` (pure current). If only pure history shows a smaller ring, the history UV mapping is scaled.

Apply these together with the hierarchy fixes above; once TAA produces a single-sized ring that aligns with particles, re‑enable min/max skipping and continuous LOD.
