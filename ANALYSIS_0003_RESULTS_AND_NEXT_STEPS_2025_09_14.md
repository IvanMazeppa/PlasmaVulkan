# Analysis of 0003 Job Results and Next Steps (2025-09-14)

Input: results/0003_implementation_results.json

Summary
- Volume mode still crashes at volume pipeline creation due to descriptor type mismatch (VUID 07990).
- Shutdown leaks persist (VUID 05137), mostly VkImage objects (HDR/bloom likely).
- TAA fixes (usage, layout, draw bracketing, storage-view split) look good.

Root cause (current blocker)
- Shader–pipeline layout mismatch: `shaders/volume.frag` declares `layout(binding=3) uniform accelerationStructureEXT topLevelAS;`, but the descriptor set layout used to create the volume pipelines still has binding 3 as `COMBINED_IMAGE_SAMPLER` somewhere in the path. Both SDR and HDR pipelines must share the same descriptor set layout object that defines binding 3 as `ACCELERATION_STRUCTURE_KHR` when RT is enabled.

Action plan (apply in order)
1) CR-0005: RT binding 3 = ACCELERATION_STRUCTURE_KHR + KHR push descriptors
- Ensure binding 3 type is `VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR` in the descriptor set layout used by BOTH volume pipelines (SDR/HDR).
- Push TLAS with `VkWriteDescriptorSetAccelerationStructureKHR` via `pNext`; use `vkCmdPushDescriptorSetKHR` everywhere.
- Verify `#extension GL_EXT_ray_query : require` and binding 3 in `volume.frag`.
- Acceptance: No VUID 07990 for SDR and HDR volume pipeline creation.

2) CR-0006: Clean leaks at shutdown (images/buffers)
- Track and free image memory for HDR/bloom images created in `Application`:
  - Add members for `VkDeviceMemory m_hdrColorImageMemory` (and any bloom image memories) in `Application.h`.
  - Bind and store memory when creating images (currently `hdrImageMemory` is local).
  - In `cleanupBloomResources()` (and `cleanup()` if needed): destroy images and free their memory counterparts.
- Recheck Volume/TAA images (already handled in `VolumeRenderer::cleanup`).
- Acceptance: No VUID 05137 on shutdown.

Additional guidance
- Unify descriptor layout creation into a helper that both SDR and HDR pipelines use; only color-attachment format differs between pipelines.
- Instrument once: print descriptor types for bindings 0..3 at pipeline creation; log which layout handle is used by each pipeline (sanity).

What’s already good
- Barriers/copies are now outside dynamic rendering.
- HDR/SDR pipeline split removed format mismatch.
- Storage-view mip0 split removes GENERAL vs SRV cross-mip conflicts.
- Draws are properly bracketed by dynamic rendering instances.

Deliverables
- changes/0005_rt_binding3_and_push_khr.json (added)
- changes/0006_cleanup_leaks_images_buffers.json (to add next)

After CR-0005
- Re-run; if clean, proceed to CR-0006 for leaks.
- If any residual descriptor errors persist, dump descriptor layout bindings (types) at creation and the fragment SPIR-V reflection of binding 3 to cross-check.
