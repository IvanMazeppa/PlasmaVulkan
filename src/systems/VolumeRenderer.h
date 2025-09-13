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
        glm::vec3 gridOrigin = glm::vec3(-30.0f, -30.0f, -30.0f);  // Expanded from 50³ to 60³ world space for better particle spread
        float voxelSize = 0.25f;                   // Keep fine voxels for detail
        glm::uvec3 gridDimensions = glm::uvec3(240, 240, 240);  // Expand grid resolution proportionally (60/0.25 = 240)
        float splatRadius = 1.0f;                  
        uint32_t maxRaySteps = 64;                 // Reasonable ray steps for performance
        float rayStepSize = 0.5f;                  // Proper step size (~2x voxel size)
        float densityScale = 0.7f;                 // Slightly reduced to prevent white dominance
        
        // HIGH QUALITY settings for realtime high-quality mode (Shift+V)
        static VolumeParams getHighQuality() {
            VolumeParams params;
            params.gridOrigin = glm::vec3(-30.0f, -30.0f, -30.0f);  // Match expanded bounds
            params.voxelSize = 0.32f;              // Proportionally adjusted detail balance  
            params.gridDimensions = glm::uvec3(188, 188, 188);  // Proportionally adjusted (60/0.32 ≈ 188)
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
        uint32_t frameIndex;   // For STBN layer selection  
        glm::vec2 cpOffset;     // Cranley-Patterson offset for temporal jitter
        float _padding2;
    };

    VolumeRenderer(VulkanContext* context, const VolumeParams& params = {});
    ~VolumeRenderer();
    
    // Update runtime parameters from Application
    void setRuntimeParameters(float densityScale, float opacityScale, float stepSize, 
                            float emissionScale, int maxSteps,
                            float tempOffset, float tempRange, float saturation);
    
    // Update TAA parameters
    void setTAAParameters(float blendFactor);
    
    // Update volume parameters (may recreate grid if voxel size changes)
    void setVolumeDetailParameters(float voxelSize);
    
    // Update quality settings dynamically
    void setRecordingQuality(bool enable);
    
    // Handle swapchain resize - recreate TAA resources
    void onSwapchainResized(VkExtent2D newExtent);
    
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
    
    // Render volumetric effect to intermediate TAA current frame target
    void renderToTAATarget(VkCommandBuffer cmd, const glm::mat4& viewProj, const glm::vec3& cameraPos, QualityLevel quality = QualityLevel::Standard);
    
    // Render TAA pass for temporal noise smoothing (call after volume rendering)
    void renderTAA(VkCommandBuffer cmd, const glm::mat4& viewProj, VkImageView currentFrameView);
    
    // Update TAA history buffer with current frame (call after TAA pass)
    void updateTAAHistory(VkCommandBuffer cmd);
    
    // Update view-projection matrix for next frame's reprojection (call after TAA processing)
    void updateTAAMatrix(const glm::mat4& viewProj);
    
    // Composite TAA result to main framebuffer as fullscreen quad
    void compositeTAAResult(VkCommandBuffer cmd);
    
    // Get TAA current frame image view for final TAA pass
    VkImageView getTAACurrentImageView() const { return m_taaCurrentImageView; }
    
    // Parameter controls
    void setVolumeParams(const VolumeParams& params) { m_params = params; }
    VolumeParams getVolumeParams() const { return m_params; }
    
    // First-frame crash prevention
    bool needsInitialUpdate() const { return !m_densityInitialized; }

private:
    void createDensityGrid();
    void createDensitySplatPipeline();
    void createVolumeRenderPipeline();
    void createTAAPipeline();
    void createTAAResources();     // TAA history buffer and pipeline
    void createDescriptorSets();
    void createSTBNTexture();
    void createOpticalDepthLUT();  // Preintegrated Beer-Lambert LUT
    void cleanupTAAResources();    // Clean up TAA resources for resize
    void recreateDensityGrid();    // Recreate density grid with new parameters
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
    
    // TAA (Temporal Anti-Aliasing) pipeline for noise smoothing
    VkPipeline m_taaPipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_taaPipelineLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_taaDescriptorSetLayout = VK_NULL_HANDLE;
    
    // STBN (Spatiotemporal Blue Noise) texture array for jittering
    VkImage m_stbnImage = VK_NULL_HANDLE;
    VkDeviceMemory m_stbnMemory = VK_NULL_HANDLE;
    VkImageView m_stbnImageView = VK_NULL_HANDLE;
    VkSampler m_stbnSampler = VK_NULL_HANDLE;
    static constexpr uint32_t STBN_SIZE = 128;
    static constexpr uint32_t STBN_LAYERS = 64;
    uint32_t m_frameIndex = 0;  // For temporal rotation
    
    // Preintegrated optical depth LUT for Beer-Lambert smoothing
    VkImage m_opticalDepthLUT = VK_NULL_HANDLE;
    VkDeviceMemory m_opticalDepthLUTMemory = VK_NULL_HANDLE;
    VkImageView m_opticalDepthLUTView = VK_NULL_HANDLE;
    VkSampler m_opticalDepthLUTSampler = VK_NULL_HANDLE;
    static constexpr uint32_t OPTICAL_DEPTH_LUT_SIZE = 256;  // 256 entries for smooth interpolation
    
    // TAA (Temporal Anti-Aliasing) history buffer for noise smoothing
    VkImage m_taaHistoryImage = VK_NULL_HANDLE;
    VkDeviceMemory m_taaHistoryMemory = VK_NULL_HANDLE;
    VkImageView m_taaHistoryImageView = VK_NULL_HANDLE;
    VkSampler m_taaHistorySampler = VK_NULL_HANDLE;
    
    // TAA current frame target (intermediate render target)
    VkImage m_taaCurrentImage = VK_NULL_HANDLE;
    VkDeviceMemory m_taaCurrentMemory = VK_NULL_HANDLE;
    VkImageView m_taaCurrentImageView = VK_NULL_HANDLE;
    VkSampler m_taaCurrentSampler = VK_NULL_HANDLE;
    
    glm::mat4 m_previousViewProjMatrix = glm::mat4(1.0f);  // For reprojection
    bool m_taaFirstFrame = true;  // Track first frame for history initialization
    
    // Shader modules
    VkShaderModule m_densitySplatShader = VK_NULL_HANDLE;
    VkShaderModule m_volumeVertShader = VK_NULL_HANDLE;
    VkShaderModule m_volumeFragShader = VK_NULL_HANDLE;
    VkShaderModule m_taaVertShader = VK_NULL_HANDLE;
    VkShaderModule m_taaFragShader = VK_NULL_HANDLE;
    
    // Mode selection
    bool m_useAtomicScatter = false; // use per-particle atomic image adds when available
    
    // Runtime adjustable parameters (tuned for optical-depth adaptive stepping)
    float m_runtimeDensityScale = 0.4f;    // Higher for better base visibility
    float m_runtimeOpacityScale = 8.0f;    // Higher absorption to balance emission
    float m_runtimeStepSize = 0.04f;       // Larger steps work better with adaptive stepping
    float m_runtimeEmissionScale = 1.5f;   // Boost emission to compensate for reduced base
    int m_runtimeMaxSteps = 128;           // Lower max steps since adaptive stepping is more efficient
    float m_runtimeTempOffset = 0.0f;
    float m_runtimeTempRange = 1.0f;
    float m_runtimeSaturation = 1.0f;
    
    // TAA parameters
    float m_taaBlendFactor = 0.1f;  // Blend factor for temporal accumulation
    VkExtent2D m_taaExtent = {0, 0}; // Current TAA image extent
    
    // First-frame crash prevention
    bool m_densityInitialized = false;    // Track if density grid has been updated at least once
    
    // Helper functions
    uint32_t findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties);
    VkShaderModule createShaderModule(const std::vector<char>& code);
    std::vector<char> readFile(const std::string& filename);
};

} // namespace plasma
