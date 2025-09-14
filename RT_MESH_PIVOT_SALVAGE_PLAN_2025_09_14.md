# RT Mesh Pivot — Salvage Plan (2025-09-14)

Objective
- Preserve and reuse the valuable parts of the recent RT/DSM/TAA work after the rollback, while pivoting RT to the mesh shader renderer for a faster MVP.

Reusable immediately (keep and integrate)
- Device feature/extension enabling in `VulkanContext.cpp`:
  - `VK_KHR_acceleration_structure`, `VK_KHR_ray_query`, `VK_KHR_deferred_host_operations`
  - `VkPhysicalDeviceAccelerationStructureFeaturesKHR::accelerationStructure = VK_TRUE`
  - `VkPhysicalDeviceRayQueryFeaturesKHR::rayQuery = VK_TRUE`
  - `bufferDeviceAddress` + `Synchronization2`
- AS build utilities (BLAS/TLAS) from `VolumeRenderer`:
  - `createAccelerationStructures`, `buildAccelerationStructures` patterns
  - Scratch/build sizing queries and barriers using `vkCmdPipelineBarrier2`
- KHR push descriptor write for AS (binding 3):
  - `VkWriteDescriptorSetAccelerationStructureKHR` usage
  - `vkCmdPushDescriptorSetKHR`
- Logging scaffolding: one-time readiness logs, per-frame guarded logs for TLAS push

Park for later (archive, don’t delete)
- DSM compute pass and visualization shaders
- Min/Max hierarchy code and docs
- Volumetric TAA path (history images, reprojection) — keep for post-pass stability later

Refactor/lightweight reuse (trim to minimal hooks)
- Volume renderer RT binding and shader code: keep the descriptor layout helper and gating; remove strict dependency for now
- Density grid transitions and mip generation: not required for mesh RT, but keep utilities intact

What we will build next (mesh path)
- A minimal occluder asset set: unit sphere + disc/annulus BLAS
- TLAS with a few instances (BH sphere, tilted disc)
- Mesh material descriptor set with binding 3 = `ACCELERATION_STRUCTURE_KHR`
- Minimal ray query shadow factor in mesh fragment shader, gated + toggle

Acceptance before proceeding to soft shadows
- Clean validation on AS build and descriptor push
- TLAS handle non-null and printed device address
- Visual shadow response when toggle is ON, no change when OFF

Notes
- Do not include particles in TLAS. RT remains for large occluders only.
- Maintain dynamic rendering rules: no barriers/copies inside render.
