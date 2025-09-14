# Real‑Time RTX Shadows & Lighting for Orbiting Volumetric Plasma (2025-09-13)

This guide gives you a pragmatic, staged path to visible RTX results and then builds up quality while keeping performance. It uses `VK_KHR_ray_query` in your existing graphics pipeline, plus a minimal set of proxy occluders for high visual impact. DSM is shelved; you can reintroduce it later as a hybrid.

Project fit
- Rendering: volumetric plasma via ray marching over a 3D density grid (`volume.frag`).
- Scene: orbiting particle cloud, no ground plane by default.
- Goal: fast, stable shadows from simple occluders (BH sphere + accretion disc ring) + scalable softening, with clear debug and performance gates.

---

## Why Ray Query first
- Integrates into `volume.frag` without a separate RT pipeline.
- Gives immediate “hard contact” shadows from simple occluders.
- Keeps descriptor model small (1 TLAS binding).

MCP
- search_vulkan_spec("VK_KHR_ray_query")
- search_vulkan_spec("VK_KHR_acceleration_structure")

---

## Stage 0 — Device, features, and tooling sanity

Enable extensions (device)
- `VK_KHR_acceleration_structure`
- `VK_KHR_ray_query`
- `VK_KHR_deferred_host_operations` (dependency of AS)

Enable features
- `VkPhysicalDeviceRayQueryFeaturesKHR::rayQuery = VK_TRUE`
- `VkPhysicalDeviceAccelerationStructureFeaturesKHR::accelerationStructure = VK_TRUE`
- `VkPhysicalDeviceBufferDeviceAddressFeatures::bufferDeviceAddress = VK_TRUE`

Load functions (volk)
- `vkCreateAccelerationStructureKHR`, `vkDestroyAccelerationStructureKHR`
- `vkGetAccelerationStructureBuildSizesKHR`, `vkCmdBuildAccelerationStructuresKHR`
- `vkGetAccelerationStructureDeviceAddressKHR`

Sanity asserts
- After `volkLoadDevice`: assert `vkCmdPushDescriptorSetKHR != nullptr` and use it exclusively.

MCP
- search_vulkan_spec("VK_KHR_deferred_host_operations")
- search_vulkan_spec("buffer device address")

---

## Stage 1 — First light: one occluder, one light, hard shadows

Pick a simple, visually obvious occluder: a horizontal “shadow catcher” disc that floats below the volume (or a large ground plane under the orbit path). This proves end‑to‑end RT plumbing and yields immediately visible shadows.

1) Build BLAS for the disc/plane
- Create a small vertex buffer (2 triangles for a plane, or a triangle fan for a disc).
- Buffer usage: `ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | SHADER_DEVICE_ADDRESS_BIT`.
- Get device address; describe geometry with `VkAccelerationStructureGeometryKHR` (triangles).
- Get build sizes, allocate BLAS buffer with `ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | SHADER_DEVICE_ADDRESS_BIT`.
- Build via `vkCmdBuildAccelerationStructuresKHR`.

2) TLAS (one instance)
- Fill `VkAccelerationStructureInstanceKHR` with identity transform and reference BLAS address.
- Upload instance buffer with `ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | SHADER_DEVICE_ADDRESS_BIT`.
- Create TLAS, build with scratch, then synchronize with Synchronization2 (see below).

3) Synchronization2 barrier (same frame use)
```cpp
VkMemoryBarrier2 asBarrier{ VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
asBarrier.srcStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
asBarrier.srcAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
asBarrier.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
asBarrier.dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;
VkDependencyInfo dep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO, nullptr, 0,nullptr, 1,&asBarrier, 0,nullptr };
vkCmdPipelineBarrier2(cmd, &dep);
```

4) Descriptor wiring (binding 3)
- Add TLAS to the volume descriptor set layout when RT is supported:
  - Binding 3: `VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR`
- Push descriptors every frame (use only `vkCmdPushDescriptorSetKHR`).

5) GLSL in `volume.frag` (boolean occlusion)
```glsl
#extension GL_EXT_ray_query : require
layout(binding = 3) uniform accelerationStructureEXT topLevelAS;

bool hasOccluderRT(vec3 originWS, vec3 dirWS, float tMax)
{
    rayQueryEXT rq;
    uint flags = gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsOpaqueEXT;
    rayQueryInitializeEXT(rq, topLevelAS, flags, 0xFF, originWS, 0.001, normalize(dirWS), tMax);
    while (rayQueryProceedEXT(rq)) {}
    return rayQueryGetIntersectionTypeEXT(rq, true) != gl_RayQueryIntersectionNoneEXT;
}
```
Integrate in your scattering block:
```glsl
float shadowVisibility = 1.0;
if (doExpensiveShading) {
    vec3 lightDir = normalize(vec3(-0.5, -0.8, -0.6));
    bool blocked = hasOccluderRT(pos, lightDir, 200.0);
    shadowVisibility = blocked ? 0.0 : 1.0;
}
// multiply into emission/scattering
```

Gating for performance
- Only call RT when `dens > threshold` and every Nth step (you already have stride gating).

Debug
- Add a hotkey to output `fragColor = blocked ? vec4(0) : vec4(1)` at one sample per pixel to confirm.

MCP
- search_vulkan_spec("vkCmdBuildAccelerationStructuresKHR")
- search_vulkan_spec("vkCmdPipelineBarrier2")
- search_vulkan_spec("VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR")

---

## Stage 2 — Project occluders: BH sphere + accretion disc ring

With first light working, replace the plane with meaningful proxies:

- Black Hole proxy: scaled sphere (low‑poly icosphere). Place at the gravity center; optionally animate spin only as transform (TLAS update/refit).
- Accretion Disc proxy: flat ring (two radii: inner/outer). Low triangle count. Its transform follows the particle disc plane.

Instance transforms
- Prefer TLAS updates (refit) over rebuild if your drivers perform well; otherwise just rebuild small TLAS each frame (still cheap with few tris).

Masks and flags
- Use instance `mask=0xFF` and query `rayQueryInitializeEXT(..., cullMask=0xFF, ...)`.
- Use `gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsOpaqueEXT` for fast boolean tests.

Softening (light penumbra)
- Multi-sample: cast 2–4 jittered light rays per shaded sample (rotated within a cone) and average. Gate heavily.
- Temporal accumulation: 1 ray per frame with blue-noise jitter, accumulate via your TAA.

---

## Stage 3 — Volumetric self‑shadow approximation (non‑DSM)

DSM is shelved; you can still add convincing self‑shadow with a low‑cost light‑aligned sample:

Option A (cheapest): single “look‑ahead” density tap
- For each shaded step, sample density a fixed distance upwind along `lightDir` (e.g., `pos + lightDir * k * voxelSize`).
- Convert density to an attenuation factor like `shadowSelf = exp(-sigma_t * dens * k * voxelSize)`.
- Combine with RT occluder term: `visibility = shadowSelf * visibilityRT`.

Option B (better): 2–3 taps along `lightDir`
- Accumulate `tau += sigma_t * dens * ds` for 2–3 equally spaced taps; compute `exp(-tau)`.
- Still far cheaper than full DSM or full light‑space integration.

These produce “shafting” and depth variation and are robust with your TAA.

---

## Stage 4 — Performance controls and expected budgets

Controls
- Density threshold: skip RT when `dens < t_min`.
- Stride: only every Nth step does RT; reuse visibility for skipped steps.
- Max distance: clamp tMax to a scene‑bounded value (e.g., disc radius * factor).
- Proxy LOD: keep BH/disc extremely low poly.

Budgets (typical on mid/high GPUs)
- 1 boolean ray query per ~8–16 shaded steps at 1080p is often OK.
- 2–3 multi‑sampled rays per “hero shot” step with temporal accumulation can be realtime if gated.

---

## Stage 5 — Quality upgrades after stable

- Temporal soft shadows: jitter lightDir slightly per frame (blue noise), accumulate in TAA.
- Contact refinement: near occluder surfaces, temporarily increase RT frequency.
- Hybrid re‑introduction of DSM: use DSM for volumetric self‑shadow and RT only for geometry occluders. Keep both cheap by sharing the same light matrices and restricting DSM size.
- Full RT pipeline (optional): migrate to `VK_KHR_ray_tracing_pipeline` for area lights and SBT; heavier lift.

MCP
- search_vulkan_spec("VK_KHR_ray_tracing_pipeline")

---

## Integration points in your codebase

- `src/renderer/VulkanContext.cpp`
  - Ensure device extensions/features chain includes AS + Ray Query + Deferred Host Ops; confirm via console prints (already present).

- `src/systems/VolumeRenderer.h/.cpp`
  - Add/create BLAS/TLAS buffers; small helpers to build/tear down.
  - Expose `buildAccelerationStructures(VkCommandBuffer)`; call once at frame start before dynamic rendering.
  - Descriptor layout: add binding 3 for TLAS when RT supported; push with `vkCmdPushDescriptorSetKHR` only.

- `shaders/volume.frag`
  - Add `#extension GL_EXT_ray_query : require`, bind TLAS at binding 3, integrate boolean occlusion in the emission/scattering block with density/stride gating.

- `src/core/Application.cpp`
  - At command buffer record start (before `vkCmdBeginRendering`): call `m_volumeRenderer->buildAccelerationStructures(cmd)` and then the Synchronization2 barrier.

---

## Debug & validation

- One‑frame asserts/logs:
  - TLAS handle non‑null before descriptor push; print descriptor write `(binding=3, type=AS_KHR)`.
  - `vkCmdPushDescriptorSetKHR` pointer non‑null (from volk).
- Shader triage: switch off RT block to confirm crash is tied to RT path.
- RenderDoc: inspect descriptors for the volume pipeline; TLAS binding should be present.

---

## MCP queries to keep handy
- search_vulkan_spec("VK_KHR_acceleration_structure")
- search_vulkan_spec("vkGetAccelerationStructureBuildSizesKHR")
- search_vulkan_spec("vkCmdBuildAccelerationStructuresKHR")
- search_vulkan_spec("vkGetAccelerationStructureDeviceAddressKHR")
- search_vulkan_spec("VkAccelerationStructureInstanceKHR")
- search_vulkan_spec("VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR")
- search_vulkan_spec("vkCmdPipelineBarrier2")
- search_vulkan_spec("GL_EXT_ray_query")

---

## Short bring‑up checklist
- [ ] Device: AS + Ray Query + Deferred Host Ops enabled.
- [ ] BLAS/TLAS for a simple plane/disc built and synced before draw.
- [ ] Descriptor binding 3 (TLAS) added and pushed via `vkCmdPushDescriptorSetKHR`.
- [ ] `volume.frag` uses `GL_EXT_ray_query`; occlusion multiplies scattering.
- [ ] Density/stride gating in place; tMax clamped.
- [ ] Visible hard shadows under the volume → then replace plane with BH/disc proxies.
- [ ] Add softening (multi‑sample or temporal) once stable.
