# NVIDIA RTX STBN Integration Guide (PlasmaVulkan) — 2025-09-12

This guide explains how to integrate the precomputed spatiotemporal blue-noise (STBN) textures found in `assets/STBN/` into the volumetric renderer for start-jitter and optional per-step micro-dither. The goal is banding reduction and temporal stability at minimal cost.

## What we have in assets
- Scalar STBN: `stbn_scalar_2Dx1Dx1D_128x128x64x1_#.png` (64 frames, 128×128, single-channel)
- Unit-vector STBN: `stbn_unitvec{1,2,3}_2Dx1D_128x128x64_#.png` (vector blue noise variants)
- Magnitude visualization: `.mag.xy.png`, `.mag.xz.png` (for debugging)

Recommendation for this project: use the scalar set (R8) as a 2D array (64 layers). It is perfect for 2D screen-space jitter and step start jitter.

## Pipeline overview
1) Load 64 scalar PNGs into a 2D array texture (format R8_UNORM).
2) Create a nearest-repeat sampler (no mips).
3) Bind as a combined image sampler (fragment stage).
4) In the shader, pick array layer by frame index and apply Cranley–Patterson (CP) offset per frame.
5) Use the sampled value to offset the ray start t (primary) and optionally modulate per-step length slightly.

## Vulkan implementation steps

### 1) Texture creation (2D array)
- Create `VkImage` with:
  - type: 2D, extent: 128×128, mipLevels: 1
  - arrayLayers: 64
  - format: `VK_FORMAT_R8_UNORM`
  - usage: `VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT`
- Allocate/bind device-local memory.
- Create `VkImageView` with `viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY`, `levelCount = 1`, `layerCount = 64`.

### 2) Upload
- Decode PNGs on CPU (stb_image) to 8‑bit R channel.
- For each layer L ∈ [0..63]:
  - Stage data into a buffer, then issue `vkCmdCopyBufferToImage` with `imageSubresource.layerCount=1` and `baseArrayLayer=L`.
- Barrier to `SHADER_READ_ONLY_OPTIMAL`.

### 3) Sampler
- `magFilter = minFilter = VK_FILTER_NEAREST`
- `addressModeU = addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT`
- `mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST`, `minLod=maxLod=0`

### 4) Descriptor
- Add a combined image sampler binding (fragment stage). If using push descriptors, mirror your volume sampler path.

## Shader usage (fragment)

### Bindings
- Add `layout(binding = X) uniform sampler2DArray stbnTex;`

### Indexing and CP transform
- Per-frame CP offset (push constants or UBO): `vec2 cp = fract(frameRandom + vec2(frameIndex * φ1, frameIndex * φ2))` with irrational-ish φ.
- Screen UVs in pixels or NDC mapped to tile:
  - `vec2 uv = (fragCoord * screenPixels / 128.0) + cp;`
  - `float n = texture(stbnTex, vec3(fract(uv), float(frameIndex % 64))).r;`
  - `float jitter = (n - 0.5);`

### Apply
- Start jitter (primary): `t += jitterScale * currentStep * jitter;`
- Optional per-step micro-dither: `step *= (1.0 + microScale * hashOrStbn(i, uv));`
  - Prefer hashing `i` to avoid per-step texture fetches.

## Choosing which STBN variant
- Scalar set is ideal for scalar jitter; cheap and effective.
- Unit-vector sets are useful if you need angle/2D direction jitter (not necessary for start‑t here).

## Parameter suggestions
- jitterScale (start t): 0.5–1.0 × currentStep
- microScale (per-step): 0.1–0.2 (only if banding persists)
- CP offsets: cycle frame index with a prime stride (e.g., `(frame * 3) % 64`) and add a small 2D offset per frame.

## Validation checklist
- Visualize STBN value: temporarily output `n` as grayscale to check tiling and motion.
- Ensure no mips and NEAREST sampling (blurred blue noise harms spectrum).
- Confirm temporal decorrelation: noise pattern shifts each frame without drifting.

## Integration points in this codebase
- Image upload/descriptor setup similar to HDR/bloom image paths in `Application.cpp`.
- Descriptor binding model mirrors volume texture push descriptor in `VolumeRenderer::render`.
- Use the frame counter available in `Application` for layer selection and CP offset; pass via push constants to `volume.frag`.

## Performance notes
- One 2D array fetch per pixel per frame (for start jitter) is negligible versus ray marching.
- Avoid per-step texture fetches; use a hash for intra-loop micro-dither.

## Troubleshooting
- If you see grid patterns: ensure REPEAT wrap and CP offset; avoid aligning tile to pixel grid (use screen pixels scale or introduce slight scale skew).
- If banding persists: double-check that your continuous LOD is engaged and try the preintegrated segment LUT.
