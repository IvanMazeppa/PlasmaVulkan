# Volumetric Rendering - Image Quality Upgrades (Vulkan 1.4)

Ordered by estimated impact. Scores are 1 (low) to 10 (very high).

## 1) ✅ Beer–Lambert Transmittance + Emission (Score: 10) [COMPLETED - WORKING]
- **Benefit**: Physically-plausible opacity and brightness; reduces banding and overbright.
- **Approach**: Accumulate transmittance T and emission per step: `T *= exp(-sigma_t * dens * step)`, `radiance += T * emission * step`.
- **Code touchpoints**: `shaders/volume.frag` main loop.
- **Relevant spec / MCP**:
  - N/A (rendering math), but relies on correct sampling and barriers → search_vulkan_spec("vkCmdPipelineBarrier2")

## 2) Cone-Stepped LOD Sampling for Detail Preservation (Score: 9)
- **Benefit**: Maintains fine detail near surfaces while accelerating in empty regions.
- **Approach**: Use mip chain and `textureLod`; derive `lod` from step size/gradient.
- **Code touchpoints**: `shaders/volume.frag`; 3D mip generation.
- **Relevant spec / MCP**:
  - search_vulkan_spec("3D image mipmap") → Sections 12.4, 12.6

## 3) Anisotropic Single Scattering with HG Phase (Score: 8)
- **Benefit**: More realistic forward-scattering glow for plasma filaments.
- **Approach**: Add Henyey–Greenstein phase function (g≈0.6–0.85) with a directional/core light.
- **Code touchpoints**: `shaders/volume.frag` lighting.
- **Relevant spec / MCP**:
  - N/A (lighting math). Sync remains per Synchronization2.

## 4) Gradient Cost Reduction / Precomputed Normals (Score: 7)
- **Benefit**: Reduce 6 extra texture fetches per step; denoise normals.
- **Approach**: (A) Precompute gradient or gradient magnitude in compute and store extra channel; or (B) sample gradient at higher LOD.
- **Code touchpoints**: `shaders/density_splat*.comp` (A); `shaders/volume.frag` (B).
- **Relevant spec / MCP**:
  - search_vulkan_spec("3D image mipmap"), barriers via search_vulkan_spec("vkCmdPipelineBarrier2")

## 5) Temporal Supersampling (TAA-style) (Score: 7)
- **Benefit**: Higher effective quality at lower per-frame ray steps; less noise.
- **Approach**: Subpixel camera jitter + history reprojection with clamped blending.
- **Code touchpoints**: Frame graph/additional textures; compositing.
- **Relevant spec / MCP**:
  - N/A (algorithmic); ensure proper layout transitions via Synchronization2.

## 6) Tone Mapping and Bloom Integration (Score: 6)
- **Benefit**: Better highlight rolloff; emphasizes hot cores/filaments.
- **Approach**: Feed volumetric color into existing bloom bright pass; apply ACES or filmic tone map.
- **Code touchpoints**: Post-processing pipeline.
- **Relevant spec / MCP**:
  - search_vulkan_spec("Blending") for final compositing → Section 31.1

## 7) ✅ Stochastic Jitter and Blue-Noise Dithering (Score: 5) [COMPLETED - WORKING]
- **Benefit**: Reduces banding/contouring at low step counts.
- **Approach**: Replace hash jitter with blue-noise pattern; vary along time for TAA.
- **Code touchpoints**: `shaders/volume.frag`.
- **Relevant spec / MCP**:
  - N/A; sampling unaffected.

## 8) ⚠️  Color Calibration for Plasma Palette (Score: 4) [PARTIALLY FIXED - NEEDS TUNING]
- **Benefit**: Avoids early white/pink saturation; preserves orange-red range.
- **Approach**: Adjust mapping curve and density scaling (already begun in shader); pair with tone map.
- **Code touchpoints**: `temperatureToColor` in `shaders/volume.frag`.
- **Relevant spec / MCP**:
  - N/A.

---

MCP quick queries to reproduce:
- search_vulkan_spec("3D image mipmap")
- search_vulkan_spec("vkCmdPipelineBarrier2")
- search_vulkan_spec("Blending")
