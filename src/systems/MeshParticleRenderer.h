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
        uint32_t rtEnabled;  // Job 1003: RT toggle
        // Job 1005: Directional light state
        glm::vec3 lightDirection;
        float lightIntensity;
        uint32_t occlusionAmplify;  // Debug toggle for occlusion amplification
        // CR 1018: Ray query mask and overlay controls
        uint32_t rtSelfShadowOverlay; // Debug overlay mode
        uint32_t rtCullMaskMode;      // 0=both, 1=external only, 2=shells only
        float _padding[1];  // Align to 16-byte boundary
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
                VkBuffer particleBuffer, uint32_t particleCount, float particleSize, float time,
                class VolumeRenderer* volumeRenderer = nullptr);  // Job 1014: Shell TLAS access
    
    // Render with SPH physics mesh shader
    void renderSPH(VkCommandBuffer cmd, const glm::mat4& viewProj, const glm::vec3& cameraPos,
                   VkBuffer particleBuffer, uint32_t particleCount, float particleSize, float time,
                   float smoothingRadius = 2.0f, float restDensity = 1000.0f, 
                   float pressureConstant = 200.0f, float viscosity = 0.1f);
    
    // Get pipeline for external binding
    VkPipeline getPipeline() const { return m_pipeline; }
    VkPipelineLayout getPipelineLayout() const { return m_pipelineLayout; }
    VkPipeline getSPHPipeline() const { return m_sphPipeline; }

    // Job 1005: Light control methods
    void adjustLightDirection(float deltaTheta, float deltaPhi);
    void adjustLightIntensity(float delta);
    void toggleOcclusionAmplify();
    glm::vec3 getLightDirection() const { return m_lightDirection; }
    float getLightIntensity() const { return m_lightIntensity; }
    bool getOcclusionAmplifyEnabled() const { return m_occlusionAmplifyEnabled; }

    // Job 1006: Bounds visualization methods
    void toggleBoundsOverlay();
    bool getBoundsOverlayEnabled() const { return m_showBounds; }

    // CR 1018: Ray query mask and overlay control methods
    void toggleRayQueryOverlay();
    void cycleCullMaskMode();
    bool getRayQueryOverlayEnabled() const { return m_rtSelfShadowOverlay; }
    uint32_t getCullMaskMode() const { return m_rtCullMaskMode; }

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

    // Job 1004: Device memory for buffers (since VMA is disabled)
    VkDeviceMemory m_sphereVertexMemory = VK_NULL_HANDLE;
    VkDeviceMemory m_sphereIndexMemory = VK_NULL_HANDLE;
    VkDeviceMemory m_discVertexMemory = VK_NULL_HANDLE;
    VkDeviceMemory m_discIndexMemory = VK_NULL_HANDLE;
    VkDeviceMemory m_sphereBLASMemory = VK_NULL_HANDLE;
    VkDeviceMemory m_discBLASMemory = VK_NULL_HANDLE;
    VkDeviceMemory m_topLevelASMemory = VK_NULL_HANDLE;
    VkDeviceMemory m_instancesMemory = VK_NULL_HANDLE;

    // Job 1004: Geometry data for AS building
    uint32_t m_sphereVertexCount = 0;
    uint32_t m_sphereIndexCount = 0;
    uint32_t m_discVertexCount = 0;
    uint32_t m_discIndexCount = 0;
    VkDeviceAddress m_sphereVertexAddress = 0;
    VkDeviceAddress m_sphereIndexAddress = 0;
    VkDeviceAddress m_discVertexAddress = 0;
    VkDeviceAddress m_discIndexAddress = 0;

    void createAccelerationStructures();
    void createOccluderGeometry();
    void buildBLAS();
    void buildTLAS();
    void cleanupAccelerationStructures();

    // Job 1004: Geometry generation helpers
    std::pair<std::vector<glm::vec3>, std::vector<uint32_t>> generateIcosphere(float radius, int subdivisions);
    std::pair<std::vector<glm::vec3>, std::vector<uint32_t>> generateDisc(float innerRadius, float outerRadius, int segments);
    void createBufferWithData(VkDevice device, const void* data, VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer& buffer, VkDeviceMemory& memory);
    void createBufferForAS(VkDevice device, VkDeviceSize size, VkBuffer& buffer, VkDeviceMemory& memory);
    VkCommandBuffer beginSingleTimeCommands();
    void endSingleTimeCommands(VkCommandBuffer commandBuffer);

    // Job 1006: AABB structure for bounds visualization
    struct AABB {
        glm::vec3 min, max;
    };

    // Job 1006: AABB computation for bounds visualization
    void computeWorldAABBs();
    AABB transformAABB(const AABB& aabb, const glm::mat4& transform);
    void renderBoundsOverlay(VkCommandBuffer cmd, const glm::mat4& viewProj);
    std::vector<glm::vec3> generateAABBWireframe(const AABB& aabb);

    // Job 1003: RT control
    bool m_rtShadowsEnabled = true;  // Runtime RT toggle

    // Job 1005: Light control state
    glm::vec3 m_lightDirection = glm::normalize(glm::vec3(-0.5f, -0.8f, -0.6f));
    float m_lightIntensity = 1.0f;
    bool m_occlusionAmplifyEnabled = false;

    // Job 1006: Bounds visualization
    bool m_showBounds = false;
    AABB m_sphereAABB, m_discAABB;
};

} // namespace plasma