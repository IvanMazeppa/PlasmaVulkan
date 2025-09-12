# NVIDIA RTX STBN Sample Code Notes — 2025-09-12

This note maps the contents of `resources/STBN/` to what we need in PlasmaVulkan, and highlights the minimal integration path for start‑jitter in the volumetric renderer.

## What’s in `resources/STBN/`
- `Assets/STBN.zip` — bundled asset archive (PNG/others inside)
- `Libraries/Shared/` — common utilities
  - `BlueNoiseTexturesND.{h,cpp}` — multi‑dimensional blue‑noise texture helpers (addressing/dimensions)
  - `Kernel/*` — kernel definitions for generators (Gaussian, symmetric)
  - `STBNMath.h`, `STBNRandom.h`, `ProgressContext.*` — math/random infra
- `Libraries/Scalar/` — scalar STBN (what we need for start‑jitter)
  - `STBNData.{h,cpp}` — data holders (dimensions, storage)
  - `STBNMaker.{h,cpp}` — scalar generator orchestration
  - `VoidAndCluster/*` — void‑and‑cluster implementation (reference and VC impl)
  - `Utils/*`, `Reporting/*` — helpers
- `Libraries/Vector/` — vector STBN generators (not required for scalar jitter)
  - Random type generators, exporters, importance sampling, value distance functions
- `Tools/ScalarApp` and `Tools/VectorApp` — small CLI apps to generate/export STBN
- `Support/Tests/*` — gtest unit tests for scalar/vector pipelines
- `External/*` — third‑party libs (stb, pcg, gtest, cxxopts)
- `README.md`, `License.txt` — license and upstream notes

## What we actually need
- A scalar spatiotemporal blue‑noise set with:
  - 2D tile: 128×128 (good),
  - temporal frames: 64 (array layers),
  - format: 8‑bit scalar (R8).
- The sample set present in `assets/STBN/` (already added to repo) matches this:
  - `stbn_scalar_2Dx1Dx1D_128x128x64x1_#.png` (64 PNGs)
  - Optional vector/unitvec variants are not needed for scalar start‑jitter.

## Minimal integration path (no need to compile sample code)
- Use the precomputed PNGs; no runtime generation required.
- Load PNGs via `stb_image` and pack into a single Vulkan 2D array image (R8_UNORM, 64 layers).
- Nearest filtering, repeat wrap, no mips.
- In shader: `sampler2DArray`, select `layer = (frameIndex * 3) % 64`, apply Cranley–Patterson (CP) offset to UV, fetch `n = texture(stbnTex, vec3(fract(uv), layer)).r`, compute `jitter = n - 0.5`.
- Apply to start t: `t += jitterScale * step * jitter`. Optional: hash‑based micro‑dither per step.

## When the sample code is useful
- If you want to generate custom sets or different dimensions:
  - See `Libraries/Scalar/STBNMaker.*` and `Libraries/Scalar/VoidAndCluster/*`.
  - `Tools/ScalarApp/main.cpp` shows how to wire CLI generation/export.
- For understanding addressing/ND layouts:
  - `Libraries/Shared/BlueNoiseTexturesND.{h,cpp}`
- For vector blue noise (directional jitter):
  - `Libraries/Vector/*` and `Tools/VectorApp/` (not required for current use).

## Vulkan specifics for our engine
- Image: `VK_FORMAT_R8_UNORM`, usage `SAMPLED|TRANSFER_DST`, extent 128×128, arrayLayers=64, mipLevels=1.
- View: `VK_IMAGE_VIEW_TYPE_2D_ARRAY`, levelCount=1, layerCount=64.
- Sampler: NEAREST/NEAREST, REPEAT wrap, mipmap mode NEAREST, minLod=maxLod=0.
- Descriptor: combined image sampler (fragment). If using push descriptors, mirror `VolumeRenderer` binding style.

## Sanity checks
- Visualize the noise value `n` (grayscale) to verify tiling and temporal motion.
- Confirm per‑frame CP offset and layer selection change the pattern each frame.
- Ensure NEAREST sampling (mips/linear can degrade blue‑noise spectrum).

## License
- `resources/STBN/License.txt` details upstream licensing; keep it in the repository and retain attribution as needed.

## See also
- `STBN_INTEGRATION_GUIDE_2025_09_12.md` — step‑by‑step Vulkan upload/sampler/shader usage and parameter suggestions tailored to PlasmaVulkan.
