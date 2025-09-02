## Accretion Disk Simulation Engine – Concepts, Architecture, and Roadmap

Date: 2025-08-31

### Goal
Design a GPU‑first physics/graphics engine that produces convincing visuals of a black hole accretion disk with optional jets and winds, suitable for real‑time exploration and high‑quality capture. Leverage Vulkan 1.4 compute + dynamic rendering, maintain graceful fallbacks for 1.2/1.3 GPUs.

### Visual Targets (reference the `screenshots/` history for style progression)
- Dense, multi‑ring disk with thermal color grading (hot inner yellow/white → cooler red/orange outer).
- Observable shear and turbulence (streaking, clumping, Kelvin–Helmholtz swirls).
- Central dark region (event horizon silhouette) and inner gap around ISCO.
- Optional polar jets (collimated, blue‑shifted core with fading halo) and dusty halo/winds.
- Subtle relativistic effects: Doppler boosting, gravitational redshift, lensing curvature.

### High‑Level Architecture
- Simulation core (GPU compute):
  - Particle layer (SPH‑light) for kinematics/turbulence and mass transport.
  - Voxel layer (3D grid in cylindrical coordinates) for density/temperature, fed by particle splats.
  - Optional signed‑distance or heightfield for thin‑disk acceleration and boundary constraints.
- Rendering core:
  - Volume ray marcher for emissive density/temperature with absorption and fog.
  - Particle draw path (point sprites or splatted impostors) for highlights, sparks, and rings.
  - Post effects: bloom, tone mapping, optional motion blur; star‑field skybox/background.
- Orchestration:
  - Frame graph with distinct compute phases → barriers → rendering passes (use Sync2).
  - Parameter system with presets; interactive controls (already present in `Application`).

### Physics Model (pragmatic)
- Gravity:
  - Start: Newtonian with softened singularity and a sink inside ISCO.
  - Upgrade: Pseudo‑Newtonian (Paczynski–Wiita potential) to mimic inner‑disk dynamics.
- Angular momentum transport:
  - Shakura–Sunyaev α‑viscosity approximation; implement as viscous force term in compute.
- Turbulence:
  - Vorticity confinement + curl‑noise injection; energy injection controls already exist.
- Disk thermodynamics:
  - Evolve temperature via simple advection + viscous heating + radiative cooling term.
- Jets and winds (optional features):
  - Prescribed axis‑aligned velocity field near poles; density falloff + blue emission bias.
- Relativistic visuals (approximate):
  - Doppler boosting factor ~ (1 − v·n/c)^{-k} in shader.
  - Gravitational redshift via radial color shift lookup.
  - Screen‑space lensing: ray deflection around a Schwarzschild mass using analytic approximation.

### Numerical Scheme (GPU‑oriented)
- Particles (SPH‑light):
  - Neighbor search via spatial hash on GPU (already present: `spatial_hash.comp`).
  - Forces: gravity + viscous drag (α term) + pressure‑like cohesion for ring stability.
  - Stable timestep with CFL‑like limiter; clamp by max acceleration.
- Grid coupling:
  - Density/temperature voxelization (splat) → `density_splat.comp` to 3D texture.
  - Optional divergence damping to avoid blow‑ups in density.
- Boundaries:
  - Absorbing inner radius (sink); reflective/advective outer ring; thin‑disk height cap.

### Rendering Model
- Volume ray marching (existing `VolumeRenderer` path):
  - Emissive color = f(temperature) using a calibrated LUT (blue‑white for hottest, red/orange cooler).
  - Absorption with density‑scaled opacity; exponential transmittance.
  - Jet cone rendered as separate emissive volume with anisotropic phase boost.
- Particle highlights:
  - Draw streaked impostors with view‑aligned quads or per‑vertex size from velocity.
- Post pipeline:
  - Bloom with high threshold; ACES‑like tone map; optional camera grain.
- Relativistic approximations in fragment shader:
  - Doppler gain and hue shift using local velocity and view direction.
  - Gravitational shift based on radial position.

### Data Layout and Performance
- Use structure‑of‑arrays for particle attributes (pos, vel, temp, mass) to improve cache.
- Subgroup operations for neighbor reductions; tile hash buckets per workgroup.
- Keep all simulation GPU‑resident; CPU only updates params.
- Synchronization2 across phases; consider timeline semaphore for frame pacing.
- Descriptor strategy:
  - Push descriptors for hot paths (already used); gate by `VK_KHR_push_descriptor`.
  - For portability, keep a fallback descriptor set path.

### Module Mapping to Current Code
- `ParticleSystem`:
  - Add α‑viscosity, vorticity confinement, and pseudo‑Newtonian gravity kernels.
  - Expose presets: Thin Disk, Turbulent Disk, Jet + Disk.
- `VolumeRenderer`:
  - Add temperature field and LUT; integrate Doppler/redshift terms in ray marcher.
  - Optional lensing pass (pre‑warp rays) before march.
- `Application`:
  - Preset loader and time scaling already exist—extend with scenario presets and capture mode.

### Parameters and Presets
- Global: black hole mass M, ISCO radius, α‑viscosity, accretion rate Ṁ, jet strength, disk height.
- Visual: temperature→color LUT, bloom threshold, jet color bias, dust absorption.
- Presets:
  - Cinematic Thin Disk (fast, clean rings)
  - Turbulent Accretion (heavy streaking, clumps)
  - Jet Dominant (bright polar beams, sparse disk)

### Roadmap (8–10 weeks)
- Week 1–2: Core cleanup and Sync2; particle/grid coupling; density/temperature fields.
- Week 3–4: α‑viscosity + vorticity confinement; stable timestep; presets; capture tools.
- Week 5–6: Volume LUT + Doppler/redshift; improved bloom; star‑field backdrop.
- Week 7: Jet cone module; optional winds; parameter UIs.
- Week 8–9: Lensing approximation; performance tuning (subgroups, tiling, LOD).
- Week 10: Polishing, documentation, sample scenes, automated captures.

### Risks & Mitigations
- Numerical instability at inner radius → sink boundary + timestep limiter.
- Overdraw in ray marching → early‑exit, depth‑aware marching, coarse occupancy grid.
- Performance on 1.2 GPUs → fallback paths (no lensing, simpler Doppler), fewer samples.

### Using the Vulkan MCP While Implementing
- Sync2: search “vkCmdPipelineBarrier2”, “VkImageMemoryBarrier2”, “vkQueueSubmit2”.
- Push descriptors: “VK_KHR_push_descriptor”, “vkCmdPushDescriptorSet”.
- Dynamic rendering attachments: “VkRenderingInfo”, “VkRenderingAttachmentInfo”.

### Screenshot Review Suggestion
Sort `screenshots/` by date and annotate changes in ring sharpness, color grading, and turbulence density over time; keep these before/after pairs as quality gates for each roadmap milestone.

No code changes accompany this document.









