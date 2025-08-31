#pragma once

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <volk.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <memory>
#include <vector>
#include <string>

namespace plasma {

class VulkanContext;

/**
 * Volumetric renderer for particle-based plasma effects
 * Converts particle data to 3D density grid and renders with ray marching
 */
class VolumeRenderer {
public:
    // Volume grid parameters
    struct VolumeParams {
        glm::vec3 gridOrigin = glm::vec3(-10.0f, -10.0f, -10.0f);
        float voxelSize = 0.5f;                    // Size of each voxel (larger = less detail, better performance)
        glm::uvec3 gridDimensions = glm::uvec3(32, 32, 32);  // 32³ grid for better performance
        float splatRadius = 1.2f;                  // Particle influence radius (smaller = sharper)
        uint32_t maxRaySteps = 32;                 // Ray marching steps (minimal for performance)
        float rayStepSize = 0.6f;                  // Step size for ray marching (larger = faster)
        float densityScale = 0.8f;                 // Density visualization scale (balanced)
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
    
    // Push constants for ray marching
    struct VolumePushConstants {
        glm::mat4 viewProjInv;
        glm::vec3 cameraPos;
        float _padding1;
        glm::vec3 gridOrigin;
        float voxelSize;
        glm::uvec3 gridDimensions;
        uint32_t maxSteps;
        float stepSize;
        float densityScale;
        float _padding2;
        float _padding3;
    };

    VolumeRenderer(VulkanContext* context, const VolumeParams& params = {});
    ~VolumeRenderer();
    
    // Update density grid from particle data
    void updateDensityGrid(VkCommandBuffer cmd, VkBuffer particleBuffer, uint32_t particleCount);
    
    // Render volumetric effect
    void render(VkCommandBuffer cmd, const glm::mat4& viewProj, const glm::vec3& cameraPos);
    
    // Parameter controls
    void setVolumeParams(const VolumeParams& params) { m_params = params; }
    VolumeParams getVolumeParams() const { return m_params; }

private:
    void createDensityGrid();
    void createDensitySplatPipeline();
    void createVolumeRenderPipeline();
    void createDescriptorSets();
    void cleanup();
    
    VulkanContext* m_context;
    VolumeParams m_params;
    
    // 3D density texture
    VkImage m_densityImage = VK_NULL_HANDLE;
    VkDeviceMemory m_densityImageMemory = VK_NULL_HANDLE;
    VkImageView m_densityImageView = VK_NULL_HANDLE;
    VkSampler m_densitySampler = VK_NULL_HANDLE;
    
    // Density splatting compute pipeline
    VkPipeline m_densitySplatPipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_densitySplatPipelineLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_densitySplatDescriptorSetLayout = VK_NULL_HANDLE;
    
    // Volume rendering graphics pipeline  
    VkPipeline m_volumePipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_volumePipelineLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_volumeDescriptorSetLayout = VK_NULL_HANDLE;
    
    // Shader modules
    VkShaderModule m_densitySplatShader = VK_NULL_HANDLE;
    VkShaderModule m_volumeVertShader = VK_NULL_HANDLE;
    VkShaderModule m_volumeFragShader = VK_NULL_HANDLE;
    
    // Helper functions
    uint32_t findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties);
    VkShaderModule createShaderModule(const std::vector<char>& code);
    std::vector<char> readFile(const std::string& filename);
};

} // namespace plasma