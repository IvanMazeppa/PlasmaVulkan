# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview
PlasmaVulkan is a modern Vulkan 1.4 volumetric particle rendering system using C++20. It implements compute-based particle physics simulation with volumetric rendering effects.

## Build Commands

### Configure and Build
```bash
# Configure with CMake (Windows with Visual Studio)
cmake -B cmake-build-debug-visual-studio -S . -G "Visual Studio 17 2022" -DCMAKE_TOOLCHAIN_FILE=[vcpkg-root]/scripts/buildsystems/vcpkg.cmake

# Build the project
cmake --build cmake-build-debug-visual-studio --target PlasmaVulkan --config Debug

# Build shaders only
cmake --build cmake-build-debug-visual-studio --target shaders
```

### Run
```bash
# Windows
./cmake-build-debug-visual-studio/Debug/PlasmaVulkan.exe

# Linux
./build/PlasmaVulkan
```

## Architecture

### Core Systems
- **VulkanContext** (`src/renderer/VulkanContext.h/cpp`): Manages Vulkan instance, device, queues, and core state. Single source of truth for Vulkan initialization.
- **ParticleSystem** (`src/systems/ParticleSystem.h/cpp`): Compute shader-based particle simulation with clean separation between physics and rendering.
- **Application** (`src/core/Application.h/cpp`): Main application loop and system coordination.

### Key Design Patterns
- **RAII for all Vulkan resources**: Every Vulkan object is wrapped in RAII classes
- **Builder pattern**: Used for complex Vulkan object creation
- **VMA memory management**: All GPU memory allocation goes through VMA
- **Dynamic rendering**: Uses Vulkan 1.4 dynamic rendering instead of traditional render passes

### Shader Pipeline
- Compute shaders handle particle physics (`shaders/particle.comp`)
- Vertex/fragment shaders for rendering (`shaders/particle.vert/frag`)
- Shaders are compiled to SPIR-V during build via CMake custom commands
- Note: `glslc` compiler from Vulkan SDK is required but may not be in PATH

## Dependencies (managed via vcpkg)
- Vulkan SDK 1.3.268.0+
- GLFW3 (windowing)
- GLM (math)
- volk (function loading)
- VMA (memory allocation)
- vk-bootstrap (initialization)
- spdlog (logging)
- stb (image loading)

## Development Notes
- Uses C++20 features extensively
- Validation layers enabled in debug builds (`ENABLE_VALIDATION` CMake option)
- Optional Tracy profiling support (`ENABLE_PROFILING` CMake option)
- Mesh shader support when available (`USE_MESH_SHADERS` CMake option)

## Project Guardrails & Context (August 2025)

### Critical Project Awareness
- **Current Date**: August 31, 2025
- **Vulkan Version**: 1.4 (modern dynamic rendering, no render passes)
- **Architecture**: Vulkan 1.4 compute shaders with modern C++20
- **Performance Target**: 1000+ FPS with 250,000 particles achieved
- **MCP Vulkan Server**: Available for API lookups (`mcp__vulkan_db__search_vulkan_api`)

### Development Philosophy
- **Defensive Security Only**: No malicious code creation or credential harvesting
- **Prefer Editing**: Always edit existing files rather than creating new ones
- **No Proactive Documentation**: Only create docs when explicitly requested
- **Memory Consistency**: Keep development plan and progress in active memory
- **Build Directory**: Always use `cmake-build-debug-visual-studio` (not `build/windows-debug`)

### Current Project State
- **Status**: Incredibly successful! Achieving stellar formation visuals at 1000+ FPS
- **Particle Count**: 250,000 particles (2.5x increase from original 100k)
- **Physics**: Runtime controllable gravity, turbulence, damping, particle count
- **Visuals**: Vibrant 6-band velocity-based color system (blue→cyan→green→yellow→orange→red)
- **Camera**: Full mouse orbit, zoom (0.5-200 units), and panning controls
- **Controls**: G/T/D/P keys + H for help + R for reset + mouse navigation