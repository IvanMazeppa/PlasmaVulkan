# DSM In‑Depth Debug + Visualization Guide — 2025-09-12

Your DSM visualization shows flat colors (white/black/red), which means one of: rays miss the volume, T is saturated (≈1 or 0) everywhere, or the sampled DSM isn’t the map being written. This guide gives you deterministic probes to isolate each failure mode and land working shadows.

## 0) Assert your inputs (one frame dump)
- Print once at startup or on keypress:
  - Volume params: `gridOrigin`, `voxelSize`, `gridDimensions`.
  - Light params: directional `lightDir`, CPU `lightView/lightProj` (row‑major values), DSM size.
  - DSM resource state before sampling: current layout is `SHADER_READ_ONLY_OPTIMAL`, `m_dsmImageView != VK_NULL_HANDLE`.
- In DSM build, log min/max τ for a 16×16 probe grid (see Section 3).

## 1) One source of truth for matrices
- Compute `lightView/lightProj` on CPU and store in a UBO. Push the SAME UBO to:
  - DSM compute (dsm_build.comp) and
  - volume.frag (remove procedural matrix rebuild).
- After resize, recompute UBO and rebuild DSM.

MCP
- search_vulkan_spec("vkCmdPipelineBarrier2")
- search_vulkan_spec("Images")

## 2) DSM compute correctness — AABB‑clipped march
Implement these exactly in dsm_build.comp:
- Ray setup per texel
```
vec2 ndc = (vec2(texelCoord)+0.5)/vec2(dsmSize)*2.0-1.0; // [-1,1]
vec4 ls = inverse(pc.lightProjMatrix) * vec4(ndc, -1.0, 1.0);
ls /= ls.w;
vec4 ws = inverse(pc.lightViewMatrix) * ls;
vec3 rayOrigin = ws.xyz;
vec3 rayDir = -pc.lightDirection; // normalized
```
- Intersect with volume AABB in world
```
vec3 bmin = pc.gridOrigin;
vec3 bmax = pc.gridOrigin + pc.voxelSize * pc.gridDimensions;
vec3 invD = 1.0 / rayDir;
vec3 t0 = (bmin - rayOrigin) * invD;
vec3 t1 = (bmax - rayOrigin) * invD;
vec3 tsm = min(t0, t1);
vec3 tbg = max(t0, t1);
float tNear = max(0.0, max(max(tsm.x, tsm.y), tsm.z));
float tFar  = min(tbg.x, min(tbg.y, tbg.z));
if (tFar <= tNear) { imageStore(dsmImage, texelCoord, vec4(1.0,0,0,1)); return; }
```
- March and clamp τ
```
float tau=0.0; float t=tNear; float tEnd=min(tFar, pc.maxDistance);
for(; t<tEnd; t+=pc.stepSize) {
  vec3 P = rayOrigin + rayDir * t;
  float d = sampleDensity(P) * pc.densityScale;
  tau += d * pc.stepSize;
  if (tau>20.0) break; // clamp for stability
}
float T = exp(-tau);
imageStore(dsmImage, texelCoord, vec4(T,0,0,1));
```
- During debug, write τ instead: `imageStore(..., vec4(tau*0.05,0,0,1))` to avoid perceptual flatness from exp().

## 3) Instrumentation: min/max τ probe
At the end of the dispatch (or every N frames), compute min/max τ for a small set of texels and print them. If both ~0, your rays miss the volume; if both huge, step/σ_t too large; if varying, sampling path likely OK.

## 4) Barriers & descriptors
- Before compute: DSM → `GENERAL`. After: DSM → `SHADER_READ_ONLY_OPTIMAL`.
- Sample path: descriptor `imageLayout` matches actual layout.
- On resize: recreate DSM; clear to 1.0.

## 5) Visualization modes (toggle by key)
- DSM view: fullscreen draw of DSM; verify that increasing density actually darkens map.
- LightUV view (volume.frag): output `lightUV.xy` as color; it must span [0,1] across screen points inside the volume.
- Single‑sample test: in volume.frag, set `shadowVisibility = texture(dsm, lightUV).r; fragColor = vec4(vec3(shadowVisibility),1);` to validate the link end‑to‑end.

## 6) Parameter ranges
- `pc.stepSize`: ≈ `voxelSize` in world units (try [0.5..1.5]×).
- `pc.maxDistance`: ≈ diagonal of volume AABB.
- `densityScale` × σ_t (opacity scale in volume) should yield τ in [0..8] typically.

If still flat
- Force dense test: inject a synthetic blob (sphere) into `sampleDensity` path (debug define) and verify DSM shows a circular dark region. If it does, density sampling from the 3D texture is the problem (grid mapping).
- Conversely, bypass DSM sampling and set `shadowVisibility = 0.0` to ensure the render path responds.

## 7) Optional: strengthen effect and next steps
- Epipolar light‑aligned sampling: add 2–3 forward samples along lightDir per view step; blend with DSM visibility.
- Ray‑query occluders: add BH/disk BLAS instances and multiply DSM with RT visibility for dramatic shadows.

MCP searches
- search_vulkan_spec("VK_KHR_ray_query")
- search_vulkan_spec("VK_KHR_acceleration_structure")
- search_vulkan_spec("vkCmdPipelineBarrier2")
- search_vulkan_spec("Images")

## 8) Quick landing checklist
- SAME matrices for build+sample via UBO.
- AABB‑clipped DSM march; τ visible in DSM debug.
- DSM barriers/layouts correct; descriptor layout matches.
- Volume frag shows `lightUV` spans [0,1] and DSM value changes with density.
- Replace τ debug with T and wire back to HG scattering.
