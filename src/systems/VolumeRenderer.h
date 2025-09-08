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
        glm::vec3 gridOrigin = glm::vec3(-15.0f, -15.0f, -15.0f);
        float voxelSize = 0.25f;                   // Much finer voxels for high detail
        glm::uvec3 gridDimensions = glm::uvec3(120, 120, 120);  // 8x increase from 50³ to 120³
        float splatRadius = 1.0f;                  
        uint32_t maxRaySteps = 128;                // Double the ray steps
        float rayStepSize = 0.2f;                  // Finer ray steps
        float densityScale = 0.7f;                 // Slightly reduced to prevent white dominance
        
        // Ultra-high quality settings for recording mode
        static VolumeParams getRecordingQuality() {
            VolumeParams params;
            params.gridOrigin = glm::vec3(-18.0f, -18.0f, -18.0f);  // Larger coverage
            params.voxelSize = 0.12f;              // 2x smaller than standard for ultra-fine detail
            params.gridDimensions = glm::uvec3(300, 300, 300);  // 27M voxels - cinematic quality!
            params.splatRadius = 1.5f;             // Smoother splatting for cinematic look
            params.maxRaySteps = 256;              // 2x more ray steps than standard
            params.rayStepSize = 0.1f;             // Very fine ray marching
            params.densityScale = 0.5f;            // Lower to maintain color range at high resolution
            return params;
        }
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
    
    // Update quality settings dynamically
    void setRecordingQuality(bool enable);
    
    // Update density grid from particle data
    void updateDensityGrid(VkCommandBuffer cmd, VkBuffer particleBuffer, uint32_t particleCount);
    
    // Render volumetric effect with optional quality override
    void render(VkCommandBuffer cmd, const glm::mat4& viewProj, const glm::vec3& cameraPos, bool highQuality = false);
    
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
    
    // Mode selection
    bool m_useAtomicScatter = false; // use per-particle atomic image adds when available
    
    // Helper functions
    uint32_t findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties);
    VkShaderModule createShaderModule(const std::vector<char>& code);
    std::vector<char> readFile(const std::string& filename);
};

} // namespace plasma
