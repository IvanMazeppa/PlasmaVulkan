# Volumetric Banding Diagnosis and Tuning

This document captures an assessment of current artifacts (banding and close-up instability), likely causes, and concrete, testable fixes. It includes pseudocode for optical-depth adaptive marching and a simple debug overlay plan to verify behavior visually.

## Observed symptoms
- Banding in high-density regions and near-camera close-ups
- Frame rate collapse when close to the volume; recovery when zoomed out
- Improved visuals with HG phase, but more sensitivity to step/LOD misconfiguration

## Likely root causes
1) LOD not actually used in the main march
- The loop samples `sampleDensity(pos)` which reads LOD 0. Continuous LOD logic is not yet wired into per-step sampling.

2) Step size tied to noisy gradient or fixed heuristics
- Near surfaces gradients spike → overly small steps; far from surfaces still small due to conservative thresholds.

3) Full-cost shading every step
- Normals computed every iteration (6 fetches), even when density is negligible; light term applied even when local transmittance ≈ 1.

4) Discrete mip jumps and no bias
- `lod = log2(step/voxel)` collapses to 0 when step shrinks near camera; discrete changes cause banding/aliasing.

## Prioritized fixes (try in order)
1) Wire continuous LOD into the march
- Replace density fetch with `sampleDensityLOD(pos, lod)`, where `lod` is continuous and clamped.

2) Switch to optical-depth adaptive step (stable)
- Choose a target optical depth per step, τ_target ∈ [0.05, 0.1]. Compute step from density:
  - `step = clamp(τ_target / max(sigma_t * dens, eps), step_min, step_max)`
- Stabilizes iteration count in bright cores and avoids tiny steps near camera.

3) Gate normals and shading
- Compute gradients only if `dens > densNormalThreshold` and every N steps.
- Skip lighting accumulation when `local_transmittance > 0.995`.

4) Continuous LOD with bias and smoothing
- `lod = clamp(log2(step/voxelSize) + lodBias, 0, maxMip-1)` with lodBias ≈ 0.2–0.6.
- Optionally low-pass filter `lod` over a few iterations to avoid flicker.

5) Add small per-step jitter and blue-noise start jitter
- Start: blue-noise t0 offset rotated per frame.
- Per-step: ±10–20% of current step to break resonance.

6) Early-exit threshold tuning
- With HG, increase exit threshold slightly (e.g., `transmittance <= 0.02`) to stop earlier in hot cores.

## Optical-depth adaptive marching (pseudocode)

```c
// Inputs: ro, rd, t0, t1, voxelSize, maxMip, sigma_t, tauTarget, stepMin, stepMax, lodBias
vec3 radiance = 0;
float T = 1.0;
float t = t0 + blueNoiseStartJitter();

float prevLod = 0.0;
int  shadeStride = 0;

for (uint i = 0; i < maxSteps && t < t1 && T > 0.02; ++i) {
    vec3 pos = ro + rd * t;

    // Estimate density at a LOD based on last step (warm start)
    float tentativeStep = max(stepMin, min(stepMax, lastStep > 0 ? lastStep : stepMin));
    float lod = clamp(log2(tentativeStep / voxelSize) + lodBias, 0.0, float(maxMip - 1));
    lod = mix(prevLod, lod, 0.7); // smooth lod

    float dens = sampleDensityLOD(pos, lod);

    // Optical-depth-driven step
    float step = clamp(tauTarget / max(sigma_t * dens, 1e-4), stepMin, stepMax);

    // Small stochastic dither to break resonance
    step *= (0.9 + 0.2 * blueNoiseStepJitter(i));

    // Local transmittance for this segment
    float tau = sigma_t * dens * step;
    float Lt = exp(-tau);

    // Shade sparsely: only when meaningful
    bool doShade = (dens > densNormalThreshold) && (shadeStride % SHADE_EVERY_N == 0 || Lt < 0.995);
    if (doShade) {
        vec3 N = estimateNormal(pos, lodForNormals); // cheaper LOD for normals
        vec3 emission = emissionFromDensity(dens, N, rd);
        // Preintegrated segment radiance (constant density approx)
        vec3 seg = emission * dens * (1.0 - Lt) / max(sigma_t * dens, 1e-4);
        radiance += T * seg;
    }

    // Update transmittance and advance
    T *= Lt;
    t += step;

    shadeStride++;
    prevLod = lod;
    lastStep = step;

    // Optional subgroup early exit
    if (i > 8 && subgroupAll(T <= 0.02)) break;
}

return vec4(radiance, 1.0 - T);
```

## Debug overlay plan
1) LOD heatmap
- In `volume.frag`, optionally output a color by `lod` instead of radiance/alpha to verify cone-stepping engages. Example mapping: cool colors for low lod, warm for high.

2) Step length visualization
- Output `step / stepMax` as grayscale to see collapses in dense regions.

3) Gradient magnitude view
- Temporarily output `length(gradient)` to assess noise and thresholds.

4) Transmittance falloff
- Output `1 - T` to ensure early-exit engages where expected.

5) Blue-noise check
- Render only the start `t` offset pattern to confirm blue-noise tiling and frame rotation.

## Parameter starting points
- tauTarget: 0.08 (try 0.05–0.12)
- lodBias: 0.3 (try 0.2–0.6)
- stepMin: 0.5 · voxelSize
- stepMax: 8–12 · voxelSize
- densNormalThreshold: 0.002–0.01
- SHADE_EVERY_N: 2–3
- early-exit T threshold: 0.02

## Next steps
- Verify LOD heatmap shows higher mips near camera when step grows; no persistent lod 0 up close.
- If banding persists, add min/max hierarchy for robust skipping and to drive lod/step decisions.
- Integrate blue-noise jitter + TAA reprojection for final stabilization.
