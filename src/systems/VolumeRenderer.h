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
        glm::vec3 gridOrigin = glm::vec3(-25.0f, -25.0f, -25.0f);  // Expanded from 30³ to 50³ world space  
        float voxelSize = 0.25f;                   // Keep fine voxels for detail
        glm::uvec3 gridDimensions = glm::uvec3(200, 200, 200);  // Actually expand grid resolution
        float splatRadius = 1.0f;                  
        uint32_t maxRaySteps = 64;                 // Reasonable ray steps for performance
        float rayStepSize = 0.5f;                  // Proper step size (~2x voxel size)
        float densityScale = 0.7f;                 // Slightly reduced to prevent white dominance
        
        // HIGH QUALITY settings for realtime high-quality mode (Shift+V)
        static VolumeParams getHighQuality() {
            VolumeParams params;
            params.gridOrigin = glm::vec3(-25.0f, -25.0f, -25.0f);  // Match expanded bounds
            params.voxelSize = 0.32f;              // Proportionally adjusted detail balance  
            params.gridDimensions = glm::uvec3(156, 156, 156);  // Maintain 1.25x increase ratio
            params.splatRadius = 1.2f;             // Smooth splatting
            params.maxRaySteps = 128;              // Higher quality ray steps  
            params.rayStepSize = 0.25f;            // Fine but reasonable ray marching
            params.densityScale = 0.6f;            // Prevent white dominance
            return params;
        }
        
        // ULTRA-HIGH QUALITY settings for recording-only mode (Ctrl+V)
        static VolumeParams getUltraRecordingQuality() {
            VolumeParams params;
            params.gridOrigin = glm::vec3(-30.0f, -30.0f, -30.0f);  // Maximum coverage for recording  
            params.voxelSize = 0.12f;              // Balanced ultra-fine detail with expanded bounds
            params.gridDimensions = glm::uvec3(500, 500, 500);  // MASSIVE resolution grid
            params.splatRadius = 2.0f;             // Maximum smooth splatting
            params.maxRaySteps = 256;              // High quality ray steps - recording only!
            params.rayStepSize = 0.12f;            // Fine ray marching - recording only!
            params.densityScale = 0.3f;            // Very low for maximum color range
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
        float opacityScale;    // Sigma_t for opacity/absorption
        float emissionScale;   // Emission intensity scaling
        float tempOffset;      // Temperature offset for color shift
        float tempRange;       // Temperature range compression/expansion
        float saturation;      // Color saturation control
    };

    VolumeRenderer(VulkanContext* context, const VolumeParams& params = {});
    ~VolumeRenderer();
    
    // Update runtime parameters from Application
    void setRuntimeParameters(float densityScale, float opacityScale, float stepSize, 
                            float emissionScale, int maxSteps,
                            float tempOffset, float tempRange, float saturation);
    
    // Update quality settings dynamically
    void setRecordingQuality(bool enable);
    
    // Update density grid from particle data
    void updateDensityGrid(VkCommandBuffer cmd, VkBuffer particleBuffer, uint32_t particleCount);
    
    // Generate mip chain for cone-stepped raymarch optimization
    void generateMipChain(VkCommandBuffer cmd);
    
    // Quality levels for volumetric rendering
    enum class QualityLevel {
        Standard,    // V key - normal quality
        High,        // Shift+V - high quality but realtime
        Ultra        // Ctrl+V - ultra quality for recording only
    };
    
    // Render volumetric effect with quality level
    void render(VkCommandBuffer cmd, const glm::mat4& viewProj, const glm::vec3& cameraPos, QualityLevel quality = QualityLevel::Standard);
    
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
    
    // 3D density texture with mip chain
    VkImage m_densityImage = VK_NULL_HANDLE;
    VkDeviceMemory m_densityImageMemory = VK_NULL_HANDLE;
    VkImageView m_densityImageView = VK_NULL_HANDLE;
    VkSampler m_densitySampler = VK_NULL_HANDLE;
    uint32_t m_densityMipLevels = 1;
    
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
    
    // Runtime adjustable parameters
    float m_runtimeDensityScale = 0.2f;    // Lower default to avoid white saturation
    float m_runtimeOpacityScale = 4.0f;  
    float m_runtimeStepSize = 0.02f;
    float m_runtimeEmissionScale = 1.0f;
    int m_runtimeMaxSteps = 512;
    float m_runtimeTempOffset = 0.0f;
    float m_runtimeTempRange = 1.0f;
    float m_runtimeSaturation = 1.0f;
    
    // Helper functions
    uint32_t findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties);
    VkShaderModule createShaderModule(const std::vector<char>& code);
    std::vector<char> readFile(const std::string& filename);
};

} // namespace plasma
