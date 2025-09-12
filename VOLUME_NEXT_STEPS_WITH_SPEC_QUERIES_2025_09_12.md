# Volumetric Next Steps + Spec Queries — 2025-09-12

Based on the latest image (shimmer in hot cores, high FPS), these are the prioritized next steps with Vulkan spec anchors and ready‑to‑run MCP queries.

## 1) Preintegrated segment + lightweight TAA (highest impact)
- Add a 1D f(τ) LUT: f(τ) = (1−e^{-τ})/τ, sampled per step; stabilize with TAA (α≈0.1) and neighborhood clamp.
- MCP queries:
  - search_vulkan_spec("vkCmdPipelineBarrier2") — ensure correct history image transitions (Section 7.6)
- Adjustments:
  - Start blue‑noise jitter as you do now; fade per‑step micro‑dither in dense cores (τ>0.5).
  - Accumulate alpha separately with a smaller α to avoid ghosting.

## 2) Min/Max RG mip hierarchy (quality + perf)
- Build RG16F mip: R=min, G=max during downsample; use G<threshold for skipping; guide step/LOD and shading cadence.
- MCP queries:
  - search_vulkan_spec("3D image mipmap") — Images & Image Views (Sections 12.4, 12.6)
  - search_vulkan_spec("vkCmdPipelineBarrier2") — per‑mip barriers (Section 7.6)
- Adjustments:
  - Prefer compute downsample for robustness; NEAREST blit if you keep graphics path.

## 3) Tone mapping + bloom refinement
- Switch to ACES/filmic and energy‑conserving bloom combine to avoid chalky whites in hot cores.
- MCP queries:
  - search_vulkan_spec("Blending") — Section 31.1
- Adjustments:
  - Slightly lower bloom strength; let HG carry perceived brightness.

## 4) HG scattering polish + simple occlusion
- Expose g in [0.6, 0.85]; add a single shadow term using ray query for 1–2 analytic lights.
- MCP queries:
  - search_vulkan_spec("VK_KHR_ray_query") — features/requirements (Sections 50.1, 49.6)
- Adjustments:
  - Use coarse LOD for shadow lookups to keep cost low; clamp max shadow distance.

## 5) Fragment shading rate in periphery (optional)
- Reduce fragment invocations away from ROI to free headroom for LUT/TAA.
- MCP queries:
  - search_vulkan_spec("VK_KHR_fragment_shading_rate pipeline fragment shading rate state create info") — Section 29.6 and pipeline dynamic state
- Adjustments:
  - Dynamic per‑draw rate; avoid interfering with dense ROI.

## 6) Cubic filtering for smoother coarse mips (optional)
- If supported for 3D, `VK_EXT_filter_cubic` can smooth coarse LOD sampling.
- MCP queries:
  - search_vulkan_spec("VK_EXT_filter_cubic 3D image support requirements") — VUIDs and constraints
- Adjustments:
  - Verify format features: VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_CUBIC_BIT_EXT for your 3D view.

---

## Ready‑to‑run MCP queries
- search_vulkan_spec("vkCmdPipelineBarrier2")
- search_vulkan_spec("3D image mipmap")
- search_vulkan_spec("Blending")
- search_vulkan_spec("VK_KHR_ray_query")
- search_vulkan_spec("VK_KHR_fragment_shading_rate pipeline fragment shading rate state create info")
- search_vulkan_spec("VK_EXT_filter_cubic 3D image support requirements")

## Practical tuning notes
- Optical‑depth step target τ: 0.06–0.10; clamp step in [0.5·voxel, 10·voxel].
- LOD from step with bias: lod = log2(step/voxel) + 0.3; smooth over time.
- Shade every 2–3 steps; skip when local transmittance > 0.995.
- Fade micro‑jitter in cores (τ>0.5) to reduce shimmer; keep start jitter.

## Validation overlays
- LOD heatmap, step length view, transmittance (1−T), and a blue‑noise debug view.
