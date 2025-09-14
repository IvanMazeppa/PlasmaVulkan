# Run Log Analysis v2 (2025-09-13) — Minimal Crash-Stop Patch Set for Claude

Source: logs/run.txt. Repeated VUIDs indicate two primary crash-drivers:
- Barriers/copies executed inside dynamic rendering instances
- Color-attachment format mismatch between bound pipeline and active target
Secondary issues: descriptor type/layout mismatches, missing transfer usage, mip-range/storage view hazards, draws outside active rendering, and shutdown leaks.

This v2 focuses on a minimal, ordered patch set to stop the crash first, then re-enable features.

---

## A) Minimal crash-stop patch set (apply in order)

1) Move all barriers/copies outside dynamic rendering
- Files:
  - `src/core/Application.cpp`
  - `src/systems/VolumeRenderer.cpp` (remove transitions from `compositeTAAResult()`)
- Rules:
  - Never call `vkCmdPipelineBarrier` or `vkCmdCopyImage` between `vkCmdBeginRendering` and `vkCmdEndRendering`.
  - Flow example per frame:
    - Begin rendering → draw
    - End rendering
    - Do image transitions/copies
    - Begin rendering → draw
- Acceptance:
  - No VUID 09553/09554/09556/01181 after a frame.

2) Correct color-attachment formats per pipeline
- Files: `src/systems/VolumeRenderer.cpp`
- Create two volume pipelines:
  - Swapchain pipeline: `pColorAttachmentFormats[0] = getSwapChainImageFormat()`
  - HDR pipeline (for TAA current): `pColorAttachmentFormats[0] = VK_FORMAT_R16G16B16A16_SFLOAT`
- Bind HDR pipeline only when rendering to `m_taaCurrentImage`; bind swapchain pipeline only when rendering to a swapchain image.
- Acceptance:
  - No VUID 08910.

3) Eliminate image copies for TAA composition (optional but simplest)
- Files:
  - `src/systems/VolumeRenderer.cpp` → `compositeTAAResult()`
- Replace `vkCmdCopyImage` with shader-based full-screen composite: sample history/current and write to the active color attachment.
- If you must copy, add proper usage flags (see step 4) and do transitions outside rendering.
- Acceptance:
  - All copy-related VUID 06662 disappear, or composite path has no copies.

4) Fix image usage flags if copying remains
- Files:
  - `src/systems/VolumeRenderer.cpp` → `createTAAResources()`
- Add `VK_IMAGE_USAGE_TRANSFER_SRC_BIT` to any image you copy from (e.g., `m_taaCurrentImage`).
- Acceptance:
  - No VUID 06662 during copies.

5) Ensure descriptor image layouts match actual layouts when sampled
- Files:
  - TAA/density images where descriptors use `SHADER_READ_ONLY_OPTIMAL`
- After any copy/clear, transition images to the layout you will sample (usually `SHADER_READ_ONLY_OPTIMAL`) before drawing, and set `VkDescriptorImageInfo::imageLayout` accordingly.
- Transitions must be outside dynamic rendering.
- Acceptance:
  - No VUID 00344/09600.

6) Bracket every draw with `vkCmdBeginRendering`/`vkCmdEndRendering`
- Files: `src/core/Application.cpp`
- Audit TAA path: after ending rendering to go off-screen, ensure the next draw is inside a new `vkCmdBeginRendering`.
- Acceptance:
  - No "vkCmdDraw(): This call must be issued inside an active render pass" VUIDs.

7) Standardize on `vkCmdPushDescriptorSetKHR`
- Files: `src/systems/VolumeRenderer.cpp`
- Replace any `vkCmdPushDescriptorSet` with `vkCmdPushDescriptorSetKHR`. Assert the function pointer after `volkLoadDevice`.
- Acceptance:
  - No push-descriptor related crashes; stable descriptor updates.

8) Clean shutdown leaks
- Files:
  - `src/systems/VolumeRenderer.cpp` (AS scratch buffers, AS buffers, TLAS/BLAS)
  - Any transient TAA/bloom images
- Destroy all child objects prior to device destruction.
- Acceptance:
  - No VUID 05137 on shutdown.

---

## B) RT-specific fixes (enable after A is stable)

B1) Descriptor type for TLAS (binding 3)
- Files: `src/systems/VolumeRenderer.cpp` → `createVolumeRenderPipeline()`
- When RT is enabled, set binding 3 to `VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR`.
- Push with `VkWriteDescriptorSetAccelerationStructureKHR` via `pNext`.
- Confirm `shaders/volume.frag` declares `layout(binding=3) uniform accelerationStructureEXT topLevelAS;`.
- Acceptance: no VUID 07990.

B2) Storage/view split for 3D density image
- Files: density grid creation
- Create a storage view restricted to mip 0 (levelCount=1) for compute writes (layout GENERAL) and a separate sampled view spanning all mips for rendering.
- Acceptance: no GENERAL vs SHADER_READ_ONLY cross-mip VUIDs (09600 family).

---

## C) Instrumentation (temporary, remove after pass)

- Print once when creating each graphics pipeline: which `pColorAttachmentFormats[0]` it uses and a tag ("volume-SDR" vs "volume-HDR").
- Print when beginning/ending dynamic rendering: "BEGIN Rendering (format=..., extent=...)" / "END Rendering".
- Before pushes: dump descriptor writes for volume: `{binding, type, layout}` for bindings 0..N.

---

## D) Acceptance test checklist (run after each step)
- No VUID 09553/09554/09556/01181 (barriers/copies now outside rendering).
- No VUID 08910 (pipeline attachment format matches target).
- No VUID 06662 (copy usage flags addressed) or no copies used.
- No VUID 00344/09600 (descriptor imageLayout matches actual; no mip-range conflict).
- No "draw outside active render pass" VUIDs.
- Shutdown: no VUID 05137.
- RT later: no VUID 07990.

---

## E) Notes on Claude’s hypothesis (HDR split may fix RT VUID)
- The 07990 report originates at pipeline creation time for the volume pipeline. If HDR/SDR pipelines build with divergent descriptor layouts, you can see a mismatch linger. Unify descriptor set layout creation into a single helper used by both pipelines, switching only the color attachment format between pipelines.

---

## F) Two-AI collaboration protocol (fast, token-efficient)

- You feed: run/build logs (tee to file); I reply with a short ordered delta list (like A/B above) + VUID anchors.
- Claude applies only the next 1–3 steps, commits, re-runs; you paste new logs.
- Token limits:
  - Cap per exchange at ~600–800 tokens. If logs are long, paste only the first instance of each unique VUID plus 10–15 lines of context.
- Guardrails:
  - Keep features gated (toggle Ray Query use) so crashes can be bisected quickly.
  - Do not mix pipeline re-creation, barriers, and descriptor layout edits in the same commit.

---

## G) MCP queries (for Claude’s reference)
- search_vulkan_spec("vkCmdBeginRendering")
- search_vulkan_spec("vkCmdPipelineBarrier2")
- search_vulkan_spec("Image Layouts")
- search_vulkan_spec("VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR")
- search_vulkan_spec("VK_KHR_ray_query")
- search_vulkan_spec("VK_KHR_acceleration_structure")
