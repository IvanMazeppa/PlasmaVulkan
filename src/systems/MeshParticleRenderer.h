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
 * Modern mesh shader-based particle renderer
 * Uses VK_EXT_mesh_shader to generate particle geometry directly on GPU
 * Eliminates CPU→GPU vertex data transfer entirely!
 */
class MeshParticleRenderer {
public:
    // Push constants for mesh shader
    struct MeshPushConstants {
        glm::mat4 viewProj;
        glm::vec3 cameraPos;
        float particleSize;
        uint32_t particleCount;
        float time;
        float _padding1;
        float _padding2;
    };
    
    struct SPHMeshPushConstants {
        glm::mat4 viewProj;
        glm::vec3 cameraPos;
        float particleSize;
        uint32_t particleCount;
        float time;
        // SPH parameters
        float smoothingRadius;
        float restDensity;
        float pressureConstant;
        float viscosity;
        float mass;
    };

    MeshParticleRenderer(VulkanContext* context);
    ~MeshParticleRenderer();
    
    // Check if mesh shaders are available
    bool isSupported() const;
    
    // Render particles using mesh shaders (no vertex buffers needed!)
    void render(VkCommandBuffer cmd, const glm::mat4& viewProj, const glm::vec3& cameraPos,
                VkBuffer particleBuffer, uint32_t particleCount, float particleSize, float time);
    
    // Render with SPH physics mesh shader
    void renderSPH(VkCommandBuffer cmd, const glm::mat4& viewProj, const glm::vec3& cameraPos,
                   VkBuffer particleBuffer, uint32_t particleCount, float particleSize, float time,
                   float smoothingRadius = 2.0f, float restDensity = 1000.0f, 
                   float pressureConstant = 200.0f, float viscosity = 0.1f);
    
    // Get pipeline for external binding
    VkPipeline getPipeline() const { return m_pipeline; }
    VkPipelineLayout getPipelineLayout() const { return m_pipelineLayout; }
    VkPipeline getSPHPipeline() const { return m_sphPipeline; }

private:
    void createDescriptorSetLayout();
    void createPipelineLayout();
    void createPipeline();
    void loadShaders();
    void createDescriptorPool();
    void allocateDescriptorSet();
    void cleanup();
    
    // Helper functions
    VkShaderModule createShaderModule(const std::vector<char>& code);
    std::vector<char> readFile(const std::string& filename);
    
    VulkanContext* m_context;
    bool m_isSupported;
    
    // Pipeline objects
    VkDescriptorSetLayout m_descriptorSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_pipeline = VK_NULL_HANDLE;
    VkPipeline m_sphPipeline = VK_NULL_HANDLE;  // SPH mesh shader pipeline
    VkPipelineLayout m_sphPipelineLayout = VK_NULL_HANDLE;  // SPH pipeline layout
    
    // Descriptor management
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet m_descriptorSet = VK_NULL_HANDLE;
    
    // Shader modules
    VkShaderModule m_meshShader = VK_NULL_HANDLE;
    VkShaderModule m_sphMeshShader = VK_NULL_HANDLE;  // SPH mesh shader
    VkShaderModule m_fragShader = VK_NULL_HANDLE;
    
    // Mesh shader properties
    uint32_t m_maxMeshWorkGroupInvocations = 0;
    uint32_t m_maxMeshWorkGroupSizeX = 0;
    uint32_t m_maxMeshOutputVertices = 0;
    uint32_t m_maxMeshOutputPrimitives = 0;

    // Job 1002: Acceleration structures for RT occluders
    VkAccelerationStructureKHR m_sphereBLAS = VK_NULL_HANDLE;
    VkAccelerationStructureKHR m_discBLAS = VK_NULL_HANDLE;
    VkAccelerationStructureKHR m_topLevelAS = VK_NULL_HANDLE;

    VkBuffer m_sphereVertexBuffer = VK_NULL_HANDLE;
    VkBuffer m_sphereIndexBuffer = VK_NULL_HANDLE;
    VkBuffer m_discVertexBuffer = VK_NULL_HANDLE;
    VkBuffer m_discIndexBuffer = VK_NULL_HANDLE;

    VkBuffer m_sphereBLASBuffer = VK_NULL_HANDLE;
    VkBuffer m_discBLASBuffer = VK_NULL_HANDLE;
    VkBuffer m_topLevelASBuffer = VK_NULL_HANDLE;
    VkBuffer m_instancesBuffer = VK_NULL_HANDLE;

    void createAccelerationStructures();
    void createOccluderGeometry();
    void buildBLAS();
    void buildTLAS();
    void cleanupAccelerationStructures();
};

} // namespace plasma