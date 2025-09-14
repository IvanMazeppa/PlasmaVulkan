# DSM Debugging and RTX Lighting Starter — 2025-09-12

You built a 1024×1024 DSM and sample it in the volume march, but the DSM visualization stays mid‑red (little variation) even when density is high. Below is a stepwise debug plan and fixes, followed by an RTX lighting starter if you want to augment DSM.

## Part 1 — DSM: Why no shadows yet

### A. Make the two spaces identical (CPU‑driven)
- Compute once per frame on CPU:
  - `lightView`, `lightProj` (tight ortho around volume AABB in light space).
  - Push these matrices to BOTH the DSM compute and volume pass. Do not reconstruct matrices in shaders.
- Push the same `gridOrigin`, `voxelSize`, `gridDimensions` used to create the 3D density image.

MCP to keep handy
- search_vulkan_spec("Images") — image views, subresources, layouts
- search_vulkan_spec("vkCmdPipelineBarrier2") — Synchronization2 barriers

### B. Prove the DSM ray actually traverses the volume
Add these temporary changes in the DSM compute (dsm_build.comp):
1) AABB intersection
```
// Intersect ray with volume AABB in world space
vec3 bmin = pc.gridOrigin;
vec3 bmax = pc.gridOrigin + pc.voxelSize * pc.gridDimensions;
vec3 invD = 1.0 / rayDirection;
vec3 t0 = (bmin - rayOrigin) * invD;
vec3 t1 = (bmax - rayOrigin) * invD;
vec3 tsm = min(t0, t1);
vec3 tbg = max(t0, t1);
float tNear = max(0.0, max(max(tsm.x, tsm.y), tsm.z));
float tFar  = min(tbg.x, min(tbg.y, tbg.z));
if (tFar <= tNear) { imageStore(dsmImage, texelCoord, vec4(1.0,0,0,1)); return; }
float t = tNear;
float tEnd = min(tFar, pc.maxDistance);
```
2) Early exit and safety
```
for(; t < tEnd; t += pc.stepSize) {
  vec3 P = rayOrigin + rayDirection * t;
  float d = sampleDensity(P) * pc.densityScale;
  opticalDepth += d * pc.stepSize;
  if (opticalDepth > 20.0) { // clamp to avoid overflow
    break;
  }
}
```
3) Write τ instead of T (debug build only)
```
imageStore(dsmImage, texelCoord, vec4(opticalDepth * 0.05,0,0,1));
```
If you still see a flat field, the ray never enters the AABB (matrix mismatch) or `pc.stepSize/maxDistance` are wrong.

### C. Tune parameters coherently
- `pc.stepSize` ~ 0.5–1.5 × `voxelSize` (world units).
- `pc.maxDistance` ~ `length(volumeDiagonal)`.
- `densityScale` matches the scale used in the view‑ray march (your `opacityScale` mapping to σ_t).
- Expect τ in [0..8] across most of the map; `T = exp(−τ)`.

### D. Descriptor/layout and barriers
- DSM image: `R16_SFLOAT` or `R32_SFLOAT`; usage = `STORAGE | SAMPLED | TRANSFER_DST`.
- Build pass: DSM → `GENERAL` (write); After: DSM → `SHADER_READ_ONLY_OPTIMAL`.
- Sample pass: descriptor `imageLayout` must be `SHADER_READ_ONLY_OPTIMAL`.
- Resize: recreate DSM for the new extent; clear to 1.0.

### E. Volume sampling correctness
- In volume.frag `sampleDSM`, use the pushed `lightProj/lightView` directly; compute `lightUV` and clamp. Visualize `lightUV` as color to confirm it covers [0,1].
- Remove the floor `max(shadowVisibility, 0.3)` while debugging; test `shadowVisibility` raw.
- Quick diagnostic: sample DSM at the volume center only and draw it to screen.

### F. Sanity toggles to add
- DSM view mode: draw DSM as a fullscreen quad (one to one) to see if dense cores actually darken.
- “Tau mode”: DSM stores τ instead of T (linear look) to confirm integration.
- “Single‑light mode”: move light across the volume and verify DSM changes spatially.

Common pitfalls resolved by this list
- Light matrices differ between build/sample; DSM aligned incorrectly.
- Rays miss the AABB; T≈1 everywhere.
- Step/σ_t inconsistent; τ either ~0 or overflows.
- Layout/usage mismatch; DSM reads stale data.

---

## Part 2 — If you want a stronger effect quickly
Once DSM is aligned, two upgrades produce visible shafts:

1) Epipolar / light‑aligned sampling (in volume.frag)
- For directional lights, add 2–3 samples along +lightDir at a coarse stride per view step, multiply by DSM visibility.
- Gate by density and transmittance to keep cost down.

2) Ray‑query occluders (with your TLAS)
- Add simple BH/disk meshes to your TLAS; from each shaded step cast one `rayQuery` toward the light (`gl_RayFlagsTerminateOnFirstHitEXT`).
- Combine `visibility = visibilityRT * T_dsm`.

MCP searches
- search_vulkan_spec("VK_KHR_ray_query") — features & usage
- search_vulkan_spec("VK_KHR_acceleration_structure") — build inputs

---

## Part 3 — Minimal task list to land DSM
1) CPU: compute volume AABB; derive tight ortho `lightProj` and `lightView`; push to DSM+volume.
2) DSM compute: add AABB intersection; integrate τ with tuned `stepSize/densityScale`; clamp τ.
3) Barriers: DSM GENERAL→READ_ONLY; descriptors match.
4) Volume: remove visibility floor; visualize `lightUV`; confirm shadows.
5) Optional: epipolar samples; later add ray‑query occluders.

If shadows are still absent after these steps, capture a RenderDoc frame and check:
- DSM image contents (τ or T) contain structure; if not, fix the build path.
- `lightUV` varies across the volume and maps into DSM bounds.

