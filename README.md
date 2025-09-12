# PlasmaVulkan

Volumetric particle rendering and fluid dynamics experiments on Vulkan 1.4, written in modern C++20. Real‑time GPU particle simulation (compute), volumetric ray marching with dynamic rendering, mip‑mapped 3D density, and extensive real‑time controls for iteration and capture.

## ✨ Engine Features
- Volumetric renderer (ray marching)
  - 3D density grid from particles (compute splat)
  - Mip‑chain generation for cone‑stepped sampling (vkCmdBlitImage, Synchronization2)
  - Beer–Lambert absorption with emission mapping (temperature‑based palette)
  - Henyey–Greenstein single scattering (forward‑scattering glow)
  - Subgroup‑coherent early exit (fragment): wave‑wide termination when opaque
  - Dynamic rendering (no legacy render passes)
- Particle simulation
  - GPU compute simulation and rendering; optional SPH mode with live parameter controls
  - Constraint presets: sphere, disc, torus, accretion disk
  - Dual‑galaxy/black‑hole scenarios; relativistic jet toggle
- Post‑processing and capture
  - HDR intermediate buffer (R16G16B16A16) ready for tone mapping/bloom
  - Recording system: numbered sessions, PNG sequence dumping, ffmpeg assembly helper
  - GPU timestamp queries for density and raymarch times
- Quality modes and live tuning
  - Standard / High / Ultra volumetric modes
  - Runtime controls for density/opacity/step/emission/max‑steps and color palette shape

## 🔧 Technology
- Vulkan 1.4 core
  - Dynamic rendering, Synchronization2 (vkQueueSubmit2 / vkCmdPipelineBarrier2)
  - Push descriptors for bindless‑style simplicity in critical paths
  - Optional: VK_EXT_shader_atomic_float, VK_EXT_mesh_shader (queried at runtime)
- GLFW (window/input), GLM (math), volk (function loading)
- CMake + vcpkg (Windows toolchain), Visual Studio 2022

## 📦 Build and Run
- Recommended (Windows): Visual Studio 2022 + vcpkg
- Quick commands (from repo root):
```bash
cd cmake-build-debug-visual-studio
cmake --build . --target PlasmaVulkan && ./Debug/PlasmaVulkan.exe
```
- Shader targets are precompiled as SPIR‑V; volume.frag targets SPIR‑V 1.3 for subgroup ops.

## 🎮 Controls
See `KEY_BINDINGS.md` for full mapping. Highlights:
- Toggle volumetrics: V (Standard) / Shift+V (High) / Ctrl+V (Ultra + recording)
- Volumetric tuning: NUM1..NUM8 for density/opacity/step/emission/max‑steps/temp offset/range/saturation
- Physics basics: G/T/D (gravity/turbulence/damping), P (particle count), R (reset)
- Camera: mouse orbit/pan/zoom; arrow keys/page up/down for gravity center
- Recording: F (toggle), Shift+F (HQ), L (loop)

## 🧠 Current State (Sept 2025)
- Stable real‑time volumetric at 1080p; 100k–1M particles depending on settings
- Implemented
  - 3D density mip‑chain with cone‑stepped sampling
  - Synchronization2 across compute/mip/graphics
  - Subgroup early exit in fragment
  - HG single scattering (forward)
  - Optical‑depth adaptive stepping (in progress and tuning)
  - HDR intermediate buffer; bloom pipeline scaffolding in place
- In progress / Next
  - Banding polish: preintegrated segment LUT + blue‑noise jitter + lightweight TAA
  - RG min/max mip hierarchy for robust empty‑space skipping and better LOD/step guidance
  - Tone mapping + energy‑conserving bloom combine
  - Optional ray‑query shadows for scattering occlusion

## 🗺️ Repository Guide
- `src/core/Application.cpp` — main loop, dynamic rendering, recording, GPU timing
- `src/renderer/VulkanContext.cpp` — instance/device/swapchain, feature queries
- `src/systems/VolumeRenderer.*` — density grid, splat compute, mip generation, ray march
- `shaders/` — volume and particle shaders (SPIR‑V); volume.frag enables subgroup ballot/vote
- `VOLUME_*_UPGRADES*.md` — curated performance and image quality roadmaps (v1/v2)
- `VOLUME_BANDING_DIAGNOSIS_AND_TUNING.md` — banding diagnosis, adaptive stepping pseudocode
- `QUICK_START.md` — quick build/run steps

## 🚧 Notes
- VMA integration is currently disabled pending allocator init investigation (see comments in `Application.cpp`). The engine uses straightforward Vulkan allocations as a temporary measure.
- Mesh shader particle renderer is probed at runtime and used if supported.

## 📸 Recording Tips
- Use High or Ultra modes for capture; enable volumetrics and bloom.
- For long loops, enable loop mode (L) and fixed timestep recording.

## 🏁 Goals
- High‑fidelity volumetric plasma at 60–120 FPS
- Robust empty‑space skipping + preintegrated segment shading
- Cinematic post pipeline (tone map + bloom)
- Optional ray‑query lighting for premium captures
