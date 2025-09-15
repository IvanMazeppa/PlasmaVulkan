### Claude Starter Brief — PlasmaVulkan (Vulkan 1.4, Mesh-RT Self‑Shadowing)

Purpose
- Provide a crisp handoff so you retain context across new chat sessions.
- What we’re doing: Implement high‑performance self‑shadowing for the mesh particle cloud using RTX Ray Query against a coarse iso‑surface shell TLAS. Volumetric path remains, but current focus is mesh + RT.

Why this approach
- External occluders did not produce meaningful shadows on the particle cloud. We pivoted to approximate the cloud volume with 1–3 iso‑surface shells extracted from a low‑res density grid and trace self‑shadow rays against those shells. This gives stable, visible, scalable shadows at high FPS.

Current state (baseline)
- API: Vulkan 1.4; features enabled: rayQuery, accelerationStructure; Synchronization2; dynamic rendering; mesh shaders.
- Renderer focus: Mesh shader particle renderer (1M+ particles at 300–375 FPS). Volumetric renderer exists but is not the target for RT now.
- RT foundation done (bindings, push descriptors, minimal queries). Self‑shadowing path implemented but not yet visible due to binding/sync issues and a crash during shell TLAS rebuild.

What’s completed
- 1001–1004: RT foundations for mesh path (AS enabled, descriptors, minimal ray query, external occluders). Logs show AS creation; some validation issues remain for scratch usage.
- 1012: Coarse density grid from particles (e.g., 96³, R16_SFLOAT) updates per frame with mips.
- 1013: Iso‑surface shells extracted (simplified marching cubes) → BLAS per shell → TLAS instanced periodically.
- 1014: Mesh fragment shader supports dual TLAS (binding 3 external, binding 4 shells) and combines occlusion. Push descriptors integrated.

Known issues to fix now
- VUID 03674: AS scratch buffers missing VK_BUFFER_USAGE_STORAGE_BUFFER_BIT; invalid scratch device addresses during vkCmdBuildAccelerationStructuresKHR.
- VUID 01197 and 09600: Coarse 3D image mip layout transitions incorrect (oldLayout mismatch), and descriptor layout vs current subresource layout mismatch.
- Shell TLAS likely not bound at render time (logs show only “Pushed 2 descriptors: particle_buffer external_TLAS”). No visible self‑shadows.
- Exit‑time VUID 05137: live buffers/images at device destroy → missing RAII/teardown for shell resources.

Next change requests (apply in order)
1) 1015_as_scratch_usage: Correct scratch usage flags/sizing/alignment via vkGetAccelerationStructureBuildSizesKHR; STORAGE_BUFFER | SHADER_DEVICE_ADDRESS; honor minAccelerationStructureScratchOffsetAlignment.
2) 1019_coarse_layout_mips: Fix per‑mip layout transitions and mip chain barriers; track per‑mip layout; no transitions inside active rendering.
3) 1016_shell_tlas_sync: Timeline semaphore chain: extract → BLAS → TLAS → render wait; log timeline values.
4) 1017_bind_shell_tlas: Push shell TLAS at binding 4 in mesh pass; log both AS device addresses; add shader guard for null binding.
5) 1018_rayquery_mask_overlay: Instance masks (external=0x01, shells=0x02); self‑shadow cullMask=0x02; add debug overlay coloring hit distance.
6) 1020_raii_cleanup: Deterministic teardown (TLAS → BLAS → buffers) with RAII; assert no live resources at shutdown.

Acceptance after applying CRs
- No VUID 03674/01197/09600 in logs across multiple frames/rebuilds.
- Render log shows: “Pushed 3 descriptors: particle_buffer external_TLAS shell_TLAS” with non‑zero shell TLAS address.
- Overlay mode reveals self‑shadow hits; masks isolate shells vs external.
- No exit‑time VUID 05137; clean shutdown.

Key files and bindings
- shaders/particle_mesh.frag: binding 3 = external TLAS, binding 4 = shell TLAS; functions: hasOccluderRT(), hasSelfShadowRT(); add mask/overlay toggles.
- src/systems/MeshParticleRenderer.*: push descriptor bindings for particle buffer + TLAS(3) + shell TLAS(4); render() integrates VolumeRenderer for shell TLAS.
- src/systems/VolumeRenderer.*: coarse grid create/update/mips; marching cubes; BLAS/TLAS builds; timeline semaphore for rebuild sync.
- src/renderer/VulkanContext.cpp: enables VK_KHR_acceleration_structure, VK_KHR_ray_query and features.

Build & run
- Build: cmake --build cmake-build-debug-visual-studio --target PlasmaVulkan
- Run: ./cmake-build-debug-visual-studio/Debug/PlasmaVulkan.exe
- Compile shaders: cmake --build cmake-build-debug-visual-studio --target shaders

Where to look for signals
- Logs: logs/run.txt. Expect scratch size/alignment logs, per‑mip layout state, timeline wait value, and descriptor push lines.
- Results: results/*.json — job results and status summaries.
- Changes: changes/*.json — change requests with acceptance and MCP queries.
- Findings: findings/* — companion guides.

MCP quick references (use sparingly)
- get_vulkan_entity('vkGetAccelerationStructureBuildSizesKHR')
- get_vulkan_entity('VkPhysicalDeviceAccelerationStructurePropertiesKHR')
- search_vulkan_spec('Synchronization2 image layout transitions with mip levels')
- get_vulkan_entity('vkCmdBlitImage')
- get_vulkan_entity('VkSemaphoreSubmitInfo') / 'VkSubmitInfo2'
- get_vulkan_entity('VkWriteDescriptorSetAccelerationStructureKHR')

Style & constraints
- Modern C++20; RAII resource mgmt; descriptive names. Always check VkResult. Prefer Sync2; dynamic rendering; VMA for memory (where available). Compute for particle physics. Do not perform barriers/copies inside an active dynamic rendering scope.

If shadows still not visible
- Verify shell TLAS device address non‑zero at render and that binding_4 is pushed.
- Ensure render waits on timeline value signaled by TLAS build.
- Use cullMask=0x02 for self‑shadow query to target shell instances only.
- Enable overlay to confirm hit distances; if no hits, inspect shell thresholds and BLAS triangle counts.

“First message” you can send to get started
- Goal: Apply CR 1015 then 1019, re‑run, confirm no VUID 03674/01197/09600, then implement 1016/1017/1018 and verify that logs show 3 pushed descriptors and overlay reveals hits. Finish with 1020 RAII cleanup.


