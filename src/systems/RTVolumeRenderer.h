#pragma once

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <volk.h>
#include <memory>
#include <string>
#include <glm/glm.hpp>

namespace plasma { class VulkanContext; } // Forward declaration

class RTVolumeRenderer {
public:
    // Volume grid parameters
    struct VolumeParams {
        glm::uvec3 gridDimensions = glm::uvec3(224, 224, 224);  // 224³ for good balance
        glm::vec3 gridOrigin = glm::vec3(-30.0f, -30.0f, -30.0f);
        float voxelSize = 0.27f;  // 60 / 224 ≈ 0.27
        float splatRadius = 1.5f;
    };

    // Push constants for density splatting
    struct DensityPushConstants {
        glm::vec3 gridOrigin;
        float voxelSize;
        glm::uvec3 gridDimensions;
        uint32_t particleCount;
        float splatRadius;
        float _padding1;
        float _padding2;
        float _padding3;
    };

    explicit RTVolumeRenderer(plasma::VulkanContext* context);
    ~RTVolumeRenderer();

    // Core rendering functions
    void updateDensityGrid(VkCommandBuffer cmd, VkBuffer particleBuffer, uint32_t particleCount);
    void generateMipChain(VkCommandBuffer cmd);
    void renderToHDR(VkCommandBuffer cmd, const glm::mat4& viewProj, const glm::vec3& cameraPos);
    void composite(VkCommandBuffer cmd);

    // Resource management
    void onSwapchainResized(const VkExtent2D& newExtent);

private:
    plasma::VulkanContext* m_context;
    VkExtent2D m_extent;
    VolumeParams m_params;

    // 3D density grid resources
    VkImage m_densityImage = VK_NULL_HANDLE;
    VkDeviceMemory m_densityMemory = VK_NULL_HANDLE;
    VkImageView m_densityImageView = VK_NULL_HANDLE;
    VkSampler m_densitySampler = VK_NULL_HANDLE;
    uint32_t m_densityMipLevels = 1;

    // Density splatting compute pipeline
    VkPipeline m_densitySplatPipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_densitySplatPipelineLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_densitySplatDescriptorLayout = VK_NULL_HANDLE;
    VkShaderModule m_densitySplatShader = VK_NULL_HANDLE;

    // HDR target resources
    VkImage m_hdrImage = VK_NULL_HANDLE;
    VkDeviceMemory m_hdrMemory = VK_NULL_HANDLE;
    VkImageView m_hdrImageView = VK_NULL_HANDLE;
    VkSampler m_hdrSampler = VK_NULL_HANDLE;

    // Composite pass resources
    VkPipeline m_compositePipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_compositePipelineLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_compositeDescriptorLayout = VK_NULL_HANDLE;
    VkShaderModule m_compositeVertShader = VK_NULL_HANDLE;
    VkShaderModule m_compositeFragShader = VK_NULL_HANDLE;

    // Helper functions
    void createDensityGrid();
    void createDensitySplatPipeline();
    void createHDRTarget();
    void createCompositePass();
    void createShaderModule(const std::string& filename, VkShaderModule& shaderModule);
    uint32_t findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties);
    void cleanup();
};