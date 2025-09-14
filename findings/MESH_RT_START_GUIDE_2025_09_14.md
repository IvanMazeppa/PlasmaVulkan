# Mesh Renderer RT Start — Quick Visible Shadows (2025-09-14)

Goal
- Add RT shadows to the mesh shader path first for fast, stable results. Keep particles rasterized; restrict RT to large occluders (BH sphere, accretion disc/ring).

## Quickstart (for Claude)
- Branch: ensure you are on the RT foundation baseline (mesh path works; volumetric experiments are parked).
- Build and run once to confirm: mesh renderer is rendering; no critical VUIDs during frame; some exit-time VUIDs are expected before Job M-0002.
- Ensure device extensions are enabled in `VulkanContext.cpp`:
  - `VK_KHR_ACCELERATION_STRUCTURE`, `VK_KHR_RAY_QUERY`, `VK_KHR_DEFERRED_HOST_OPERATIONS`
- Ensure features are enabled via pNext chain:
  - `VkPhysicalDeviceAccelerationStructureFeaturesKHR::accelerationStructure = VK_TRUE`
  - `VkPhysicalDeviceRayQueryFeaturesKHR::rayQuery = VK_TRUE`
  - `VkPhysicalDeviceBufferDeviceAddressFeatures::bufferDeviceAddress = VK_TRUE`
- Instrument logs (Job M-0001 will add explicit prints) and tee output to `logs/run.txt`.

MCP queries to have ready
- search_vulkan_spec("VK_KHR_acceleration_structure")
- search_vulkan_spec("VK_KHR_ray_query")
- search_vulkan_spec("vkGetAccelerationStructureBuildSizesKHR")
- search_vulkan_spec("vkCmdBuildAccelerationStructuresKHR")
- search_vulkan_spec("vkGetAccelerationStructureDeviceAddressKHR")
- search_vulkan_spec("VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR")
- search_vulkan_spec("vkCmdPipelineBarrier2")
- search_vulkan_spec("vkDestroyAccelerationStructureKHR")

---

## M-0001 — Ray Query feature/extension reconciliation + visibility logs

Why
- You see AS init success but no RayQuery support messages. We need authoritative logs and to guarantee features are truly enabled.

Deliverables
- In `VulkanContext.cpp` after querying device feature support and before creating the device:
  - Explicitly chain `VkPhysicalDeviceRayQueryFeaturesKHR` into the pNext chain passed to `vkCreateDevice`. Confirm `rayQuery` is set to `VK_TRUE` only if supported.
- After device creation, print one-time logs:
  - `Ray Query extension enabled: <YES/NO>`
  - `Ray Query feature (rayQuery) enabled: <YES/NO>`
  - `Acceleration Structure extension enabled: <YES/NO>`
- Store `m_supportsRayQuery` and `m_supportsAccelerationStructure` flags and log them once on first frame.

Acceptance
- `logs/run.txt` contains the three lines above (once per run).
- If Ray Query is unsupported/disabled, log a clear fallback message and keep the build running (we’ll still use TLAS for future compatibility).

Notes
- Keep pNext chain order consistent and terminate correctly. Ensure no feature structs are lost due to overwritten `pNext` pointers.

---

## M-0002 — Occluder BLAS/TLAS + teardown hygiene (fix exit-time VUIDs)

Why
- You have exit-time VUIDs. We’ll add occluder AS plus correct destruction order to eliminate shutdown validation noise.

Deliverables
- Implement utilities to build once and reuse:
  - BLAS meshes: unit sphere (icosphere/UV sphere), unit disc/annulus (ring). Store vertex/index buffers with `SHADER_DEVICE_ADDRESS_BIT` and build flags.
  - TLAS with 1–3 instances (BH sphere at origin, tilted disc). Log TLAS device address.
- Synchronization: after each BLAS/TLAS build, issue `vkCmdPipelineBarrier2` from `ACCELERATION_STRUCTURE_BUILD_BIT_KHR` to `FRAGMENT_SHADER_BIT` (and `COMPUTE_SHADER_BIT` if needed).
- Teardown hygiene:
  - On shutdown, call `vkDeviceWaitIdle(device)` before destroying pipelines/descriptors/AS/buffers.
  - Destroy in safe order: stop draws → destroy pipelines → free descriptor sets/layouts → destroy TLAS → destroy BLAS → free buffers/memory.
  - Null handles after destroy; optional logs for each destruction step (guarded).

Acceptance
- `logs/run.txt` prints non-null TLAS device address after build.
- No validation errors during build/use.
- Exit-time VUIDs related to AS/descriptor/pipeline destruction are gone on 3 consecutive runs.

Notes
- Use VMA for buffers; keep scratch buffers transient.
- Prefer build-once; we can refit later when transforms animate.

---

## M-0003 — Mesh descriptor binding 3 = AS_KHR, push TLAS, minimal ray query

Why
- We need a visible effect in the mesh path with minimal risk and cost.

Deliverables
- Descriptor layout: in the mesh material/scene set, add binding 3 as `VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR` when RT is enabled. Ensure both SDR/HDR mesh pipelines share the same layout.
- Push TLAS: use `VkWriteDescriptorSetAccelerationStructureKHR` and `vkCmdPushDescriptorSetKHR` to bind TLAS at draw time. Log once per frame (guarded): “Pushed TLAS at binding 3 (AS_KHR)”.
- Shader change (mesh fragment):
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
- Use a fixed scene light for now; multiply a visibility factor into emission/lighting:
```glsl
float shadowVis = 1.0;
vec3 L = normalize(vec3(-0.5, -0.8, -0.6));
if (/* enable_rt */ hasOccluderRT(worldPos, L, 1000.0)) shadowVis = 0.0;
finalColor.rgb *= shadowVis;
```
- Add a runtime toggle (push constant/UBO) to disable RT instantly for triage.

Acceptance
- Toggling the RT flag changes the shading (darker where occluded by sphere/disc).
- No VUID 07990 (layout mismatch) or push-descriptor errors.
- Logs show TLAS pushed (once per frame, throttled).

Notes
- Ensure `worldPos` and `L` are in the same (world) space.
- Keep cost minimal: one ray query per-fragment; add softening/temporal later (M-0004).

---

## M-0004 — Soft shadows + temporal stabilization (baseline)

Deliverables
- Blue-noise jitter for ray direction (small cone ~1–2°)
- Temporal accumulation of shadow factor with clamp
- Optional: sample two directions per frame and interleave

Acceptance
- Penumbrae appear; minimal shimmer
- Stable over motion; no major ghosting

Validation and logging
- Assert no barriers/copies within dynamic rendering blocks
- Print support flags once per run (M-0001)
- Print TLAS device address once after build (M-0002)
- Print TLAS push notice once per frame during bring-up (M-0003, throttled)
