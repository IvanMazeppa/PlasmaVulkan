# Ray Query Crash Debug Guide — Volumetric Mode (2025-09-13)

Goal: make Ray Query shadows run without crashing, with minimal code changes. This checklist targets likely crash points in your current implementation and gives quick probes to isolate the fault. MCP spec queries are included for each critical area.

Applies to
- `src/renderer/VulkanContext.cpp`
- `src/systems/VolumeRenderer.{h,cpp}`
- `shaders/volume.frag`
- `src/core/Application.cpp`

Context snapshot
- Device extensions and features appear enabled for ray query + acceleration structures.
- BLAS/TLAS objects are created in `createAccelerationStructures()`; actual builds occur once in `buildAccelerationStructures(cmd)` (called at frame record start, before dynamic rendering).
- Volumetric render path pushes TLAS at binding 3 (ray query path), DSM is shelved.

---

## Top suspects (fix/check in this order)

1) Push descriptor function selection
- Symptom: crash on first `vkCmdPushDescriptorSet(...)` that includes TLAS write.
- Your code calls both `vkCmdPushDescriptorSet` (no KHR) and `vkCmdPushDescriptorSetKHR`. Only the KHR symbol is guaranteed by the extension.
- Action: temporarily print and assert that `vkCmdPushDescriptorSetKHR` is non‑null (volk loads it), and replace every `vkCmdPushDescriptorSet` with `vkCmdPushDescriptorSetKHR` across the codebase.

MCP
- search_vulkan_spec("VK_KHR_push_descriptor")
- search_vulkan_spec("vkCmdPushDescriptorSetKHR")

2) Acceleration structure descriptor write correctness
- Ensure the TLAS write uses:
  - `descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR`
  - `pNext = &VkWriteDescriptorSetAccelerationStructureKHR{ sType = ..., pAccelerationStructures = &m_topLevelAS, count = 1 }`
  - `dstBinding` matches the shader binding (your `volume.frag` uses binding 3)
- Print once per frame (until stable): dstBinding, descriptorType, pNext != nullptr, AS handle value.

MCP
- search_vulkan_spec("VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR")

3) BLAS/TLAS build order and barriers
- Build must happen BEFORE you start dynamic rendering, and you must sync:
```cpp
// After vkCmdBuildAccelerationStructuresKHR(...)
VkMemoryBarrier2 asBarrier{ VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
asBarrier.srcStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
asBarrier.srcAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
asBarrier.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
asBarrier.dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;
VkDependencyInfo dep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO, nullptr, 0,nullptr, 1,&asBarrier, 0,nullptr };
vkCmdPipelineBarrier2(cmd, &dep);
```
- You already record `buildAccelerationStructures(cmd)` before `vkCmdBeginRendering` — keep it that way.

MCP
- search_vulkan_spec("vkCmdBuildAccelerationStructuresKHR")
- search_vulkan_spec("vkCmdPipelineBarrier2")

4) Shader side: compile and bind sanity
- GLSL: `#extension GL_EXT_ray_query : require` (or `: enable` if you want to keep portability guards) and binding must match layout (binding=3).
- Build system must emit `SPV_KHR_ray_query`. If unsure, dump SPIR-V and search for `OpRayQueryInitializeKHR`.
- Crash on draw with no validation output often means shader binary mismatch (extension ops not recognized) → recompile `shaders/volume.frag` with ray query enabled.

MCP
- search_vulkan_spec("GL_EXT_ray_query")

5) Guard usage until AS exists
- In `volume.frag`, gate the RT call with a uniform push flag `rtEnabled` or reuse a constant until bring‑up:
```glsl
bool useRT = true; // set via push constants (temporary)
float rtVis = 1.0;
if (useRT) {
    rtVis = hasOccluderRT(pos, lightDir, tMax) ? 0.0 : 1.0;
}
```
- In CPU, set `useRT = (supportsRayTracing && s_accelerationStructuresBuilt)`. If the crash stops when false, the issue is in the RT path (descriptor or AS state).

6) Descriptor set layout consistency
- At pipeline creation, when RT is supported, the volume descriptor set layout must include binding 3 with type `ACCELERATION_STRUCTURE_KHR`.
- At runtime, when you push descriptors, the number of writes must equal the layout bindings you declared. Dump `descriptorCount` and each `dstBinding` during bring‑up. Mismatch can crash or trigger undefined behavior.

---

## Concrete probes to localize the crash

A) Verify TLAS handle and build path (first frame only)
- After `vkCreateAccelerationStructureKHR` (TLAS): log handle.
- After `buildAccelerationStructures(cmd)`: set a static `s_accelBuilt = true;` and log once.
- Before pushing TLAS descriptor: `assert(m_topLevelAS != VK_NULL_HANDLE);` and log handle.

B) Verify `vkCmdPushDescriptorSetKHR` pointer
```cpp
// After volkLoadDevice
assert(vkCmdPushDescriptorSetKHR && "vkCmdPushDescriptorSetKHR not loaded");
```

C) Validate descriptor write struct
- Sanity print (once):
  - `descriptorWrites[3].descriptorType == VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR`
  - `descriptorWrites[3].pNext != nullptr`
  - `((VkWriteDescriptorSetAccelerationStructureKHR*)descriptorWrites[3].pNext)->pAccelerationStructures[0] == m_topLevelAS`

D) Shader compile proof (one‑time)
- Recompile `volume.frag` → SPIR-V.
- Check disassembly contains `OpExtension "SPV_KHR_ray_query"` and ray query ops.

E) Quick shading bypass test
- Temporarily set `shadowVisibility = 1.0;` in the RT block. If the crash disappears, it is in RT call/descriptor/AS. If it persists, look at descriptors/pipeline/layout.

---

## Remove DSM impact while iterating RT
- Ensure the RT pipeline path no longer references DSM bindings. Your current code already sets the descriptor set layout to 3 or 4 bindings (adds binding 3 as TLAS when RT). Keep DSM code compiled out of descriptor layouts/writes during RT bring‑up to reduce variables.

---

## Known pitfalls and their fixes

- Mixed `vkCmdPushDescriptorSet` vs `vkCmdPushDescriptorSetKHR`
  - Use only `vkCmdPushDescriptorSetKHR`. Keep the `VK_KHR_push_descriptor` extension enabled and set `VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT` on the layout.
- AS build but no barrier before fragment read
  - Must add Synchronization2 barrier (see Section 3).
- Shader/descriptor mismatch
  - If RT is enabled at compile time, the runtime layout and descriptor pushes must include TLAS at binding 3; otherwise create a non‑RT variant of the pipeline.
- Missing `VK_KHR_deferred_host_operations`
  - Required by `VK_KHR_acceleration_structure`. Ensure device enables it.
- Wrong device address flags
  - Scratch buffers for builds must have `VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT` and memory allocated with `VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT`.

MCP
- search_vulkan_spec("VK_KHR_deferred_host_operations")
- search_vulkan_spec("VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT")

---

## Minimal debug print points to add (remove after fix)

- After logical device creation (once): "Ray Query support: YES/NO", "Acceleration Structure support: YES/NO" (already printed).
- In `createAccelerationStructures()`: print BLAS/TLAS handle values after creation.
- In `buildAccelerationStructures(cmd)`: print once after barrier: "AS built and synced".
- In `render()` just before pushing descriptors: print `descriptorCount` and a compact list of `{binding, type}`; assert TLAS write has `pNext`.
- In shader: atomically switch to `useRT=false` to falsify the RT block and check if the crash disappears.

---

## Validation & tooling

- Enable GPU‑assisted validation for descriptor and AS errors.
- Capture a short RenderDoc trace at the moment of crash. Inspect the pipeline state → descriptors → check the acceleration structure descriptor binding and handle.

---

## Bring‑up checklist
- [ ] All `vkCmdPushDescriptorSet` calls converted to `vkCmdPushDescriptorSetKHR` and pointer verified non‑null.
- [ ] TLAS descriptor write: correct type, pNext, binding index, and handle.
- [ ] BLAS/TLAS builds occur once per boot (or on scene change) BEFORE any draw that queries RT.
- [ ] AS build → barrier to fragment read.
- [ ] Shader compiled with `GL_EXT_ray_query`, SPIR‑V contains ray query ops.
- [ ] Shader RT block gated by CPU flag for easy on/off triage.
- [ ] No DSM descriptors in the RT path while iterating.

---

## MCP queries to run
- search_vulkan_spec("VK_KHR_push_descriptor")
- search_vulkan_spec("vkCmdPushDescriptorSetKHR")
- search_vulkan_spec("VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR")
- search_vulkan_spec("vkCmdBuildAccelerationStructuresKHR")
- search_vulkan_spec("vkCmdPipelineBarrier2")
- search_vulkan_spec("GL_EXT_ray_query")
- search_vulkan_spec("VK_KHR_deferred_host_operations")
