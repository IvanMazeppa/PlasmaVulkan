# Quick Start Guide - PlasmaVulkan

## 🚀 Immediate Commands

### Build & Run (from project root)
```bash
# Quick build and run
cd cmake-build-debug-visual-studio
cmake --build . --target PlasmaVulkan && ./Debug/PlasmaVulkan.exe

# Just run (if already built)
./cmake-build-debug-visual-studio/Debug/PlasmaVulkan.exe
```

### If Build Fails
```bash
# Reconfigure from scratch
cd cmake-build-debug-visual-studio
rm -rf *
"/mnt/e/Program Files/CLion 2025.2/bin/cmake/win/x64/bin/cmake.exe" .. -DCMAKE_TOOLCHAIN_FILE=E:/vcpkg/vcpkg/scripts/buildsystems/vcpkg.cmake -G "Visual Studio 17 2022"
```

## 📁 Key Files for Next Session

### To Implement Particles (START HERE)
1. `src/systems/ParticleSystem.cpp` - CREATE THIS
2. `shaders/particle.comp` - Physics simulation
3. `shaders/particle.vert` - Vertex shader
4. `shaders/particle.frag` - Fragment shader

### Current Working Files
- ✅ `src/renderer/VulkanContext.cpp` - Vulkan setup (DONE)
- ✅ `src/core/Application.cpp` - Main loop (DONE)
- ✅ `src/main.cpp` - Entry point (DONE)

## 🎯 Next Task: Get Particles on Screen

### Step 1: Create ParticleSystem.cpp
```cpp
// Minimal implementation to get points on screen
class ParticleSystem {
    VkBuffer particleBuffer;
    VkPipeline computePipeline;
    VkPipeline graphicsPipeline;
    
    void update(VkCommandBuffer cmd) {
        // Dispatch compute shader
    }
    
    void render(VkCommandBuffer cmd) {
        // Draw particles
    }
};
```

### Step 2: Simple Compute Shader
```glsl
// particle.comp
layout(local_size_x = 64) in;

struct Particle {
    vec3 pos;
    vec3 vel;
};

layout(binding = 0) buffer Particles {
    Particle particles[];
};

void main() {
    uint id = gl_GlobalInvocationID.x;
    // Simple physics here
    particles[id].pos += particles[id].vel * 0.016;
}
```

### Step 3: Hook into Application::render()
```cpp
// In Application.cpp render():
if (m_particleSystem) {
    m_particleSystem->update(commandBuffer);
    m_particleSystem->render(commandBuffer);
}
```

## 🔧 Cursor/VS Code Shortcuts

### Build & Run
- `Ctrl+Shift+B` - Build
- `F5` - Debug run
- `Ctrl+F5` - Run without debug

### Claude in Cursor
- `Ctrl+L` - Open chat
- `Ctrl+K` - Inline edit
- `Ctrl+Shift+L` - Add file to context

## 🎨 Visual Goals
1. **First Goal**: Get 1000 white dots moving on screen
2. **Second Goal**: Add color based on velocity
3. **Third Goal**: Add glow/bloom effect
4. **Final Goal**: Full volumetric plasma

## ⚠️ Common Issues

### "Device Lost"
- Reduce particle count
- Check compute shader for infinite loops
- Ensure proper synchronization

### Black Screen
- Check shader compilation
- Verify pipeline creation
- Enable validation layers

### Build Errors
- vcpkg packages already installed
- Use Visual Studio 17 2022 generator
- Check CMake path is correct

## 💡 Pro Tips
- Start with 1000 particles, scale up later
- Use RenderDoc for GPU debugging
- Window title shows FPS
- Validation layers enabled in debug builds

## 📊 System Info
- **GPU**: RTX 4060 Ti (16GB VRAM)
- **API**: Vulkan 1.4
- **Resolution**: 1920x1080
- **Target FPS**: 60+

---
**Remember**: Foundation is complete. Just need to add particles!
**Current Status**: Window opens, Vulkan initialized, waiting for content
**Next Step**: Implement ParticleSystem.cpp