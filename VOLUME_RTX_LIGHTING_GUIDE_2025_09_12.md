# Volumetric RTX Lighting Guide — 2025-09-12

Goal: High-quality volumetric lighting in space (no ground plane), leveraging RTX where it helps and staying performant. You already have BLAS/TLAS; this guide plugs RT into the volume with minimal churn.

Contents
- A. Deep Shadow Map (opacity shadow map) for directional lights
- B. Epipolar / light‑aligned sampling (shaft quality at low cost)
- C. Ray‑query occluders with your TLAS (no ground needed)
- D. Procedural RT volume (advanced, optional)
- E. Recommended path (ordered tasks), files to touch, and validation
- F. MCP spec queries

---

## A) Deep Shadow Map (DSM) — Opacity shadow map in light space
Best first step for “space” scenes. Precompute transmittance along the light direction; during raymarch, fetch visibility with a single lookup.

Light model
- Start with a directional light (e.g., cinematic 3‑point or single key).
- Light view: `L_view = lookAt(lightPos, target, up)`, `L_proj = ortho(lbounds)` or a tight fit around the volume AABB.

Resources
- `image2D dsm` (R16F or R32F), size e.g., 1024×1024; usage: `STORAGE | SAMPLED | TRANSFER_DST`.
- Optional cascades (two maps) if your volume spans a very long axis.

Build pass (compute)
- For each pixel (x,y) in DSM:
  - Cast a ray along `−lightDir` through the volume bounds; step in world units `s_light`.
  - Accumulate optical depth τ = Σ (σ_t · density(world) · s_light).
  - Store transmittance T = exp(−τ) in `dsm[x,y]`.
- Acceleration
  - Use your min/max hierarchy: skip blocks whose `max<threshold` in light space; advance by block size.
  - Use a light‑space MIP (optional) for further skipping.
- Barriers
  - Before compute: DSM → `GENERAL`; after compute: DSM → `SHADER_READ_ONLY_OPTIMAL`.

Sampling in volume.frag
- For a shaded sample at world position `pos`:
  - Project into light clip: `L_ndc = L_proj * L_view * vec4(pos,1)` → `L_uv = L_ndc.xy / L_ndc.w * 0.5 + 0.5`.
  - Fetch `T = texture(dsm, L_uv).r` (use CLAMP + bilinear; optional 3×3 PCF if aliasing).
  - Multiply your single‑scattering/emission by `T`.

Notes
- DSM integrates only participating media; no opaque occluders. That’s handled in (C).
- For multiple lights, build DSM per light or update every N frames and interleave.

---

## B) Epipolar / light‑aligned sampling
Increases shaft fidelity without huge step counts.

Concept
- Reparameterize sampling along epipolar lines (aligned to the light direction). Integrals are shared coherently across pixels.

Minimal integration (hybrid)
- Keep view‑ray march, but when computing single scattering for a directional light, sample nearby points along the light ray with larger spacing and interpolate.
- Or precompute a low‑res 3D grid of integrated in‑scattering along the light and sample it in the volume pass.

Practical recipe
- For each step, compute `posLightRay = pos + k * lightDir` for k in {0, 1, 2} at a coarse stride; use the min/max hierarchy to stop early.
- Weight and sum contributions with HG phase; multiply by DSM visibility.

---

## C) Ray‑Query occluders (with your TLAS)
Add “solid” occluders (black‑hole sphere, accretion disk slab/torus, crafts debris). No ground plane required.

Pipeline
- Your TLAS already exists. Add BLAS for:
  - Black‑hole: sphere mesh (or SDF proxy converted to triangles), instance at gravity center.
  - Accretion disk: thin cylinder/torus mesh (low‑poly) scaled to current radius.
- Bind TLAS as `accelerationStructureEXT` to the volume pass.

Shader (GL_EXT_ray_query)
```glsl
#extension GL_EXT_ray_query : enable
layout(binding = X) uniform accelerationStructureEXT tlas;
...
vec3 L = normalize(lightDirOrPoint - pos);
rayQueryEXT rq;
rayQueryInitializeEXT(rq, tlas, gl_RayFlagsTerminateOnFirstHitEXT, 0xFF,
                      pos + L*0.002, 0.0, L, maxShadowDist);
while (rayQueryProceedEXT(rq)) {}
bool occluded = (rayQueryGetIntersectionTypeEXT(rq,false) != gl_RayQueryCommittedIntersectionNoneEXT);
float visibility = occluded ? 0.0 : 1.0;
```
Tips
- Only trace when `dens > smallThreshold` and every N steps (e.g., N=2–3).
- Use a reduced `maxShadowDist` tied to volume thickness.
- Combine with DSM: final `visibility = visibility * T_dsm`.

---

## D) Procedural RT volume (advanced)
Represents the entire volume as a TLAS AABB and evaluates transmittance in an intersection/any‑hit shader.

Sketch
- TLAS: one AABB instance for the volume bounds.
- Intersection shader: short fixed‑iteration march inside the AABB to integrate τ; call `ignoreIntersection` or `terminate` based on density.
- Any‑hit: accumulate attenuation (requires custom payload). This is powerful but requires a full RT pipeline (`VK_KHR_ray_tracing_pipeline`) and a denoiser.

Recommendation: do this later after DSM+ray‑query are working.

---

## E) Recommended path (ordered tasks)
1) DSM (opacity shadow map)
   - Create DSM image, build compute pass (before volume render each frame or every N frames), sample in `volume.frag`.
   - Use min/max hierarchy to skip in the DSM builder.
2) Epipolar light‑aligned sampling
   - Add coarse light‑ray sampling in the volume shader for the directional light; blend with DSM visibility.
3) Ray‑query occluders
   - Add simple analytic BLAS (BH, disk); integrate visibility in the volume shader using your TLAS.
4) Optional: DLSS SR (Streamline) for free perf, then NRD/DLSS‑RR once RT signals are stable.
5) Optional: Procedural RT volume.

Files to touch
- `src/renderer/VulkanContext.cpp` — enable RT features: `accelerationStructure`, `rayQuery`, `bufferDeviceAddress`.
- `src/systems/VolumeRenderer.*` — create DSM image + compute pipeline; bind TLAS descriptor; drive DSM build before volume; pass light matrices, DSM handle, and TLAS to `volume.frag`.
- `shaders/volume.frag` — add DSM sampling and (later) ray‑query visibility; epipolar samples; HG polish.
- New `shaders/dsm_build.comp` — compute DSM in light space.

Validation checklist
- DSM barriers: GENERAL during compute, SHADER_READ for sampling.
- DSM projection covers volume AABB; verify with a debug view of `L_uv` and `T_dsm`.
- Ray query returns occlusion against your BLAS; debug mode rendering `visibility`.
- Epipolar sampling improves shafts at similar perf.

Perf notes
- DSM at 1024² per frame is typically cheap; update every 2–4 frames if needed.
- Ray queries: gate by density and cadence; keep `maxShadowDist` tight.
- Use current min/max to skip both view‑ray and DSM/light‑ray work.

---

## F) MCP Spec Queries
- search_vulkan_spec("VK_KHR_ray_query") — feature struct & usage
- search_vulkan_spec("VK_KHR_acceleration_structure") — build commands & geometry
- search_vulkan_spec("vkCmdPipelineBarrier2") — Synchronization2 for DSM transitions
- search_vulkan_spec("descriptor acceleration structure KHR") — descriptors for TLAS

This roadmap yields dramatic volumetric depth with no ground plane: DSM supplies media self‑shadowing, epipolar sampling enhances shafts, and ray queries add occlusion from BH/disk geometry. Move to procedural RT volume once these are solid.
