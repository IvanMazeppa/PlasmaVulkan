# First‑Frame Crash Diagnosis — 2025-09-12

This note captures why the app exits on the first frame and how to fix it safely.

## Symptom
- Exits/crashes on the first rendered frame (after init messages). Console shows first frame start.

## Likely root cause
- The 3D density image starts in `VK_IMAGE_LAYOUT_UNDEFINED` and is sampled by the fragment shader before it’s ever transitioned/cleared.
- `updateDensityGrid()` performs the first transitions/clear, but it only runs when `m_shouldUpdatePhysics` is true. On frame 1 it’s commonly false, so the volume is sampled uninitialized.

## Evidence in code
- Density image is created with UNDEFINED layout and no immediate clear/transition:
```cpp
// VolumeRenderer::createDensityGrid()
imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
// ... no one‑time clear/transition across all mips here
```
- Per‑frame init/clear is gated by physics:
```cpp
// Application::recordCommandBuffer()
if (m_particleSystem && m_shouldUpdatePhysics) {
    m_volumeRenderer->updateDensityGrid(...);
    m_volumeRenderer->generateMipChain(...);
}
```
- Volume is rendered (and sampled) regardless:
```cpp
if (m_volumetricMode && m_volumeRenderer) {
    m_volumeRenderer->render(...);
}
```
- Fragment shader samples the density texture on first frame:
```glsl
float sampleDensity(vec3 worldPos) { return sampleDensityLOD(worldPos, 0.0); }
```

## Secondary risk
- 3D mip generation via `vkCmdBlitImage` using `VK_FILTER_LINEAR` on `VK_FORMAT_R32_SFLOAT` may be unsupported on some GPUs. If the crash aligns with mip generation, switch to NEAREST blit or compute downsample.

## Fix checklist (safe, minimal changes)
1) One‑time density image initialization (before first render)
   - Record a one‑time command buffer after `createDensityGrid()` that:
     - Transitions the WHOLE subresource range (all mips, all layers) from UNDEFINED → TRANSFER_DST_OPTIMAL
     - Clears ALL mips to 0 with `vkCmdClearColorImage` (`levelCount = m_densityMipLevels`)
     - Transitions the WHOLE range → SHADER_READ_ONLY_OPTIMAL
   - This guarantees safe sampling even if `updateDensityGrid()` hasn’t run yet.

2) Ensure first‑frame update runs at least once
   - Call `updateDensityGrid()` + `generateMipChain()` unconditionally once before the first `render()` (or keep an `initialized` flag inside `VolumeRenderer` and perform a cheap ensure‑initialized path).
   - Alternatively, decouple volume update from `m_shouldUpdatePhysics` (physics can be gated, volume init should not).

3) Mip generation filter safety
   - If you see validation errors or crash during `generateMipChain()`, use `VK_FILTER_NEAREST` for 3D blits or replace blits with a compute downsample (robust, preferred).

4) Descriptor vs actual image layout
   - Setting `imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL` in descriptors does not transition the image. Ensure the barriers above executed before sampling.

5) Optional belt‑and‑braces while wiring LOD
   - Temporarily force `lod = 0` sampling until you confirm mips were built on the first frame.

## Order of operations (frame 0)
1) Create density image (all mips).
2) One‑time init CB: UNDEFINED → TRANSFER_DST → clear all mips → SHADER_READ_ONLY.
3) Optionally run `updateDensityGrid()` + `generateMipChain()` once.
4) Render: sample density in fragment safely.

## Why this works
- Sampling an UNDEFINED image is undefined behavior and can crash immediately. The one‑time initialization ensures a valid, zero state for all mips so the first render is safe even if physics hasn’t stepped yet. Running the proper per‑frame update afterward preserves your current pipeline design.
