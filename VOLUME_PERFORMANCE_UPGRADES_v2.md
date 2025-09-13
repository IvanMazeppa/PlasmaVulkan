# Volumetric Rendering - Performance Upgrades v2 (Vulkan 1.4)

Ordered by estimated impact for the current codebase state. Scores: 1 (low) → 10 (very high).

## 1) ✅ Adaptive Step + Continuous LOD + Preintegrated Segment (Score: 10) [COMPLETED - Sep 13, 2025]
- **Benefit**: Larger steps with fewer artifacts; reduces banding and sample count.
- **Status**: ✅ **FULLY IMPLEMENTED AND ENHANCED**
  - ✅ Optical-depth-driven adaptive stepping: `tauTarget / max(sigma_t * dens, 1e-4)`
  - ✅ Enhanced continuous LOD with dual-level blending for smoother transitions
  - ✅ Preintegrated optical depth LUT (256 entries) already implemented
  - ✅ Runtime voxel size control (NUM9) for dynamic detail adjustment
- **Implementation**:
  - `shaders/volume.frag`: Advanced adaptive stepping with optical depth targeting
  - `sampleDensityContinuousLOD()`: Dual-level sampling with smooth blending between LOD levels
  - `VolumeRenderer::setVolumeDetailParameters()`: Runtime grid recreation for voxel size changes
- **Performance**: Maintains high FPS with significantly reduced voxel blockiness
- **Code touchpoints**: `shaders/volume.frag` ray loop; add preintegrated segment helper; smooth LOD mapping.
- **Spec / MCP**:
  - search_vulkan_spec("3D image mipmap") → Sections 12.4, 12.6
  - search_vulkan_spec("vkCmdPipelineBarrier2") → Section 7.6

## 2) Min/Max (Occupancy) Hierarchy for Empty-Space Skipping (Score: 9)
- **Benefit**: Skip large transparent blocks early; accelerates worst cases.
- **Approach**: Build RG16F mip where R=min, G=max per cell during downsample. If `max<threshold`, advance `t` by the block length and increase `lod`.
- **Code touchpoints**: Downsample compute or blit path; `volume.frag` skip logic.
- **Spec / MCP**: Same as above (images, barriers).

## 3) Async Compute Overlap with Graphics via Timeline Semaphores (Score: 8)
- **Benefit**: Hide splat + mip generation latency behind previous frame rendering.
- **Approach**: Run density splat + mip on compute queue; render previous frame’s volume on graphics; synchronize with timeline semaphores using Submit2.
- **Code touchpoints**: Frame graph/`Application.cpp` submissions; `vkQueueSubmit2` with `VkSemaphoreSubmitInfo`.
- **Spec / MCP**:
  - search_vulkan_spec("timeline semaphore") → Sections 6.5, 7.4
  - get_function_spec("vkQueueSubmit2")

## 4) ✅ Blue‑Noise Jitter Sequence + TAA Budget Reduction (Score: 8) [COMPLETED - Sep 12, 2025]
- **Benefit**: Lets you reduce steps while avoiding structured banding; stabilizes with history.
- **Status**: ✅ **FULLY IMPLEMENTED**
  - ✅ STBN (Spatiotemporal Blue Noise) 64-layer texture array integration
  - ✅ TAA (Temporal Anti-Aliasing) foundation with 3x3 neighborhood clamping
  - ✅ Reprojection matrices for camera movement compensation
  - ✅ RGB16F history buffer with volumetric-optimized blend factor
- **Implementation**: 
  - `VolumeRenderer::createSTBNTexture()` loads 64-layer STBN texture array
  - `VolumeRenderer::createTAAPipeline()` complete TAA graphics pipeline
  - `shaders/volume.frag` STBN sampling with temporal consistency
  - `shaders/taa.frag` temporal accumulation with ghosting prevention
- **Performance**: Maintains 200+ FPS with enhanced noise reduction
- **Code touchpoints**: `src/systems/VolumeRenderer.cpp` (STBN + TAA), `shaders/volume.frag` (STBN sampling), `shaders/taa.frag` (TAA implementation).
- **Spec / MCP**: N/A (algorithmic).

## 5) FP16 Density Path With Atomic Scatter Compatibility (Score: 7)
- **Benefit**: Halves bandwidth where atomics not required.
- **Approach**: Use dual path: R16_SFLOAT for gather path; R32_SFLOAT for atomic scatter path. Choose per feature flags.
- **Code touchpoints**: `createDensityGrid()` format selection.
- **Spec / MCP**: search_vulkan_spec("Additional Image Capabilities") → Section 53.1

## 6) Fragment Shading Rate in Periphery (Score: 6)
- **Benefit**: Fewer fragment invocations off‑axis with minimal quality loss.
- **Approach**: Apply `VK_KHR_fragment_shading_rate` radial falloff mask.
- **Code touchpoints**: Pipeline state and per‑draw settings.
- **Spec / MCP**: search_vulkan_spec("VK_KHR_fragment_shading_rate"), Section 29.6

## 7) Epipolar/Light‑Aligned Sampling for Directional Lights (Score: 6)
- **Benefit**: Efficient shadowed single‑scattering with fewer samples.
- **Approach**: Reparameterize sampling along epipolar lines for the dominant light.
- **Code touchpoints**: Light pass compute; volume integration.
- **Spec / MCP**: N/A; uses core compute.

## 8) VMA + Sparse Residency (Large Grids) (Score: 4)
- **Benefit**: Memory savings and faster binds when scaling volume size.
- **Approach**: Use VMA for allocation; consider sparse 3D images for streaming.
- **Spec / MCP**: search_vulkan_spec("Images"), sparse residency sections.

## 9) Descriptor Buffer / Indexing (Scale‑out) (Score: 3)
- **Benefit**: Lower CPU cost if many volumes or per‑view sets.
- **Approach**: `VK_EXT_descriptor_buffer` or descriptor indexing.
- **Spec / MCP**: search_vulkan_spec("descriptor indexing"), Section 14.2 / 14.4

---

Implementation notes observed in code
- Mip‑chain via vkCmdBlitImage and Synchronization2 is present and solid.
- Subgroup early exit is correctly integrated; keep threshold and warmup iters configurable.
- Consider moving blit/downsample to compute for RG min/max hierarchy.

MCP quick queries
- search_vulkan_spec("3D image mipmap")
- search_vulkan_spec("vkCmdPipelineBarrier2")
- search_vulkan_spec("timeline semaphore")
- search_vulkan_spec("VK_KHR_fragment_shading_rate")
- search_vulkan_spec("descriptor indexing")
