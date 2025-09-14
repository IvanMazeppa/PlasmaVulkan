#pragma once

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <GLFW/glfw3.h>
#include <volk.h>
#include <vk_mem_alloc.h>

#include <memory>
#include <vector>
#include <string>
#include <optional>
#include <algorithm>

namespace plasma {

// Helper structures
struct QueueFamilyIndices {
    std::optional<uint32_t> graphicsFamily;
    std::optional<uint32_t> presentFamily;
    std::optional<uint32_t> computeFamily;
    
    bool isComplete() const {
        return graphicsFamily.has_value() && 
               presentFamily.has_value() && 
               computeFamily.has_value();
    }
};

struct SwapChainSupportDetails {
    VkSurfaceCapabilitiesKHR capabilities;
    std::vector<VkSurfaceFormatKHR> formats;
    std::vector<VkPresentModeKHR> presentModes;
};

/**
 * VulkanContext manages the core Vulkan objects
 */
class VulkanContext {
public:
    static constexpr int MAX_FRAMES_IN_FLIGHT = 3; // Match swapchain image count
    
    explicit VulkanContext(GLFWwindow* window, bool enableValidation = true);
    ~VulkanContext();
    
    // Delete copy/move
    VulkanContext(const VulkanContext&) = delete;
    VulkanContext& operator=(const VulkanContext&) = delete;
    
    // Getters
    VkInstance getInstance() const { return m_instance; }
    VkPhysicalDevice getPhysicalDevice() const { return m_physicalDevice; }
    VkDevice getDevice() const { return m_device; }
    VkSurfaceKHR getSurface() const { return m_surface; }
    VkQueue getGraphicsQueue() const { return m_graphicsQueue; }
    VkQueue getPresentQueue() const { return m_presentQueue; }
    VkQueue getComputeQueue() const { return m_computeQueue; }
    VkCommandPool getCommandPool() const { return m_commandPool; }
    VkSwapchainKHR getSwapChain() const { return m_swapChain; }
    VkFormat getSwapChainImageFormat() const { return m_swapChainImageFormat; }
    VkExtent2D getSwapChainExtent() const { return m_swapChainExtent; }
    const std::vector<VkImage>& getSwapChainImages() const { return m_swapChainImages; }
    const std::vector<VkImageView>& getSwapChainImageViews() const { return m_swapChainImageViews; }
    VkImage getSwapChainImage(uint32_t index) const { return m_swapChainImages[index]; }
    VkImageView getSwapChainImageView(uint32_t index) const { return m_swapChainImageViews[index]; }
    
    // Sync objects
    VkSemaphore getImageAvailableSemaphore(uint32_t frame) const { return m_imageAvailableSemaphores[frame]; }
    VkSemaphore getRenderFinishedSemaphore(uint32_t frame) const { return m_renderFinishedSemaphores[frame]; }
    VkFence getInFlightFence(uint32_t frame) const { return m_inFlightFences[frame]; }
    
    // Properties
    const VkPhysicalDeviceProperties& getDeviceProperties() const { return m_deviceProperties; }
    const VkPhysicalDeviceFeatures& getDeviceFeatures() const { return m_deviceFeatures; }
    const QueueFamilyIndices& getQueueFamilyIndices() const { return m_queueFamilyIndices; }
    
    // Capabilities
    bool supportsShaderAtomicFloat() const { return m_supportsAtomicFloat; }
    bool supportsMeshShaders() const { return m_supportsMeshShaders; }
    bool supportsSubgroupBallot() const { return m_supportsSubgroupBallot; }
    bool supportsRayQuery() const { return m_supportsRayQuery; }
    bool supportsAccelerationStructure() const { return m_supportsAccelerationStructure; }
    uint32_t getSubgroupSize() const { return m_subgroupSize; }
    
    // Command buffer helpers
    VkCommandBuffer beginSingleTimeCommands();
    void endSingleTimeCommands(VkCommandBuffer commandBuffer);
    
    // Swap chain recreation
    void createSwapChain();
    
private:
    void createInstance();
    void setupDebugMessenger();
    void createSurface();
    void pickPhysicalDevice();
    void createLogicalDevice();
    void createCommandPool();
    void createSyncObjects();
    void recreateSyncObjects();
    void cleanup();
    
    // Helper functions
    bool isDeviceSuitable(VkPhysicalDevice device);
    bool checkDeviceExtensionSupport(VkPhysicalDevice device);
    QueueFamilyIndices findQueueFamilies(VkPhysicalDevice device);
    SwapChainSupportDetails querySwapChainSupport(VkPhysicalDevice device);
    VkSurfaceFormatKHR chooseSwapSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& availableFormats);
    VkPresentModeKHR chooseSwapPresentMode(const std::vector<VkPresentModeKHR>& availablePresentModes);
    VkExtent2D chooseSwapExtent(const VkSurfaceCapabilitiesKHR& capabilities);
    std::vector<const char*> getRequiredExtensions();
    
    // Core objects
    GLFWwindow* m_window;
    VkInstance m_instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT m_debugMessenger = VK_NULL_HANDLE;
    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    VkSurfaceKHR m_surface = VK_NULL_HANDLE;
    
    // Queues
    VkQueue m_graphicsQueue = VK_NULL_HANDLE;
    VkQueue m_presentQueue = VK_NULL_HANDLE;
    VkQueue m_computeQueue = VK_NULL_HANDLE;
    QueueFamilyIndices m_queueFamilyIndices;
    
    // Swap chain
    VkSwapchainKHR m_swapChain = VK_NULL_HANDLE;
    std::vector<VkImage> m_swapChainImages;
    std::vector<VkImageView> m_swapChainImageViews;
    VkFormat m_swapChainImageFormat;
    VkExtent2D m_swapChainExtent;
    
    // Command pool
    VkCommandPool m_commandPool = VK_NULL_HANDLE;
    
    // Sync objects
    std::vector<VkSemaphore> m_imageAvailableSemaphores;
    std::vector<VkSemaphore> m_renderFinishedSemaphores;
    std::vector<VkFence> m_inFlightFences;
    
    // Device properties
    VkPhysicalDeviceProperties m_deviceProperties{};
    VkPhysicalDeviceFeatures m_deviceFeatures{};
    VkPhysicalDeviceSubgroupProperties m_subgroupProperties{};
    bool m_supportsAtomicFloat = false; // VK_EXT_shader_atomic_float
    bool m_supportsMeshShaders = false; // VK_EXT_mesh_shader
    bool m_supportsSubgroupBallot = false; // Core Vulkan 1.1 subgroup ballot operations
    bool m_supportsRayQuery = false; // VK_KHR_ray_query
    bool m_supportsAccelerationStructure = false; // VK_KHR_acceleration_structure
    uint32_t m_subgroupSize = 0;
    
    // Configuration
    bool m_enableValidation;
};

} // namespace plasma
