# Volumetric Assessment and Next Steps — 2025-09-12

## Snapshot assessment (120 FPS, ~750k particles)
- Strong overall: coherent filaments, convincing HG depth and falloff.
- Residual banding/soft halos on mid-density wisps; bright cores slightly washed when very close.
- Performance headroom looks good; adaptive stepping appears stable after tuning.

## Best next upgrade
Implement preintegrated segment + blue-noise start jitter + lightweight TAA. This combo removes remaining banding, stabilizes close-up detail, and preserves FPS.

---

## Preintegrated segment integration
Goal: replace per-step naive emission/absorption with an analytic segment integral for constant density over the step (or a tiny 1D LUT).

### 1) Analytic form (constant density over step)
Given density d, extinction sigma_t, step length s:
- τ = sigma_t · d · s
- Local transmittance Lt = exp(-τ)
- Segment contribution for an emission term E(d):
  seg = E(d) · d · (1 − Lt) / max(sigma_t · d, ε)
Accumulate: radiance += T · seg; T *= Lt.

This is already close to what you do; the preintegration aspect is to ensure E(d) and τ interact smoothly as s varies, avoiding banding when s changes.

### 2) 1D LUT approach (more robust, cheap)
- Domain: τ ∈ [0, τ_max] (start with τ_max ≈ 2.0), resolution 256.
- Table stores f(τ) = (1 − exp(-τ)) / max(τ, ε).
- Then seg ≈ E(d) · d · s · f(τ).
- Benefits: smooth in τ, numerically stable for small τ; avoids discontinuities when s changes.

LUT build options:
- CPU upload once (tiny buffer) or compute pass that fills a 1D image.
- Sample with linear filtering.

---

## Blue-noise jitter + lightweight TAA

### 1) Blue-noise start jitter
- Use a tiled blue-noise texture (e.g., 128×128) to offset starting t by δt = jitterScale · step.
- Rotate/offset the tile each frame (Cranley–Patterson) to avoid locking.

### 2) Per-step micro dither (optional)
- Multiply step by (0.9 + 0.2 · noise(i)) to break resonance in homogeneous areas.

### 3) Lightweight TAA (history reprojection)
- Reproject previous frame color using current and previous view-proj.
- Blend: C_out = clampMix(C_curr, C_hist, α, neighborhoodClamp)
  - α ≈ 0.08–0.12
  - Neighborhood clamp: clamp C_hist to min/max of a 3×3 window of current color to prevent ghosting.
- Accumulate alpha separately with smaller α to avoid lingering smoke.

Barriers: use Synchronization2 for history image transitions between sample and write.

---

## Recommended parameter starts
- τ_target: 0.06–0.10
- step ∈ [0.5·voxel, 10·voxel]
- lodBias: 0.3 (smooth lod over time)
- blue-noise jitterScale: 0.5–1.0 of current step
- TAA α: 0.1; neighborhood clamp ±10–20% luminance range

---

## Next after this
1) Min/Max RG mip hierarchy for robust skipping and better step/LOD guidance.
2) Tone mapping + bloom refinement (ACES/filmic; energy-conserving combine).
3) Precomputed normals at low LOD; shade every 2–3 steps only when needed.
4) Ray-query shadows for HG scattering.

---

## Pseudocode (concise)

```c
// Inputs: ro, rd, t0, t1, sigma_t, voxel, maxMip, tauTarget
vec3 radiance = 0; float T = 1; float t = t0 + blueNoiseStart();
float lastStep = 0, prevLod = 0; int shadeStride = 0;
for (uint i=0; i<maxSteps && t<t1 && T>0.02; ++i) {
  float step = clamp(lastStep>0?lastStep:0.5*voxel, 0.5*voxel, 10*voxel);
  float lod = clamp(log2(step/voxel) + 0.3, 0, maxMip-1);
  lod = mix(prevLod, lod, 0.7);
  vec3 pos = ro + rd * t;
  float d = sampleDensityLOD(pos, lod);
  step = clamp(tauTarget / max(sigma_t*d, 1e-4), 0.5*voxel, 10*voxel);
  step *= (0.9 + 0.2*blueNoiseStep(i));
  float tau = sigma_t * d * step;
  float Lt = exp(-tau);
  bool doShade = (d>0.003) && (shadeStride%2==0 || Lt<0.995);
  if (doShade) {
    vec3 N = estimateNormalCoarse(pos); // cheap LOD
    vec3 E = emissionFromDensity(d, N);
    float f = lut_f_tau(tau); // (1-exp(-tau))/tau
    radiance += T * (E * d * step * f);
  }
  T *= Lt; t += step; shadeStride++; prevLod = lod; lastStep = step;
  if (i>8 && subgroupAll(T<=0.02)) break;
}
return vec4(radiance, 1-T);
```
