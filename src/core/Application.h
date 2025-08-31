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
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>
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
    
protected:
    virtual void update(float deltaTime);
    virtual void render();
    
private:
    void initWindow();
    void initVulkan();
    void initImGui();
    void createCommandBuffers();
    void recordCommandBuffer(VkCommandBuffer commandBuffer, uint32_t imageIndex);
    void recreateSwapChain();
    void renderImGui(VkCommandBuffer commandBuffer);
    void cleanup();
    
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
    float m_totalTime = 0.0f;
    
    // Timing
    float m_frameTime = 0.0f;
    float m_fps = 0.0f;
    uint32_t m_frameCount = 0;
    
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
    uint32_t m_fullParticleCount = 250000;  // For orbital physics (2.5x increase)
    uint32_t m_sphParticleCount = 15000;    // For SPH physics (increased for better fluid dynamics)
    
    // Volumetric rendering
    bool m_volumetricMode = false;
    
    // ImGui state
    VkDescriptorPool m_imguiDescriptorPool = VK_NULL_HANDLE;
    bool m_showGUI = true;
    
    // State
    bool m_isRunning = false;
};

} // namespace plasma