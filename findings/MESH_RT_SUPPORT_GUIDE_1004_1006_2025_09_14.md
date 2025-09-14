# Mesh RT Support Guide for 1004–1006 (2025-09-14)

Purpose
- Increase implementation success rate for 1004–1006 with precise checklists, pitfalls, validation checks, and MCP spec queries. Assumes: RT features enabled, mesh path stable, TLAS build works, runtime toggle for RT exists.

General invariants
- Dynamic rendering only: never issue barriers/copies inside active rendering blocks.
- Descriptor layouts must match shader expectations across SDR/HDR variants if both exist.
- Use Buffer Device Address for AS geometry buffers; verify non-null device addresses before AS builds.

---

## 1004 — Real occluder geometry and correct placement

What to build
- A CPU icosphere (subdivisions 2–3) for the black hole sphere.
- A CPU annulus (disc with inner radius) for the accretion disc. Default radii: inner=1.0, outer=6.0, in XZ.
- Rebuild BLAS for both meshes and update TLAS with instance transforms placed to intersect the particle cloud under the chosen light direction.

Checklist
- Geometry buffers
  - Vertex format: position only, XYZ as float32 (VK_FORMAT_R32G32B32_SFLOAT)
  - Index type: VK_INDEX_TYPE_UINT32 (prefer) or UINT16 if small
  - Buffer usage: VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT
  - Device address queried via vkGetBufferDeviceAddress; log non-null
- BLAS build
  - Fill VkAccelerationStructureGeometryTrianglesDataKHR: vertexFormat, vertexData.deviceAddress, vertexStride, indexType, indexData.deviceAddress; transformData = 0
  - Query build sizes; allocate AS buffer and scratch
  - Record vkCmdBuildAccelerationStructuresKHR
- TLAS instances
  - VkAccelerationStructureInstanceKHR.transform is ROW-MAJOR 3x4; populate from a world transform matrix (no perspective)
  - InstanceCustomIndex = 0..n (optional), mask = 0xFF, instanceShaderBindingTableRecordOffset = 0, flags = 0
  - Place sphere at origin (scale=radius), disc centered at origin with a tilt (20–35°) around X or Z
  - Ensure occluders sit between the light direction and the particle cloud (so they can cast onto it)
- Synchronization
  - After BLAS/TLAS builds, issue vkCmdPipelineBarrier2 from ACCELERATION_STRUCTURE_BUILD_BIT_KHR to FRAGMENT_SHADER_BIT
- Instrumentation
  - Log device addresses for geometry buffers, BLAS, and TLAS once after build
  - Optional: debug draw occluder bounds (AABB/wireframe) with a toggle

Pitfalls
- Row-major vs column-major instance transform; wrong order causes misplaced instances
- Using wrong vertex format/stride yields build failures or invisible shadows
- Occluders behind the light or not intersecting the cloud → no visible effect

Validation targets
- No validation during build; TLAS device address logged
- Visible darkening where occluders overlap particles with RT enabled

MCP queries
- search_vulkan_spec("VkAccelerationStructureGeometryTrianglesDataKHR")
- search_vulkan_spec("VkAccelerationStructureInstanceKHR transform row-major")
- search_vulkan_spec("vkCmdBuildAccelerationStructuresKHR")

---

## 1005 — Exit-time VUID cleanup (destroy everything in order)

Goal
- Eliminate VUID-vkDestroyDevice-device-05137 by destroying/freeing all child objects before vkDestroyDevice.

Checklist
- Add debug-only inventory of resources (per subsystem):
  - VkImage, VkImageView, VkBuffer, VkDeviceMemory (or VMA allocations), VkAccelerationStructureKHR, VkPipeline, VkPipelineLayout, VkDescriptorPool, VkDescriptorSetLayout
- Teardown order (high-level)
  1) Stop submitting/drawing; wait for device: vkDeviceWaitIdle
  2) Destroy graphics/compute pipelines, pipeline layouts
  3) Free descriptor sets (via pool reset) and destroy descriptor pools/set layouts
  4) Destroy TLAS, then BLAS; destroy AS backing buffers
  5) Destroy images views → images → free their device memory
  6) Destroy buffers → free their device memory
  7) Destroy swapchain image views, then swapchain
  8) Destroy query pools, samplers, and any remaining objects
- Known image/buffer owners to audit
  - TAA: history/current images, views, sampler (if any)
  - Bloom: ping/pong images, views, samplers
  - Volume renderer: density image and mip views; STBN array; optical depth LUT; any DSM images if present
  - Mesh renderer: geometry buffers for occluders; particle storage buffers
  - Swapchain: image views; (images are owned by swapchain, do not destroy directly)
- Instrument shutdown
  - Print counts per resource type destroyed in debug
  - If validation still reports leaks, print the tracked IDs you believe you destroyed to reconcile

Pitfalls
- Forgetting to destroy image views (leaves the image referenced)
- Not freeing VkDeviceMemory after destroying raw images/buffers (VMA is currently disabled)
- Destroying device before pools/pipelines/images

Validation targets
- No 05137 VUIDs on three consecutive runs/shutdowns

MCP queries
- search_vulkan_spec("Object lifetime and destruction order")
- search_vulkan_spec("vkDestroyDevice children must be destroyed")

---

## 1006 — Soft shadows (jitter) + temporal stabilization

Goal
- Reduce hard edges/shimmer by jittering the ray direction and temporally accumulating visibility.

Minimal plan (no new descriptors)
- Jitter: compute a tiny cone perturbation around the light direction with a per-pixel hash seeded by frame index
  - Build an orthonormal basis (T,B,N) from lightDir; sampled angle ~1–2°
  - dirJittered = normalize(N * cos(theta) + (T*cos(phi)+B*sin(phi)) * sin(theta))
- Temporal: maintain a per-pixel running average in an R8 or R16 target (allocate a small half-res buffer if bandwidth is tight)
  - vis_accum = clamp(mix(prev, curr, alpha), prev - k, prev + k)
  - alpha ~ 0.1; k ~ 0.25; skip accumulation when discontinuities are detected (optional)

Optional STBN integration
- If you already bind STBN in the mesh set, use a single-channel layer for phi/theta sampling; otherwise the hash fallback avoids new bindings

Checklist
- Add a small runtime toggle and console line: “RT soft shadows ON, cone=1.5°, alpha=0.1”
- Keep 1 ray-query per fragment; avoid loops
- Ensure the visibility target usage: COLOR_ATTACHMENT | SAMPLED | STORAGE or COPY as needed; transitions with Sync2 outside rendering

Pitfalls
- Excessive jitter angle → grainy shadows; too small → hard edges
- Accumulating across rapid lighting/camera changes without clamps → ghosting
- sRGB vs linear mismatch on the visibility target if sampled

Validation targets
- Penumbrae visible with reduced shimmer
- Stable over camera motion with minimal ghosting

MCP queries
- search_vulkan_spec("GL_EXT_ray_query")
- search_vulkan_spec("VkImageLayout for sampled images during postprocess")

---

Triage workflow
- If shadows do not appear after 1004: toggle debug bounds; log instance transforms; check that occluders lie between light and particles
- If exit VUIDs persist after 1005: enable per-type counters; identify which owner never destroys a resource
- If softening causes instability: lower cone angle, increase clamp; verify visibility target format/layout

Acceptance recap
- 1004: Visible darkening beneath occluders; TLAS device address logged; no build VUIDs
- 1005: No 05137 VUIDs on clean exit, three times in a row
- 1006: Soft penumbrae with minimal shimmer; stable temporal behavior
