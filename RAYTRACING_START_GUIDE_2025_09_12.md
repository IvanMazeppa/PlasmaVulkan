# RTX/AI Options and Ray Tracing Start Guide — 2025-09-12

This guide lists pragmatic AI/RTX upgrades for image quality/perf, and a step‑by‑step plan to add Vulkan ray tracing, starting with `VK_KHR_ray_query` for volumetric shadows.

## RTX/AI techniques worth adding

- DLSS Super Resolution (via NVIDIA Streamline)
  - Upscale from 1440p→4K or 1080p→1440p; best QoS win with minimal code.
  - Also consider DLAA (anti‑aliasing at native res) if bandwidth is ample.
- DLSS Ray Reconstruction (or NRD)
  - If/when you add ray‑traced signals (shadows/GI), use DLSS‑RR (Ada) or NVIDIA NRD denoisers (REBLUR/RELAX) for temporal denoising.
- RTXDI (ReSTIR Direct Illumination)
  - Many‑lights friendly; pairs well with volumetrics if you later ray‑trace shadowed single scattering.
- Vendor‑neutral options
  - FSR2/FSR3 SR (ffx) as fallback; can be wired behind the same upscaler interface.

Notes
- Streamline supports Vulkan and abstracts DLSS/FSR paths; good fit for a research engine.

---

## Minimal Ray Tracing: Ray‑Query Shadows for Volumetrics
Goal: add hard shadowing (occlusion) to Henyey–Greenstein single scattering using ray queries from the volume fragment shader. No ray‑tracing pipeline required.

### 0) Enable features and extensions
- Required device features/extensions (add to `VulkanContext::createLogicalDevice`):
  - `VK_KHR_acceleration_structure`
  - `VK_KHR_ray_query`
  - `VK_KHR_deferred_host_operations`
  - `VK_KHR_buffer_device_address`
  - Descriptor indexing already present is helpful
- In the features chain, enable:
  - `VkPhysicalDeviceAccelerationStructureFeaturesKHR { accelerationStructure = VK_TRUE }`
  - `VkPhysicalDeviceRayQueryFeaturesKHR { rayQuery = VK_TRUE }`
  - `VkPhysicalDeviceBufferDeviceAddressFeatures { bufferDeviceAddress = VK_TRUE }`

### 1) Build a minimal AS (BLAS+TLAS)
- Start with a single BLAS containing one box that represents your scene geometry
  - If you already draw analytic scene (e.g., walls/floor) as triangles, use those triangles in BLAS.
  - For initial testing, create a single unit cube or a ground quad.
- Create TLAS with one instance referencing the BLAS.
- Use device‑address buffers for geometry and scratch; record `vkCmdBuildAccelerationStructuresKHR` once at startup (or per resize/scene change).

Checklist
- BLAS: `VkAccelerationStructureGeometryKHR` with triangles or AABBs
- Buffers have `VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT`
- Scratch buffer has `VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | SHADER_DEVICE_ADDRESS`

### 2) Expose TLAS to shaders
- Create a descriptor of type `VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR` and bind TLAS to it in a set visible to the volume pass.
- Push descriptors are fine, or allocate a small set.

### 3) Shader side (volume.frag)
- Enable the extension and add the descriptor:
```
#extension GL_EXT_ray_query : enable
layout(binding = X) uniform accelerationStructureEXT tlas;
```
- For each ray‑march step where you currently shade, cast a visibility ray toward the light:
```
vec3 L = normalize(lightPos - pos); // or directional
rayQueryEXT rq;
rayQueryInitializeEXT(rq, tlas, gl_RayFlagsTerminateOnFirstHitEXT, 0xFF,
                      pos + L * 0.001, 0.0, L, maxShadowDist);
bool occluded = false;
while (rayQueryProceedEXT(rq)) {
    if (rayQueryGetIntersectionTypeEXT(rq, true) != gl_RayQueryCandidateIntersectionTriangleEXT)
        continue;
}
occluded = (rayQueryGetIntersectionTypeEXT(rq, false) != gl_RayQueryCommittedIntersectionNoneEXT);
float visibility = occluded ? 0.0 : 1.0;
```
- Multiply your scattering/emission by `visibility`. Start with hard shadows; later replace with soft/denoised versions.

Performance tips
- Only fire a shadow ray when `dens > threshold` and every N steps.
- Use a smaller `maxShadowDist` than the whole scene.

### 4) Validation & testing
- Add a debug mode: render only visibility (white=1, black=0) to confirm the TLAS is working.
- Start with a single triangle/quad occluder and verify that volumetric shadows appear.

---

## Toward Full Ray Tracing (optional roadmap)
If you want a full ray‑tracing pipeline (`VK_KHR_ray_tracing_pipeline`):
1) Enable `VK_KHR_ray_tracing_pipeline` and pipeline features.
2) Create shader stages: `.rgen`, `.rmiss`, `.rchit` (and `.rint` for procedural, e.g., AABBs).
3) Pack SBT (shader binding table) with stage records.
4) For volumetrics, two approaches:
   - Hybrid: keep rasterized volume; use raygen for shadows/reflections only.
   - Procedural volume: TLAS contains an AABB for the volume; intersection shader does short ray‑march inside the box (then `anyhit` accumulates extinction). This is advanced but powerful.
5) Denoise any RT signals with NRD or DLSS RR.

---

## Integration with this codebase (where to put things)
- `VulkanContext.cpp` — add extensions/features to the pNext chains and enabled extension list; load function pointers via volk.
- `VolumeRenderer` — create/destroy AS, hold TLAS handle and its descriptor; bind it for the volume pass.
- `shaders/volume.frag` — enable `GL_EXT_ray_query`, add TLAS binding, compute visibility in the emission/scattering block.
- Resize — AS usually doesn’t change with window size; TLAS updates only if scene geometry or transforms change.

---

## MCP spec queries to keep handy
- search_vulkan_spec("VK_KHR_ray_query") — feature struct & usage
- search_vulkan_spec("VK_KHR_acceleration_structure") — build commands & geometry
- search_vulkan_spec("VK_KHR_ray_tracing_pipeline") — if you move beyond ray queries
- search_vulkan_spec("descriptor acceleration structure KHR") — descriptor setup

This plan gets you RT shadows quickly and safely inside the current rasterized volumetric pipeline, while leaving the door open for more advanced RTX features (RTXDI, NRD/ DLSS RR, and full ray‑traced volumetrics) once shadows are verified.
