# Volumetric Rendering - Image Quality Upgrades v2 (Vulkan 1.4)

Ordered by expected visual impact for your current renderer. Scores: 1 (low) → 10 (very high).

## 1) Banding Reduction: Preintegrated Segment + Blue‑Noise/TAA (Score: 10)
- **Why**: Remaining banding stems from coarse per‑step integration and structured jitter.
- **Approach**:
  - Use preintegrated absorption/emission per step: `radiance += T * f_preint(dens, step)`, `T *= e^{-sigma_t*dens*step}`. Implement a 1D LUT over optical depth or a closed form for constant density.
  - Replace hash jitter with tiled blue‑noise; rotate per‑frame (Cranley‑Patterson). Accumulate with TAA history (clamp and neighborhood clipping).
- **Touches**: `volume.frag` main loop; postprocess reprojection.
- **Spec**: N/A; see barriers (vkCmdPipelineBarrier2) for history image transitions.

## 2) ✅ Physically‑Based Single Scattering (Henyey–Greenstein) (Score: 9) [COMPLETED - Sep 12, 2025]
- **Why**: Adds directional glow and depth; plasma reads more volumetric.
- **Status**: ✅ **FULLY IMPLEMENTED**
  - ✅ Henyey-Greenstein phase function with g=0.75 for forward scattering
  - ✅ Directional light from upper-left-front with warm white color
  - ✅ Single scattering contribution integrated into ray marching loop
  - ✅ Maintained edge enhancement for fine feature definition
- **Implementation**:
  - `shaders/volume.frag`: Added henyeyGreenstein() function
  - Integrated scattering: `scatteredLight = lightColor * phase * strength * density`
  - Light direction: vec3(-0.5, -0.8, -0.6) for cinematic angle
- **Performance**: 77-86 FPS (slight cost for dramatic visual improvement)
- **Touches**: `volume.frag` lighting section with HG phase function integration.
- **Spec**: N/A; sampling and barriers as implemented.

## 3) Multiple Scattering Approximation (Score: 8)
- **Why**: Softer glow and thicker energy accumulation.
- **Approach**: Add inexpensive approximation (e.g., forward‑scattering boost or screen‑space diffuse) or iterative volumetric blur in 3D at coarse mip levels accumulated back.
- **Touches**: Separate compute pass on mip pyramid; composite back in raymarch.
- **Spec**: N/A.

## 4) Min/Max Hierarchy‑Aware Shading (Score: 8)
- **Why**: Guides LOD and lighting cost to where it matters and prevents over‑integration in void regions.
- **Approach**: Use RG min/max mip: if `max<threshold` skip; else descend LOD and compute normals from appropriate level; modulate light steps.
- **Touches**: Downsample compute; `volume.frag` LOD/skip logic.
- **Spec**: search_vulkan_spec("3D image mipmap"), barriers.

## 5) Tone Mapping, HDR Output, and Bloom Refinement (Score: 7)
- **Why**: Better highlight rolloff and cinematic glow.
- **Approach**: Ensure HDR render target; apply ACES/filmic tone map; route volume output to bright‑pass and perform dual‑filter bloom with energy‑conserving combine.
- **Touches**: Post pipeline; `Application.cpp` HDR swapchain (R16G16B16A16_SFLOAT present).
- **Spec**: search_vulkan_spec("Blending") → Section 31.1.

## 6) Gradient Quality: Precomputed Normals or Structure Tensor (Score: 6)
- **Why**: Current gradient from density costs fetches and can be noisy.
- **Approach**: Compute normals (or gradient magnitude) in compute at LOD 1–2 and store in RGBA16F volume; sample normals directly during raymarch.
- **Touches**: Splat/downsample compute; shader sampling.
- **Spec**: Images/mips; barriers.

## 7) Ray Tracing Integration Options (Score: 6)
- **A) Ray‑Query Shadows**
  - Use `VK_KHR_ray_query` to cheaply test occlusion from small numbers of analytic lights for the scattering term.
  - Spec / MCP: search_vulkan_spec("VK_KHR_ray_query") → Features struct and requirements.
- **B) Full Ray Tracing for Light Transport (Selective)**
  - For recording mode, add a ray‑gen shader to sample volumetric media in path segments (e.g., primary or secondary rays) using `VK_KHR_ray_tracing_pipeline`.
  - Spec / MCP: search_vulkan_spec("VK_KHR_ray_tracing_pipeline") → Sections 10.5, limits in 51.1.

## 8) Epipolar / Light‑Aligned Raymarch (Score: 5)
- **Why**: Better quality for directional lighting with fewer samples.
- **Approach**: March along light epipolar lines, reusing integrals across pixels; combine with your cone‑LOD scheme.
- **Touches**: New light‑space pass; gather in raymarch.

## 9) Cubic Filtering for Smooth Mip Sampling (Score: 4)
- **Why**: Smoother gradients when sampling lower mips.
- **Approach**: If supported for 3D images, enable `VK_EXT_filter_cubic` for the density sampler (note 3D support constraints per spec).
- **Spec / MCP**: search_vulkan_spec("VK_EXT_filter_cubic") → VUIDs and usage notes.

## 10) Artistic Controls and Color Science (Score: 4)
- **Why**: Consistent orange‑red palette without chalky whites.
- **Approach**: Keep the temperature controls; add LUT‑based color grading in post; expose contrast and shoulder parameters.

---

Tuning checklist for current banding
- Lower base step when gradient magnitude is high; raise when low.
- Use continuous LOD tied to current step; avoid large discrete jumps.
- Prefer blue‑noise jitter + TAA over white‑noise hash.
- Clamp history with neighborhood clipping to avoid ghosting.

MCP quick references
- search_vulkan_spec("3D image mipmap")
- search_vulkan_spec("vkCmdPipelineBarrier2")
- search_vulkan_spec("VK_KHR_ray_query")
- search_vulkan_spec("VK_KHR_ray_tracing_pipeline")
- search_vulkan_spec("VK_EXT_filter_cubic")
- search_vulkan_spec("Blending")
