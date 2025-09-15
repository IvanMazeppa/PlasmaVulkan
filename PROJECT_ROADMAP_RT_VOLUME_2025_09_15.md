## PlasmaVulkan RT-Centric Volume Roadmap (2025-09-15)

Goal
- High-quality plasma/gas rendering with directional lighting, RT occlusion, TAA accumulation, and offline-quality presets, while retaining a fast preview path.

Principles
- Decouple from legacy renderers: new `RTVolumeRenderer` path owns its resources.
- Predictable performance: cost scales with grid/steps, not particle count.
- Vulkan 1.4 practices: dynamic rendering, Sync2, timeline semaphores, push descriptors where helpful, VMA later.

Milestones
1) Foundation (RTV-2001)
   - Add `RTVolumeRenderer` subsystem and new mode flag.
   - HDR target (R16G16B16A16_SFLOAT), composite pass.
   - Robust single-time cmds using SubmitInfo2; all image layouts validated.
   - Acceptance: mode toggles on/off without affecting mesh/point paths; clean logs.

2) Density pipeline (RTV-2002)
   - 3D density image (R16F), dimensions 160–256 cubed, mip chain.
   - Compute splat from particle buffer; start with atomic add, then move to tile/binning.
   - Occupancy bitmask per 8³ bricks for skipping; optional min–max later.
   - Acceptance: visual density debug slice, correct layouts, stable with 1M particles.

3) Ray marching (RTV-2003)
   - Compute marcher → HDR: STBN jitter, cone/adaptive stepping, early-out on opacity.
   - Beer–Lambert with preintegrated optical depth LUT; HG phase single scattering.
   - Acceptance: clean noise structure, stable T vs step count, adjustable quality presets.

4) TAA (RTV-2004)
   - Reprojection with clamping; jitter-only matrix; update prev matrix after TAA.
   - Acceptance: purple/green artifacts eliminated; resize-safe; history reset on mode change.

5) RT shadows (RTV-2005)
   - RayQuery to external TLAS for occluders (mask 0x01). Bias, max distance, overlay.
   - Acceptance: hit overlay behaves; no VUID 07990/08114; GPU-side waits via timeline.

6) Self-shadow and god rays (RTV-2006)
   - In-march transmittance for self-shadow; optional light-space marching for shafts.
   - Acceptance: demonstrable light shafts/god rays; debug toggles.

7) Quality/upscaling (RTV-2007)
   - Half-res volume + bilateral upscale; temporal reuse; optional SVGF.
   - Acceptance: >60 FPS preview at target res; offline mode configurable.

Risk controls
- Empty workgroups: guard final group in all compute/mesh shaders, return on zero work.
- Depth state: disable depth test unless a valid depth attachment is bound.
- Descriptor/pipeline drift: dual pipelines for optional bindings; log pipeline selection.
- Sync: no barriers inside dynamic rendering; use Synchronization2 everywhere.
- Resize: recreate all dependent images and reset TAA/history.

Deliverables
- CRs for each milestone with acceptance checks and logs.
- Companion guides with MCP queries for spec cross-refs.
