# PlasmaVulkan - Modern Vulkan 1.4 Particle System

## Project Philosophy
- **Clean Code First**: Every module should be understandable in isolation
- **Safety by Design**: Use RAII, smart pointers, and modern C++ to prevent errors
- **Performance Second**: Get it working correctly first, optimize later
- **Modular Architecture**: Each system should be replaceable without affecting others

## Directory Structure

```
plasmavulkan/
├── src/
│   ├── main.cpp                 # Entry point - minimal
│   ├── core/
│   │   ├── Application.h/cpp    # Main application class
│   │   ├── Window.h/cpp         # GLFW window management
│   │   └── Timer.h/cpp          # Frame timing
│   ├── renderer/
│   │   ├── VulkanContext.h/cpp  # Vulkan instance, device, queues
│   │   ├── Swapchain.h/cpp      # Swapchain management
│   │   ├── Pipeline.h/cpp       # Pipeline abstraction
│   │   ├── Buffer.h/cpp         # Buffer management with VMA
│   │   ├── Image.h/cpp          # Image/texture management
│   │   ├── CommandBuffer.h/cpp  # Command buffer helpers
│   │   └── RenderPass.h/cpp     # Render pass abstraction
│   ├── systems/
│   │   ├── ParticleSystem.h/cpp # Particle simulation
│   │   ├── VolumeRenderer.h/cpp # Volumetric rendering
│   │   └── Camera.h/cpp         # Camera controls
│   └── utils/
│       ├── Math.h               # GLM wrappers and math utilities
│       ├── FileIO.h/cpp         # File loading utilities
│       └── Debug.h/cpp          # Debug utilities
├── shaders/
│   ├── particle.comp            # Particle physics compute shader
│   ├── particle.vert            # Particle vertex shader
│   ├── particle.frag            # Particle fragment shader
│   ├── volume.comp              # Volume density computation
│   ├── volume.frag              # Volume ray marching
│   └── common.glsl              # Shared shader code
├── assets/
│   └── textures/
├── external/                    # Third-party libraries (via vcpkg)
├── CMakeLists.txt              # Modern CMake configuration
├── CMakePresets.json           # Build presets
├── vcpkg.json                  # Dependencies
└── README.md                   # Documentation
```

## Core Design Principles

### 1. RAII Everywhere
Every Vulkan resource is wrapped in a RAII class that handles creation and destruction.

### 2. Builder Pattern for Complex Objects
Use builders for complex Vulkan objects to make the code readable.

### 3. Clear Ownership
Use `std::unique_ptr` for single ownership, `std::shared_ptr` only when necessary.

### 4. Error Handling
Use exceptions for initialization errors, return codes for runtime errors.

### 5. Minimal Public Interfaces
Each class exposes only what's necessary, hiding Vulkan complexity.

## Key Components

### VulkanContext
- Manages instance, device, queues
- Handles validation layers and extensions
- Single source of truth for Vulkan state

### ParticleSystem
- Compute-based particle simulation
- Clean separation between simulation and rendering
- Easily extensible physics

### VolumeRenderer
- Ray marching based volumetric rendering
- Density field from particles
- Temperature-based emission

### Modern Features
- Vulkan 1.4 with dynamic rendering
- Mesh shaders for efficient particle rendering
- Timeline semaphores for synchronization
- Descriptor indexing for bindless textures

## Build System
- CMake 3.25+ with modern practices
- vcpkg for dependency management
- Precompiled headers for faster builds
- Automatic shader compilation

## Dependencies (via vcpkg)
- vulkan-sdk (1.4+)
- glfw3 (window management)
- glm (math)
- volk (Vulkan function loading)
- vma (memory allocation)
- vk-bootstrap (simplified initialization)
- spdlog (logging)
- tracy (optional profiling)