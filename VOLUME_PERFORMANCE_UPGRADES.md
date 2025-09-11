# Volumetric Rendering - Performance Upgrades (Vulkan 1.4)

Ordered by estimated impact for this project. Scores are 1 (low) to 10 (very high).

## 1) 3D Density Mip-Chain + Cone-Stepped Raymarch (Score: 10)
- **Benefit**: Large skip of empty/low-detail regions; fewer samples with minimal detail loss.
- **Approach**:
  - Create 3D image with full mip chain; downsample in compute after splatting.
  - In fragment shader, use `textureLod` and increase `lod` with step size/transmittance.
  - Optional: min/max hierarchy (RG16F) to safely skip blocks.
- **Code touchpoints**: `src/systems/VolumeRenderer.cpp` (image creation, mip generation passes, sampler `maxLod`), `shaders/volume.frag` (LOD sampling, cone stepping).
- **Relevant spec / MCP**:
  - Images and mip levels: search_vulkan_spec("3D image mipmap") → Sections 12.4 (Images), 12.6 (Image Views)
  - Pipeline barriers for builds: search_vulkan_spec("vkCmdPipelineBarrier2") → Section 7.6

## 2) ✅ Switch Density Image to FP16 (R16_SFLOAT) (Score: 9) [COMPLETED]
- **Benefit**: Halves bandwidth and memory footprint of volume reads/writes.
- **Approach**: Use `VK_FORMAT_R16_SFLOAT` if format supports STORAGE+SAMPLED; verify via format props.
- **Code touchpoints**: `createDensityGrid()` format; sampler unchanged.
- **Relevant spec / MCP**:
  - Image format properties: search_vulkan_spec("Additional Image Capabilities") → Section 53.1

## 3) Synchronization2 Barriers Across Passes (Score: 8)
- **Benefit**: Clear, modern sync; potential driver optimizations and correctness under reordering.
- **Approach**: Replace legacy `vkCmdPipelineBarrier` with `vkCmdPipelineBarrier2` and `VkImageMemoryBarrier2` for compute→fragment and for per-mip downsample steps.
- **Code touchpoints**: `updateDensityGrid()` and mip builder; graphics read barriers.
- **Relevant spec / MCP**:
  - search_vulkan_spec("vkCmdPipelineBarrier2") → Section 7.6; Section 54.6 list

## 4) Subgroup-Coherent Early Exit (Score: 7)
- **Benefit**: Whole-wave termination during raymarch when fully opaque; reduces iterations.
- **Approach**: Use subgroup vote/ballot to test if all invocations reached transmittance threshold, then break.
- **Code touchpoints**: `shaders/volume.frag` ray loop.
- **Relevant spec / MCP**:
  - search_vulkan_spec("subgroup ballot") → Sections 54.6 (caps), 50.2 (subgroup properties), 9.27 (Group Operations)
  - search_vulkan_spec("shader subgroup") → feature controls and stages

## 5) VMA for 3D Image Allocation (Score: 6)
- **Benefit**: Better memory placement, easier lifetime management, potential aliasing/pooling.
- **Approach**: Replace manual `vkAllocateMemory` with VMA; use dedicated allocation for large 3D.
- **Code touchpoints**: `createDensityGrid()`.
- **Relevant spec / MCP**:
  - N/A (library), but uses Vulkan memory rules; see search_vulkan_spec("Device Memory") → Sections 11.x

## 6) Min/Max Hierarchy for Empty-Space Skipping (Score: 6)
- **Benefit**: Stronger skip guarantees than average-only mipmaps.
- **Approach**: Build RG16F: R=min, G=max per-cell during downsample; skip when G<threshold.
- **Code touchpoints**: Downsample compute shader, sampler; `volume.frag` skip logic.
- **Relevant spec / MCP**:
  - Same as mip chain + barriers entries above

## 7) Fragment Shading Rate in Periphery (Score: 5)
- **Benefit**: Reduce fragment invocations in low-importance regions.
- **Approach**: Use `VK_KHR_fragment_shading_rate` to lower shading rate outside ROI.
- **Code touchpoints**: Pipeline state for FSR, dynamic or per-draw rate setting.
- **Relevant spec / MCP**:
  - search_vulkan_spec("VK_KHR_fragment_shading_rate") and "Fragment Shading Rates" → Section 29.6

## 8) Compute Workgroup/Occupancy Tuning (Score: 4)
- **Benefit**: Better utilization during splat and downsample.
- **Approach**: Match local sizes to hardware subgroup size; use `computeFullSubgroups` where helpful.
- **Code touchpoints**: `shaders/density_splat*.comp` and downsample shader.
- **Relevant spec / MCP**:
  - search_vulkan_spec("shader subgroup") → subgroup size control

## 9) Descriptor Buffer (Optional) (Score: 3)
- **Benefit**: Lower CPU overhead for descriptor updates if scaling to many volumes.
- **Approach**: Use `VK_EXT_descriptor_buffer` for bindless-like descriptor storage.
- **Code touchpoints**: Descriptor setup.
- **Relevant spec / MCP**:
  - search_vulkan_spec("descriptor indexing"), "descriptor buffers" → Sections 14.2, 14.4

## Implementation note
- Per-particle atomic scatter via `VK_EXT_shader_atomic_float` is already implemented and feature-gated (`m_useAtomicScatter` in `VolumeRenderer`). Action: verify device feature enabling for `VK_EXT_shader_atomic_float` and test both scatter and gather paths.
- Relevant spec / MCP: search_vulkan_spec("VK_EXT_shader_atomic_float"), "Feature Requirements" → Sections 49.6, 50.1

---

MCP quick queries to reproduce:
- search_vulkan_spec("3D image mipmap")
- search_vulkan_spec("vkCmdPipelineBarrier2")
- search_vulkan_spec("subgroup ballot")
- search_vulkan_spec("shader subgroup")
- search_vulkan_spec("VK_KHR_fragment_shading_rate")
- search_vulkan_spec("descriptor indexing")
