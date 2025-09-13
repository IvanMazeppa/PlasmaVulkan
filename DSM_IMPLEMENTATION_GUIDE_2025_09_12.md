# Deep Shadow Map (DSM) Implementation Guide — 2025-09-12

You have a DSM compute pass and volume sampling in place, but no visible shadows. Below is a focused checklist to get DSM working, based on your current shaders.

## 1) Match light space between build and sample
Current status
- dsm_build.comp uses `lightViewMatrix/lightProjMatrix` from push constants.
- volume.frag reconstructs `lightView/lightProj` procedurally with a hardcoded `lightDir` and an AABB‑fit ortho. If these differ from what the compute pass used, `sampleDSM` will fetch the wrong texel.

Fix
- Compute the light matrices ONCE on CPU and pass them to both passes:
  - Push constants (or UBO) for dsm_build.comp: `lightView`, `lightProj`.
  - Push the same matrices to volume.frag (add to `VolumePushConstants`).
- Remove procedural rebuild in `sampleDSM` and multiply by the pushed matrices.

Validation
- In volume.frag, output `lightUV` as color (RGB = lightUV, clamped) to confirm mapping.

## 2) Correct ray origin for DSM builder
Current status
- dsm_build.comp transforms texel to NDC then back through inverse projection at z = −1 (front clip), and then to world. This origin may be outside the volume depending on the light frustum.

Fix
- Establish the ray start at the NEAR plane of the light ortho that tightly encloses the volume AABB:
  - Build `lightProj` on CPU with bounds that enclose the volume (min,max in light space).
  - In the shader, compute world position at NEAR for that texel:
    - Use `L_invProj * vec4(ndc, -1, 1)` (orthographic) and then `L_invView`.
  - March along `-lightDir` until exiting the AABB; early‑out when outside.

Validation
- Render DSM as a fullscreen quad (debug) to see transmittance distribution (bright=clear, dark=opaque).

## 3) Integrate σ_t consistently
- In dsm_build.comp: τ += σ_t · density · step
- In volume.frag: scattering uses the same `sigma_t` (your `opacityScale`) and the same `densityScale` (or normalize density in DSM build to the same scale).
- If DSM uses different scaling, T may be near 1 everywhere.

Tip
- Temporarily multiply DSM result by a strength `k` in volume.frag to test visibility (`shadowVisibility = pow(texture(dsm,...).r, k)`; k>1 darkens). If this reveals shadows, tune σ_t/step in DSM build.

## 4) DSM image format, usage, and barriers
- DSM image: `R16_SFLOAT` or `R32_SFLOAT`, usage: `STORAGE | SAMPLED | TRANSFER_DST`.
- Build pass: DSM → GENERAL; after compute: DSM → SHADER_READ_ONLY_OPTIMAL.
- Sample pass: descriptor imageLayout must be READ_ONLY and match actual layout.
- On resize: recreate DSM with new extent; update matrices; clear to 1.0.

## 5) Robust AABB marching in DSM build
- Before stepping, intersect the light ray with the volume AABB (same function as in volume.frag but in light space is fine). Start at `t0`, stop at `t1`. This avoids marching outside the volume and aligns DSM and volume rays.

## 6) Use min/max hierarchy to skip in DSM
- From `samplePos`, query min/max at appropriate LOD in light space to jump over empty space: `if max<threshold → t += block; continue;`.

## 7) Volume sampling path
- In volume.frag, after matching matrices, `sampleDSM(pos)` → `T_dsm`. Multiply your HG scattering by `T_dsm` (and clamp as needed). Remove or lower the fallback `max(shadowVisibility, 0.3)` during debugging to see the raw effect.

## 8) Debug modes (add toggles)
- Show DSM texel (render DSM to screen) and `T_dsm` per step (e.g., as tint) to verify attenuation appears where dense.
- Show `lightUV` and a depth slice of density in light space to confirm alignment.

## 9) Common pitfalls
- Mismatch of light matrices between build and sample → no shadows.
- Wrong DSM origin or frustum size → rays miss the volume → T≈1.
- σ_t/step too small in DSM build → too clear; or too large → fully dark; tune against volume’s parameters.
- Descriptor layouts not matching actual layouts at use → silent failure or validation.

## 10) Minimal CPU integration steps
1) Compute volume AABB corners in world; transform to light space; derive tight ortho bounds and build `lightView/lightProj`.
2) Push these matrices and DSM parameters to dsm_build.comp; dispatch (1024×1024 in 8×8 groups).
3) Barrier DSM → READ_ONLY.
4) Push same matrices to volume pass; sample `T_dsm = texture(dsm, lightUV).r`.
5) Multiply scattering by `T_dsm`; remove the 0.3 floor during testing; verify shadows.

## MCP spec references
- search_vulkan_spec("vkCmdPipelineBarrier2") — Synchronization2 barriers for DSM transitions.
- search_vulkan_spec("Images") — image usage and layout transitions.

Once DSM casts shadows visibly and stably, add epipolar sampling and (optionally) ray‑query occluders for hard shadows from BH/disk geometry.
