# Run Log Analysis (2025-09-13) — Actionable Change Requests for Claude

Source: logs/run.txt (871 lines). The errors fall into a few categories: descriptor type/layout mismatches, dynamic rendering format mismatch, illegal barriers inside dynamic rendering, missing image usage bits for transfers, and draw calls outside active rendering. Below are precise change requests with VUIDs, file touch points, and acceptance tests.

---

## 1) RT descriptor layout mismatch (binding 3)
VUID: 07990
- Symptom:
  - “SPIR-V uses descriptor [Set 0, Binding 3] of type ACCELERATION_STRUCTURE_KHR but expected COMBINED_IMAGE_SAMPLER.”
- Cause:
  - Fragment shader declares `layout(binding=3) uniform accelerationStructureEXT topLevelAS;`, but `createVolumeRenderPipeline()` builds a layout where binding 3 is still `COMBINED_IMAGE_SAMPLER`.
- Required edits:
  - File: `src/systems/VolumeRenderer.cpp` → `createVolumeRenderPipeline()`
    - When `supportsRayTracing() == true`, define binding 3 as `VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR` for the volume descriptor set layout.
    - Ensure push descriptors use `vkCmdPushDescriptorSetKHR` and the TLAS write includes a valid `VkWriteDescriptorSetAccelerationStructureKHR` in `pNext`.
  - File: `shaders/volume.frag`
    - Confirm presence of `#extension GL_EXT_ray_query : require` and `layout(binding=3) uniform accelerationStructureEXT topLevelAS;`.
- Acceptance test:
  - Pipeline creation logs show no VUID 07990. Volume pipeline compiles successfully.

---

## 2) Dynamic rendering color format mismatch
VUID: 08910
- Symptom:
  - “pColorAttachments[0].imageView format (R16G16B16A16_SFLOAT) must match VkPipelineRenderingCreateInfo::pColorAttachmentFormats[0] (B8G8R8A8_SRGB).”
- Cause:
  - Rendering to HDR TAA target (`VK_FORMAT_R16G16B16A16_SFLOAT`) using a pipeline created for the swapchain format (`B8G8R8A8_*`).
- Required edits:
  - File: `src/systems/VolumeRenderer.cpp`
    - Create a separate volume graphics pipeline for HDR targets with `pColorAttachmentFormats[0] = VK_FORMAT_R16G16B16A16_SFLOAT`.
    - Use the HDR pipeline in `renderToTAATarget()`.
  - Keep the existing pipeline for direct rendering to the swapchain.
- Acceptance test:
  - No VUID 08910 when rendering to TAA current image; both pipelines bind and draw cleanly.

---

## 3) Illegal barriers/copies inside dynamic rendering
VUIDs: 09553, 09554, 09556, 01181
- Symptoms:
  - Multiple errors: vkCmdPipelineBarrier called “inside a dynamic rendering instance.” Image layout transitions within render pass.
- Cause:
  - Transitions/copies are being recorded between `vkCmdBeginRendering` and `vkCmdEndRendering`. `compositeTAAResult()` and some TAA flow are performing transitions while rendering is active.
- Required edits:
  - File: `src/core/Application.cpp`
    - Ensure the flow is: Begin rendering → draw → End rendering → do transitions/copies → Begin rendering → draw.
    - All `vkCmdPipelineBarrier` and `vkCmdCopyImage` calls must occur outside any active dynamic rendering instance.
  - File: `src/systems/VolumeRenderer.cpp`
    - In `compositeTAAResult()`: remove transitions/copies. This function should only bind the TAA pipeline and draw (sample history/current); do not transition images here.
- Acceptance test:
  - No VUID 09553/09554/09556/01181 while running a frame.

---

## 4) Missing image usage for transfer source
VUID: 06662
- Symptom:
  - “vkCmdCopyImage(): srcImage was created with SAMPLED|COLOR_ATTACHMENT but requires TRANSFER_SRC.”
- Cause:
  - Copying from `m_taaCurrentImage` (or similar) without `VK_IMAGE_USAGE_TRANSFER_SRC_BIT` in its create info.
- Required edits:
  - File: `src/systems/VolumeRenderer.cpp` → `createTAAResources()`
    - Add `VK_IMAGE_USAGE_TRANSFER_SRC_BIT` to `m_taaCurrentImage` if you continue copying from it.
  - Alternatively, avoid copies by doing shader-based composites (preferred) and remove the copy path.
- Acceptance test:
  - VUID 06662 no longer appears during TAA history update.

---

## 5) Descriptor image layout mismatches
VUIDs: 00344, 09600
- Symptoms:
  - “Descriptor layout SHADER_READ_ONLY_OPTIMAL doesn’t match previous known layout TRANSFER_SRC_OPTIMAL.”
  - “vkQueueSubmit2 expects image mip N to be in layout GENERAL—actual is SHADER_READ_ONLY_OPTIMAL.”
- Causes:
  - (A) After copies, images are left in `TRANSFER_SRC_OPTIMAL` but sampled as `SHADER_READ_ONLY_OPTIMAL`.
  - (B) Storage image descriptor for the 3D density grid uses a view spanning ALL mips with `imageLayout=GENERAL`, while some mips are transitioned to `SHADER_READ_ONLY_OPTIMAL` for sampling → validator expects all subresources in GENERAL (mip 1..7 errors).
- Required edits:
  - (A) After any copy, transition the source image back to `SHADER_READ_ONLY_OPTIMAL` before sampling; do this outside dynamic rendering and ensure the descriptor `imageLayout` matches the actual layout.
  - (B) File: `src/systems/VolumeRenderer.cpp` → density splat path
    - Create a dedicated storage image view for mip 0 only (levelCount=1) and bind that in the compute descriptor (imageLayout GENERAL).
    - Keep the separate sampled image view that spans all mips for rendering.
- Acceptance test:
  - No VUID 00344/09600 regarding mismatched image layouts; no mip-level GENERAL vs SHADER_READ conflicts for the 3D density image.

---

## 6) Draws outside active render pass
VUID: renderpass (vkCmdDraw inside active instance)
- Symptom:
  - “vkCmdDraw(): This call must be issued inside an active render pass.”
- Cause:
  - Some draw calls occur after `vkCmdEndRendering` but before `vkCmdBeginRendering` is called again.
- Required edits:
  - File: `src/core/Application.cpp`
    - Audit the TAA path: after ending the main rendering to render to TAA targets, ensure each subsequent draw is bracketed by a matching `vkCmdBeginRendering`/`vkCmdEndRendering`.
- Acceptance test:
  - No VUID “vkCmdDraw(): This call must be issued inside an active render pass.”

---

## 7) Use only vkCmdPushDescriptorSetKHR
- Symptom:
  - Mixed use of `vkCmdPushDescriptorSet` and `vkCmdPushDescriptorSetKHR` can crash on some drivers.
- Required edits:
  - Files: `src/systems/VolumeRenderer.cpp`
    - Replace any `vkCmdPushDescriptorSet` with `vkCmdPushDescriptorSetKHR` consistently.
    - After `volkLoadDevice`, assert that `vkCmdPushDescriptorSetKHR` is non-null.
- Acceptance test:
  - No push-descriptor related crashes; logs show consistent use of KHR variant.

---

## 8) Resource cleanup leaks
VUID: 05137 (vkDestroyDevice child objects not destroyed)
- Symptom:
  - Leaked buffers/images reported at device destruction.
- Required edits:
  - Ensure all temporary scratch buffers (BLAS/TLAS scratch), AS buffers, and any transient images created for TAA/bloom are destroyed in cleanup paths:
    - Files: `src/systems/VolumeRenderer.cpp` (AS buffers, scratch) and any TAA/bloom images in `Application.cpp` cleanup.
- Acceptance test:
  - No VUID 05137 on shutdown.

---

## Implementation order (commit by commit)
1) Fix RT binding 3 descriptor type and pushes (07990). Rebuild natively.
2) Add HDR volume pipeline and use in TAA path (08910).
3) Move all barriers/copies out of dynamic rendering; remove transitions from `compositeTAAResult()` (09553/09554/09556/01181).
4) Add TRANSFER_SRC usage where copying or remove copy path (06662).
5) Fix descriptor imageLayout lifetimes and storage-view-only-mip0 (00344/09600).
6) Bracket all draws with begin/end rendering (renderpass VUID).
7) Standardize on `vkCmdPushDescriptorSetKHR`.
8) Cleanup leaks.

Run after each step and paste fresh logs; the next changes should only proceed when current VUIDs vanish.

---

## Quick probes (optional, minimal instrumentation)
- Log once at pipeline creation the `pColorAttachmentFormats[0]` for volume vs. TAA pipelines.
- Log TLAS descriptor push: binding=3, type=AS_KHR, pNext != nullptr.
- Print when beginning/ending dynamic rendering to catch misplaced barriers/copies.

---

## MCP spec queries to reference
- search_vulkan_spec("VK_KHR_ray_query")
- search_vulkan_spec("VK_KHR_acceleration_structure")
- search_vulkan_spec("VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR")
- search_vulkan_spec("vkCmdBeginRendering")
- search_vulkan_spec("vkCmdPipelineBarrier2")
- search_vulkan_spec("Image Layouts") (VUID 00344, 01181)
- search_vulkan_spec("Copies vkCmdCopyImage usage flags") (VUID 06662)
