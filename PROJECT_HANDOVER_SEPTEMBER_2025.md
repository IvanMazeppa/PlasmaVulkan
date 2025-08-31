# PlasmaVulkan Project Handover - September 2025

## Executive Summary
This is a clean-slate Vulkan 1.4 volumetric particle rendering project, rebuilt from scratch after the original `plasma_vk` project became too messy. The goal is to create spectacular, GPU-accelerated plasma effects leveraging modern Vulkan features and the RTX 4060 Ti's capabilities.

## Project Context & History

### Timeline
- **August 2025**: Original `plasma_vk` project developed but became cluttered with issues
- **August 30, 2025**: Complete project reset initiated at user's request
- **Current**: Clean foundation established, ready for particle system implementation

### Previous Issues Resolved
- The original project had invisible volumetric effects and device lost errors
- GPT-5 bridge MCP server work was completed and set aside
- Decision made to focus purely on graphics rather than tooling

### Current Setup
- **IDE**: Cursor (with trial account) + Claude Code in terminal
- **Location**: `/mnt/d/users/dilli/androidstudioprojects/plasmavulkan/`
- **Build System**: CMake with vcpkg dependency management
- **Compiler**: Visual Studio 2022 (MSVC 19.44)
- **Platform**: Windows 11 (WSL2 for development)
- **GPU**: NVIDIA GeForce RTX 4060 Ti

## Technical Architecture

### Core Systems Status

#### ✅ COMPLETED
1. **VulkanContext** (`src/renderer/VulkanContext.cpp/.h`)
   - Vulkan 1.4 instance creation with validation layers
   - Device selection (successfully picks RTX 4060 Ti)
   - Swap chain management
   - Command pool and sync objects
   - Dynamic rendering enabled (no render passes needed)

2. **Application Framework** (`src/core/Application.cpp/.h`)
   - GLFW window management (1920x1080)
   - Main render loop with proper frame synchronization
   - FPS counter in window title
   - Clean shutdown handling

3. **Build System**
   - CMake configuration with vcpkg
   - Shader compilation pipeline (GLSL → SPIR-V)
   - All dependencies resolved via vcpkg

#### 🚧 IN PROGRESS
1. **Particle System** (`src/systems/ParticleSystem.h`)
   - Headers created but implementation pending
   - Planned: 1 million+ particles with compute shader physics

#### 📋 TODO
1. **Volumetric Rendering**
   - Ray marching through 3D density fields
   - Plasma glow effects
   - Temperature-based color emission

## Key Technical Decisions

### Vulkan 1.4 Features Utilized
```cpp
// Dynamic Rendering (no render passes)
VkRenderingInfo renderingInfo{};
vkCmdBeginRendering(commandBuffer, &renderingInfo);

// Timeline Semaphores (better sync)
// Buffer Device Address (GPU pointers)
// Mesh Shaders (if supported)
```

### Memory Management
- VMA (Vulkan Memory Allocator) for all GPU allocations
- RAII pattern throughout for automatic cleanup
- Volk for dynamic function loading

### Shader Pipeline
```
shaders/*.glsl → glslc → shaders/*.spv
- particle.comp: Physics simulation
- particle.vert/frag: Billboard rendering
- volume.comp: Density field updates (planned)
```

## Build Instructions

### Prerequisites
- Windows with Visual Studio 2022
- Vulkan SDK 1.3.296.0 installed at `C:\VulkanSDK\`
- vcpkg installed at `E:\vcpkg\`
- CMake via CLion at `E:\Program Files\CLion 2025.2\bin\cmake\win\x64\bin\cmake.exe`

### Build Commands
```bash
# Configure (from project root)
cd cmake-build-debug-visual-studio
"/mnt/e/Program Files/CLion 2025.2/bin/cmake/win/x64/bin/cmake.exe" .. \
  -DCMAKE_TOOLCHAIN_FILE=E:/vcpkg/vcpkg/scripts/buildsystems/vcpkg.cmake \
  -G "Visual Studio 17 2022"

# Build executable
"/mnt/e/Program Files/CLion 2025.2/bin/cmake/win/x64/bin/cmake.exe" \
  --build . --target PlasmaVulkan

# Build shaders
"/mnt/e/Program Files/CLion 2025.2/bin/cmake/win/x64/bin/cmake.exe" \
  --build . --target shaders

# Run
./Debug/PlasmaVulkan.exe
```

## Current State
- ✅ Window opens successfully
- ✅ Vulkan initializes without errors
- ✅ Renders black screen (no content yet)
- ✅ Proper frame pacing and sync
- ⚠️ No visual content implemented yet

## Next Implementation Steps

### 1. Particle System (PRIORITY)
```cpp
// In ParticleSystem.cpp, implement:
struct Particle {
    glm::vec3 position;
    glm::vec3 velocity;
    float temperature;
    float life;
};

// Compute shader for physics
// - Plasma confinement forces
// - Temperature diffusion
// - Vortex dynamics
```

### 2. Volumetric Rendering
```glsl
// volume_raymarch.frag
// - Ray march through 3D texture
// - Accumulate density
// - Apply emission based on temperature
// - Bloom post-process for glow
```

### 3. RTX 4060 Ti Optimizations
- Use async compute for particle physics
- Leverage 16GB VRAM for large particle counts
- Consider DLSS integration for upscaling
- Ray tracing for advanced lighting (optional)

## Project Goals & Vision

### Visual Targets
1. **Plasma Accretion Disk** - Swirling, hot plasma with gravitational effects
2. **Volumetric Lightning** - Branching electrical discharges through volume
3. **Stellar Corona** - Sun-like plasma eruptions and magnetic field lines
4. **Nebula Effects** - Colorful gas clouds with internal turbulence

### Technical Excellence Goals
- 60+ FPS with 1M+ particles
- Real-time parameter tweaking via GUI
- Physically-inspired (not necessarily accurate) simulations
- Beautiful, cinematic quality output

## Important Context for AI Assistants

### User Background
- Experienced with Java, learning C++ and Vulkan
- Prefers clean, well-structured code
- Values visual results over perfect accuracy
- Has struggled with Vulkan's complexity but committed to learning

### Code Style Preferences
- Modern C++20 features
- Clear variable names over brevity
- Extensive comments for learning
- RAII and smart pointers everywhere

### Session Context
- This is September 2025
- GPT-5 and Claude Opus 4.1 are available
- User has Cursor trial with Claude integration
- Previous 10+ hours spent on earlier attempts
- User wants "something spectacular" with Vulkan 1.4

## Critical Files to Review

1. `/src/renderer/VulkanContext.cpp` - Core Vulkan setup
2. `/src/core/Application.cpp` - Main application loop
3. `/shaders/particle.comp` - Particle physics (ready for implementation)
4. `/CMakeLists.txt` - Build configuration
5. `/.vscode/` - IDE configuration for Cursor

## Known Issues & Workarounds

1. **Post-build asset copy fails** - Ignore, no assets needed yet
2. **Validation layer warnings** - Normal for now, will fix as needed
3. **WSL path issues** - Use Windows paths for CMake, WSL paths for navigation

## Immediate Next Task
**Implement the particle system compute shader** to get visual output on screen. Start with simple point sprites, then add physics, then volumetrics.

## Success Metrics
- Particles visible on screen
- Smooth 60+ FPS performance
- No device lost errors
- Visually impressive output that showcases RTX 4060 Ti

## Final Notes
The foundation is solid. The user is excited about creating beautiful visuals. Focus on getting something impressive on screen quickly, then iterate. The clean architecture allows for rapid experimentation without the mess of the original project.

Remember: The goal is "spectacular visuals" - prioritize beauty over perfect physics accuracy.

---
*Handover prepared: August 30, 2025*
*Next session: Continue with particle system implementation*