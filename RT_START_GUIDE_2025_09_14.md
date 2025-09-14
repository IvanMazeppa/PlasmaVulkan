# Real‑Time RT Lighting/Shadows — Staged Start (2025-09-14)

This document gives Claude the exact information to implement the first RT jobs safely on the reset baseline. We gate features, add visibility logging, and validate each step with acceptance tests.

Readiness checklist (confirm, don’t change unless false)
- Device extensions enabled: `VK_KHR_acceleration_structure`, `VK_KHR_ray_query`, `VK_KHR_deferred_host_operations`
- Device features enabled: `VkPhysicalDeviceAccelerationStructureFeaturesKHR::accelerationStructure = VK_TRUE`, `VkPhysicalDeviceRayQueryFeaturesKHR::rayQuery = VK_TRUE`, `VkPhysicalDeviceBufferDeviceAddressFeatures::bufferDeviceAddress = VK_TRUE`
- Synchronization2 in use (`vkQueueSubmit2`, `vkCmdPipelineBarrier2`)
- Separate pipelines per render target format: SDR (swapchain), HDR (TAA target)
- No barriers/copies inside dynamic rendering; all transitions occur between `vkCmdEndRendering`/`vkCmdBeginRendering`

MCP queries to keep handy
- search_vulkan_spec("VK_KHR_acceleration_structure")
- search_vulkan_spec("VK_KHR_ray_query")
- search_vulkan_spec("vkGetAccelerationStructureBuildSizesKHR")
- search_vulkan_spec("vkCmdBuildAccelerationStructuresKHR")
- search_vulkan_spec("vkGetAccelerationStructureDeviceAddressKHR")
- search_vulkan_spec("VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR")
- search_vulkan_spec("vkCmdPipelineBarrier2")
- search_vulkan_spec("GL_EXT_ray_query")

Build system notes (shader)
- Ensure `glslc` (or your shader compiler) emits `SPV_KHR_ray_query` for RT shaders; target env ≥ Vulkan 1.2. A tiny compile probe is included in Job 0001.

---

## Job 0001 — RT readiness and shader toolchain probe

Goal
- Prove device feature/extension readiness and shader toolchain support for ray query without touching the main pipeline.

Edits
- Add a tiny fragment shader `shaders/test_ray_query.frag` (already present) with `#extension GL_EXT_ray_query : require` and minimal syntax; compile to SPIR‑V as a CI sanity check.
- At startup (one-time), log: Ray Query support (YES/NO), Acceleration Structure support (YES/NO).

Acceptance
- `test_ray_query.frag` compiles to `test_ray_query.spv` and contains `OpRayQuery*` ops.
- Console shows “Ray Query support: YES, Acceleration Structure support: YES”.

---

## Job 0002 — Minimal BLAS/TLAS (single proxy)

Goal
- Build one BLAS (plane/disc) and a TLAS with a single instance; make sure TLAS handle is non‑null and synchronized before use.

Edits
- Create a small static vertex buffer for a plane (2 triangles) or a ring (disc proxy). Usage: `ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | SHADER_DEVICE_ADDRESS_BIT`.
- Build BLAS: query build sizes, create AS buffer + scratch, record `vkCmdBuildAccelerationStructuresKHR`.
- Build TLAS: upload one instance referencing BLAS device address, record build + barrier.
- Add a one-time log after TLAS build: `m_topLevelAS != VK_NULL_HANDLE` and print its device address.
- Use `vkCmdPipelineBarrier2` to sync AS build → fragment read.

Acceptance
- No validation errors during AS build.
- TLAS handle non‑null and a device address is printed.

---

## Job 0003 — Volume descriptor layout + TLAS push (no shader changes yet)

Goal
- Unify the volume descriptor set layout used by both SDR/HDR pipelines. When RT is enabled, binding 3 is `ACCELERATION_STRUCTURE_KHR`. Push TLAS via KHR write API and KHR push descriptors. Do not modify the fragment shader yet.

Edits
- Create a helper to build the volume descriptor set layout; return the same layout for both pipelines.
- When RT is enabled, define binding 3 as `VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR`. Remove any legacy DSM binding at 3 in the RT build.
- At bind time, only write/push binding 3 if TLAS handle is non‑null. Use `VkWriteDescriptorSetAccelerationStructureKHR` with `pNext` and `vkCmdPushDescriptorSetKHR`.
- Add a one‑time log: “Pushed TLAS at binding 3 (AS_KHR), handle non‑null”.

Acceptance
- No VUID 07990 at pipeline creation for SDR/HDR volume pipelines.
- Frame runs with no push-descriptor errors.

---

## Job 0004 — Minimal ray query occlusion in volume.frag (gated)

Goal
- Add boolean shadow visibility using Ray Query with strict gating to keep cost low.

Edits
- Add to `shaders/volume.frag` (RT variant):
```glsl
#extension GL_EXT_ray_query : require
layout(binding = 3) uniform accelerationStructureEXT topLevelAS;

bool hasOccluderRT(vec3 originWS, vec3 dirWS, float tMax) {
    rayQueryEXT rq;
    const uint flags = gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsOpaqueEXT;
    rayQueryInitializeEXT(rq, topLevelAS, flags, 0xFF, originWS, 0.001, normalize(dirWS), tMax);
    while (rayQueryProceedEXT(rq)) {}
    return rayQueryGetIntersectionTypeEXT(rq, true) != gl_RayQueryIntersectionNoneEXT;
}
```
- Inside your expensive shading block (where DSM used to be), add a boolean visibility multiply, gated by density and stride:
```glsl
float shadowVis = 1.0;
if (dens > 0.01 && (shadeStride % 4u == 0u)) {
    vec3 lightDir = normalize(vec3(-0.5, -0.8, -0.6));
    shadowVis = hasOccluderRT(pos, lightDir, 200.0) ? 0.0 : 1.0;
}
// multiply into emission/scattering
emission *= shadowVis;
```
- Keep a runtime toggle (push constant or global) to disable RT quickly for triage.

Acceptance
- No RT validation errors; visibly darker regions under the proxy.
- Toggle off returns to original look.

---

## Instrumentation & safety
- Log once per frame (guarded) during bring‑up: whether binding 3 was pushed and TLAS handle value.
- Assert no barriers/copies occur while a dynamic rendering instance is active.
- Keep shader RT code under a runtime toggle to bisect stability quickly.

---

## After these jobs
- We’ll add softening (multi‑sample or temporal), and more proxies (BH sphere + disc ring) with TLAS update/refit.
- Then consider a cheap volumetric self‑shadow approximation (2–3 light‑aligned taps) before reintroducing DSM or more advanced methods.
