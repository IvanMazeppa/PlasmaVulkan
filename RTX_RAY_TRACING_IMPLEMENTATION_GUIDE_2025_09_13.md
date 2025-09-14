# RTX Ray Tracing Implementation Guide — Start Here (2025-09-13)

This guide replaces DSM for now and takes you from zero-to-first‑light using `VK_KHR_ray_query` for physically plausible occluder shadows integrated directly into your existing volumetric renderer. It is additive: no render pass surgery, no separate RT pipeline required.

Scope
- Features & extensions to enable (instance/device)
- BLAS/TLAS build (complete, practical)
- Descriptor and pipeline layout
- GLSL ray query integration in `volume.frag`
- Synchronization2 barriers
- Resize/update/rebuild policy
- Validation, debugging, performance tuning
- Next steps (soft shadows, hybrid DSM/RT, full RT pipeline)
- MCP spec queries you can run to cross‑check

Shelved
- DSM is shelved in this path; keep the code but bypass sampling while iterating RT.

---

## 0) Prerequisites & Project State

- You already request Vulkan 1.4 features (`synchronization2`, `dynamicRendering`, `bufferDeviceAddress`).
- You have scaffolding for BLAS/TLAS creation in `VolumeRenderer.cpp`, but you do not build them yet (comment indicates TODO). This guide fills those gaps and wires `rayQuery` in `volume.frag`.
- Push descriptors are used; we will keep that and add TLAS binding (5) when RT is present.

---

## 1) Enable Required RTX Features and Extensions

Device extensions (add if missing)
- `VK_KHR_acceleration_structure`
- `VK_KHR_ray_query`
- `VK_KHR_deferred_host_operations` (required by acceleration structure extension)

Device features chain
- `VkPhysicalDeviceRayQueryFeaturesKHR { rayQuery = VK_TRUE }`
- `VkPhysicalDeviceAccelerationStructureFeaturesKHR { accelerationStructure = VK_TRUE }`
- `VkPhysicalDeviceBufferDeviceAddressFeatures { bufferDeviceAddress = VK_TRUE }` (you already enable via Vulkan 1.2)

Function pointers (via volk)
- `vkCreateAccelerationStructureKHR`
- `vkDestroyAccelerationStructureKHR`
- `vkGetAccelerationStructureBuildSizesKHR`
- `vkCmdBuildAccelerationStructuresKHR`
- `vkGetAccelerationStructureDeviceAddressKHR`

MCP
- search_vulkan_spec("VK_KHR_acceleration_structure")
- search_vulkan_spec("VK_KHR_ray_query")
- search_vulkan_spec("buffer device address")

Checklist
- Ensure `deviceExtensions.push_back(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);`
- Ensure `deviceExtensions.push_back(VK_KHR_RAY_QUERY_EXTENSION_NAME);`
- Ensure `deviceExtensions.push_back(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);`
- Chain `VkPhysicalDeviceRayQueryFeaturesKHR` → `VkPhysicalDeviceAccelerationStructureFeaturesKHR` → next.

---

## 2) BLAS Build (triangle geometry)

Goal
- Convert your ground plane (2 triangles) into a built BLAS.

Geometry
- Vertex format: `VK_FORMAT_R32G32B32_SFLOAT`
- No indices needed (optional). If using indices, set indexType accordingly.

Steps
1) Create/Upload vertex buffer with `VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR`.
2) Get device address via `vkGetBufferDeviceAddress`.
3) Describe geometry:
```cpp
VkAccelerationStructureGeometryKHR geom{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
geom.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
geom.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
geom.geometry.triangles = {
  VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR,
  nullptr,
  VK_FORMAT_R32G32B32_SFLOAT,
  vertexAddress, // VkDeviceAddress
  sizeof(float)*3,
  VK_NULL_HANDLE, // indexData (if used)
  VK_INDEX_TYPE_NONE_KHR, // or UINT32
  VK_NULL_HANDLE, // transformData
  maxVertex // highest index (vertexCount-1)
};
```
4) Build sizes:
```cpp
uint32_t primitiveCount = 2; // two triangles
VkAccelerationStructureBuildGeometryInfoKHR buildInfo{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
buildInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
buildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
buildInfo.geometryCount = 1;
buildInfo.pGeometries = &geom;
VkAccelerationStructureBuildSizesInfoKHR sizeInfo{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
vkGetAccelerationStructureBuildSizesKHR(device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &buildInfo, &primitiveCount, &sizeInfo);
```
5) Create BLAS buffer with `VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT`, allocate/bind.
6) Create BLAS:
```cpp
VkAccelerationStructureCreateInfoKHR asCreate{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR };
asCreate.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
asCreate.buffer = blasBuffer;
asCreate.size = sizeInfo.accelerationStructureSize;
vkCreateAccelerationStructureKHR(device, &asCreate, nullptr, &m_bottomLevelAS);
```
7) Scratch buffer: `sizeInfo.buildScratchSize` with `VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT`.
8) Record build:
```cpp
VkAccelerationStructureBuildRangeInfoKHR range{}; range.primitiveCount = primitiveCount;
const VkAccelerationStructureBuildRangeInfoKHR* pRanges[1] = { &range };
buildInfo.dstAccelerationStructure = m_bottomLevelAS;
buildInfo.scratchData.deviceAddress = scratchAddress;
vkCmdBuildAccelerationStructuresKHR(cmd, 1, &buildInfo, pRanges);
```
9) Barrier to ensure BLAS is readable by TLAS build:
```cpp
VkMemoryBarrier2 barrier{ VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
barrier.srcStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
barrier.srcAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
barrier.dstStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
barrier.dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;
VkDependencyInfo dep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO, nullptr, 0,nullptr, 1, &barrier, 0,nullptr };
vkCmdPipelineBarrier2(cmd, &dep);
```

MCP
- search_vulkan_spec("vkGetAccelerationStructureBuildSizesKHR")
- search_vulkan_spec("vkCmdBuildAccelerationStructuresKHR")
- search_vulkan_spec("Acceleration Structure build flags")

---

## 3) TLAS Build (instances)

1) Instance with identity transform referencing BLAS device address:
```cpp
VkAccelerationStructureDeviceAddressInfoKHR addrInfo{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR };
addrInfo.accelerationStructure = m_bottomLevelAS;
VkDeviceAddress blasAddr = vkGetAccelerationStructureDeviceAddressKHR(device, &addrInfo);

VkAccelerationStructureInstanceKHR inst{};
inst.transform = { 1,0,0,0,  0,1,0,0,  0,0,1,0 };
inst.instanceCustomIndex = 0;
inst.mask = 0xFF;
inst.instanceShaderBindingTableRecordOffset = 0;
inst.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
inst.accelerationStructureReference = blasAddr;
```
2) Upload `inst` to a buffer with `VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT` and get its device address.
3) TLAS build:
```cpp
VkAccelerationStructureGeometryKHR tlasGeom{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
tlasGeom.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
tlasGeom.geometry.instances = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR };
tlasGeom.geometry.instances.data.deviceAddress = instanceBufferAddress;

VkAccelerationStructureBuildGeometryInfoKHR tlasInfo{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
tlasInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
tlasInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
tlasInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
tlasInfo.geometryCount = 1; tlasInfo.pGeometries = &tlasGeom;

uint32_t instanceCount = 1;
VkAccelerationStructureBuildSizesInfoKHR sizeInfo{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
vkGetAccelerationStructureBuildSizesKHR(device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &tlasInfo, &instanceCount, &sizeInfo);

// Create TLAS buffer + TLAS object
VkAccelerationStructureCreateInfoKHR create{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR };
create.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
create.buffer = tlasBuffer; create.size = sizeInfo.accelerationStructureSize;
vkCreateAccelerationStructureKHR(device, &create, nullptr, &m_topLevelAS);

// Scratch + build
VkAccelerationStructureBuildRangeInfoKHR range{}; range.primitiveCount = instanceCount;
const VkAccelerationStructureBuildRangeInfoKHR* ranges[1] = { &range };
tlasInfo.dstAccelerationStructure = m_topLevelAS;
tlasInfo.scratchData.deviceAddress = tlasScratchAddress;
vkCmdBuildAccelerationStructuresKHR(cmd, 1, &tlasInfo, ranges);
```
4) Barrier to make TLAS visible to fragment shader ray queries:
```cpp
VkMemoryBarrier2 asBarrier{ VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
asBarrier.srcStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
asBarrier.srcAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
asBarrier.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
asBarrier.dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;
VkDependencyInfo dep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO, nullptr, 0,nullptr, 1,&asBarrier, 0,nullptr };
vkCmdPipelineBarrier2(cmd, &dep);
```

MCP
- search_vulkan_spec("VkAccelerationStructureInstanceKHR")
- search_vulkan_spec("vkGetAccelerationStructureDeviceAddressKHR")

---

## 4) Descriptor and Pipeline Layout

- Add binding 5 (TLAS) to volume descriptor set layout when RT is supported:
```cpp
layout(binding = 5) uniform accelerationStructureEXT topLevelAS;
```
- You already gate binding 5 on `supportsRayTracing()`. Keep that.
- Ensure push descriptors include binding 5 with `VkWriteDescriptorSetAccelerationStructureKHR` in all paths that render with RT.

MCP
- search_vulkan_spec("VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR")

---

## 5) GLSL Integration in volume.frag (Ray Query Shadows)

Enable extension and bind TLAS
```glsl
#extension GL_EXT_ray_query : require
layout(binding = 5) uniform accelerationStructureEXT topLevelAS;
```

Directional occlusion test along lightDir
```glsl
bool hasOccluderRT(vec3 originWS, vec3 dirWS, float tMax)
{
    rayQueryEXT rq;
    uint flags = gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsOpaqueEXT;
    rayQueryInitializeEXT(rq, topLevelAS, flags, 0xFF, originWS, 0.001, normalize(dirWS), tMax);
    while (rayQueryProceedEXT(rq)) { }
    const uint type = rayQueryGetIntersectionTypeEXT(rq, true);
    return (type != gl_RayQueryIntersectionNoneEXT);
}
```

Use in your shading block (replace DSM usage while DSM is shelved)
```glsl
// Inside doExpensiveShading block where you currently call sampleDSM(pos)
vec3 lightDir = normalize(vec3(-0.5, -0.8, -0.6));
float tMax = 200.0; // or distance to scene bounds
bool blocked = hasOccluderRT(pos, lightDir, tMax);
float shadowVisibility = blocked ? 0.0 : 1.0;
```

Notes
- Keep your existing gating (`doExpensiveShading`) so RT only runs when density is meaningful.
- Later you can combine RT with DSM (when DSM is fixed) via `visibility = min(visibilityDSM, visibilityRT)`.

MCP
- search_vulkan_spec("GLSL rayQueryInitializeEXT")
- search_vulkan_spec("GL_EXT_ray_query")

---

## 6) Synchronization2 — graphics after AS build

When BLAS/TLAS are built/refit in frame N and sampled in the same frame:
```cpp
VkMemoryBarrier2 barrier{ VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
barrier.srcStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
barrier.srcAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
barrier.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
barrier.dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;
VkDependencyInfo dep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO, nullptr, 0,nullptr, 1,&barrier, 0,nullptr };
vkCmdPipelineBarrier2(cmd, &dep);
```

MCP
- search_vulkan_spec("vkCmdPipelineBarrier2")
- search_vulkan_spec("Synchronization2 acceleration structure hazards")

---

## 7) Resize & Updates

- Swapchain resize does not require rebuilding BLAS/TLAS unless geometry or instances change.
- If you move geometry or instance transforms, prefer TLAS refit/UPDATE (if supported) for performance. Otherwise, rebuild.
- Rebuild barriers identical to Section 6.

MCP
- search_vulkan_spec("VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR")

---

## 8) Validation & Debugging

- Enable validation features (GPU assisted) to catch AS misuse.
- Sanity tests:
  - Place the ground plane above/below the volume and verify occlusion flips when lightDir changes sign.
  - Render a debug full‑screen: `fragColor = blocked ? vec4(0,0,0,1) : vec4(1);` at a single shaded sample to confirm behavior.
  - Confirm descriptor binding 5 is set (print push writes once) when RT enabled.

Common pitfalls
- Missing `VK_KHR_deferred_host_operations` → AS creation fails.
- No buffer device address or missing address flags on scratch/geom buffers.
- Forgetting barriers after build before sampling.
- Not compiling GLSL with ray query support in your build system.

---

## 9) Performance Tuning

- Keep `TerminateOnFirstHitEXT` and `OpaqueEXT` flags for fast boolean occlusion.
- Restrict RT calls to meaningful shading points (`dens > threshold`, stride gating as you already do).
- Use simple proxy meshes as occluders (low poly), not detailed ones.
- Consider a per‑frame toggle to profile RT cost vs. quality.

---

## 10) Next Steps (after first light)

- Soft shadows: multi‑ray stochastic sampling or temporal accumulation of a single jittered ray.
- Hybrid DSM+RT: DSM for volumetric self‑shadowing + RT for hard geometry occluders.
- Full RT Pipeline (optional): use `VK_KHR_ray_tracing_pipeline` for advanced effects (area lights, shadow filtering, SBT). Heavier lift.

MCP
- search_vulkan_spec("VK_KHR_ray_tracing_pipeline")

---

## 11) Build System Notes

GLSL (glslangValidator)
- Ensure toolchain supports `GL_EXT_ray_query` and emits `SPV_KHR_ray_query`.
- Target env ≥ Vulkan 1.2: e.g., `--target-env vulkan1.2` or higher.

CMake
- Add a shader compile rule for `volume.frag` with ray query enabled.
- Guard the shader path with a compile‑time `#ifdef` so you can switch DSM/RT quickly.

---

## 12) Bring‑Up Checklist

- [ ] Device extensions/features enabled: AS + Ray Query + Deferred Host Ops.
- [ ] BLAS built with barriers.
- [ ] TLAS built with barriers.
- [ ] Descriptor binding 5 (TLAS) added and pushed.
- [ ] `volume.frag` includes ray query code; DSM sampling bypassed for now.
- [ ] Frame runs without validation errors; occlusion toggles as expected.
- [ ] Gated RT calls to keep performance high; profile FPS.

---

## Appendix — MCP queries to run
- search_vulkan_spec("VK_KHR_acceleration_structure")
- search_vulkan_spec("VK_KHR_ray_query")
- search_vulkan_spec("vkGetAccelerationStructureBuildSizesKHR")
- search_vulkan_spec("vkCmdBuildAccelerationStructuresKHR")
- search_vulkan_spec("vkGetAccelerationStructureDeviceAddressKHR")
- search_vulkan_spec("VkAccelerationStructureInstanceKHR")
- search_vulkan_spec("vkCmdPipelineBarrier2")
- search_vulkan_spec("VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR")
- search_vulkan_spec("VK_KHR_ray_tracing_pipeline")
