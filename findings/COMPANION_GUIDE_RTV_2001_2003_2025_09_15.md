### Companion Guide: RT Volume Start (RTV-2001 → RTV-2003)

Context
- We pivoted to a fresh RT-centric volumetric path. Prior issues: descriptor/pipeline drift, depth-without-attachment, barriers inside dynamic rendering, empty workgroups, timeline misuses.

RTV-2001: Skeleton and Mode Integration
- Create `RTVolumeRenderer` with RAII; add mode toggle in `Application`.
- HDR target: R16G16B16A16_SFLOAT, STORAGE | SAMPLED. Composite via fullscreen.
- Use Synchronization2 for single-time submits; no barriers inside dynamic rendering.
- Checks:
  - Toggle mode on/off without affecting mesh/point.
  - Resize-safe; no VUIDs.

RTV-2002: Density Grid + Splat + Mips
- 3D density (R16F), dims 192–256³, mips = floor(log2(maxDim))+1.
- Compute splat from particle buffer; guard empty workgroups; bounds check writes; consider tile/binning later.
- Build mips (blit or compute); keep layouts coherent with descriptor usage.
- Checks:
  - Slice debug and histogram look sane.
  - No 09600; one-time init transitions.

RTV-2003: Compute Ray Marcher
- Inputs: density 3D (sampled), STBN, optical depth LUT, light params.
- STBN jitter; cone/adaptive stepping; early termination on transmittance.
- Beer–Lambert + HG phase; write to HDR storage image.
- Optional TAA after march; update prev matrix after TAA.
- Checks:
  - March outputs visible glow with controllable noise.
  - No purple/green in TAA; history reset on resize/mode switch.

Pitfalls to avoid
- Empty/partial workgroups: return early; never write past SetMeshOutputs.
- Depth state: disable unless depth attachment is present.
- Descriptor optionality: use split pipeline or push descriptors; avoid binding null AS.
- Sync: use timeline semaphores for long ops; never block host in render loop.

MCP queries
- search_vulkan_api: Synchronization2, dynamic rendering, rayQuery, image layout transitions for 3D images.
- get_vulkan_entity: `vkCmdPipelineBarrier2`, `VkRenderingInfo`, `rayQueryEXT` interface, `vkCmdBlitImage` with 3D.
