#pragma once

#define VK_NO_PROTOTYPES
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include <vulkan/vulkan.h>

// Configure VMA to use volk for function loading
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 0
#include <vk_mem_alloc.h>

#include "renderer/VulkanContext.h"
#include "systems/ParticleSystem.h"
#include "systems/VolumeRenderer.h"
#include <memory>
#include <string>
#include <vector>
#include <chrono>

namespace plasma {

/**
 * Main application class
 */
class Application {
public:
    Application(const std::string& title, uint32_t width, uint32_t height);
    ~Application();
    
    // Delete copy/move
    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;
    
    void run();
    void stop();
    
    // Accessors
    VmaAllocator getAllocator() const { return m_allocator; }
    
    // Setters for initial configuration (call before run())
    void setActiveParticleCount(uint32_t count);
    void setGravityStrength(float strength);
    void setTurbulenceStrength(float strength);
    void setDampingFactor(float factor);
    void setAngularMomentumBoost(float boost);
    void setTimeScale(float scale);
    void setConstraintShape(int shape);
    void setBlackHoleMass(float mass);
    void setGravityCenter(const glm::vec3& center);
    void setDualGalaxyMode(bool enabled);
    void setGravityCenter2(const glm::vec3& center);
    void setBlackHoleMass2(float mass);
    void setCameraPosition(float distance, const glm::vec3& target);
    
protected:
    virtual void update(float deltaTime);
    virtual void render();
    
private:
    void initWindow();
    void initVulkan();
    void createCommandBuffers();
    void recordCommandBuffer(VkCommandBuffer commandBuffer, uint32_t imageIndex);
    void recreateSwapChain();
    void cleanup();
    
    // Enhanced console display
    void updateWindowTitle();
    void printStatusUpdate();
    void printParameterChange(const std::string& paramName, float value);
    
    // Callbacks
    static void framebufferResizeCallback(GLFWwindow* window, int width, int height);
    static void keyCallback(GLFWwindow* window, int key, int scancode, int action, int mods);
    static void mouseButtonCallback(GLFWwindow* window, int button, int action, int mods);
    static void cursorPosCallback(GLFWwindow* window, double xpos, double ypos);
    static void scrollCallback(GLFWwindow* window, double xoffset, double yoffset);
    
    // Window
    GLFWwindow* m_window = nullptr;
    std::string m_title;
    uint32_t m_width;
    uint32_t m_height;
    bool m_framebufferResized = false;
    
    // Vulkan
    std::unique_ptr<VulkanContext> m_vulkanContext;
    VmaAllocator m_allocator = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> m_commandBuffers;
    
    // Systems
    std::unique_ptr<ParticleSystem> m_particleSystem;
    std::unique_ptr<VolumeRenderer> m_volumeRenderer;
    
    // Frame management
    uint32_t m_currentFrame = 0;
    std::chrono::high_resolution_clock::time_point m_startTime;
    float m_currentPhysicsDeltaTime = 0.016f; // Current physics timestep
    float m_totalTime = 0.0f;
    float m_physicsAccumulator = 0.0f; // Accumulator for fixed timestep physics
    bool m_shouldUpdatePhysics = false; // Flag to indicate if physics should update this frame
    
    // Timing
    float m_frameTime = 0.0f;
    float m_fps = 0.0f;
    uint32_t m_frameCount = 0;
    
    // Enhanced console display
    float m_statusUpdateTimer = 0.0f;
    bool m_showOSD = true;
    float m_lastParamUpdate = 0.0f;
    
    // Advanced physics parameters
    float m_timeScale = 1.0f;
    bool m_repulsiveGravity = false;
    float m_energyInjection = 0.0f;
    float m_energyTimer = 0.0f;
    
    // Shape constraints
    enum class ConstraintShape {
        NONE = 0,           // Open space - current behavior
        SPHERE = 1,         // Spherical boundary
        DISC = 2,           // Flat disc (accretion disc)
        TORUS = 3,          // Donut shape (fusion reactor style)
        ACCRETION_DISK = 4  // Black hole accretion disk simulation
    };
    ConstraintShape m_constraintShape = ConstraintShape::NONE;
    bool m_showWireframe = false;
    float m_constraintRadius = 15.0f;  // Main radius for all shapes
    float m_constraintThickness = 3.0f; // For disc thickness, torus tube radius
    
    // Accretion disk physics parameters
    float m_blackHoleMass = 1.0f;      // In solar masses
    float m_alphaViscosity = 0.1f;     // Shakura-Sunyaev α parameter (0.01-0.4 typical)
    float m_temperatureScale = 1.0f;   // Global temperature scaling for color control
    
    // Enhanced camera controls
    float m_cameraDistance = 20.0f;
    float m_cameraTheta = 0.0f;     // Horizontal rotation (azimuth)
    float m_cameraPhi = 0.0f;       // Vertical rotation (elevation) 
    glm::vec3 m_cameraTarget = {0.0f, 0.0f, 0.0f}; // Look-at point
    
    // Mouse interaction state
    bool m_mousePressed = false;
    double m_lastMouseX = 0.0;
    double m_lastMouseY = 0.0;
    bool m_middleMousePressed = false; // For panning
    
    // Performance optimization  
    uint32_t m_fullParticleCount = 2000000; // 2 million particles - performance balance
    uint32_t m_sphParticleCount = 25000;    // Increased SPH for better fluid dynamics
    
    // Volumetric rendering
    bool m_volumetricMode = false;
    
    // State
    bool m_isRunning = false;
};

} // namespace plasma