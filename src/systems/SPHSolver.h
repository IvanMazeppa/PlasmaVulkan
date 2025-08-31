#pragma once

#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
#include <memory>
#include <vector>

namespace plasma {

class VulkanContext;

// SPH (Smoothed Particle Hydrodynamics) parameters
struct SPHParams {
    float smoothingRadius = 0.5f;      // h - kernel radius
    float restDensity = 1000.0f;       // ρ₀ - water density kg/m³
    float pressureConstant = 200.0f;   // k - gas constant
    float viscosity = 0.01f;           // μ - dynamic viscosity
    float mass = 0.02f;                // particle mass
    float surfaceTension = 0.0728f;    // σ - surface tension N/m
    float _padding1 = 0.0f;
    float _padding2 = 0.0f;
};

// Grid parameters for spatial hashing
struct GridParams {
    glm::vec3 gridOrigin = glm::vec3(-10.0f);
    float cellSize = 1.0f;  // Usually 2 * smoothing radius
    glm::uvec3 gridDimensions = glm::uvec3(32, 32, 32);
    uint32_t maxNeighbors = 64;
};

class SPHSolver {
public:
    SPHSolver(VulkanContext* context, uint32_t maxParticles);
    ~SPHSolver();

    // Update SPH simulation
    void update(VkCommandBuffer cmd, VkBuffer particleBuffer, uint32_t particleCount, float deltaTime);
    
    // Set SPH parameters
    void setParams(const SPHParams& params) { m_sphParams = params; updateParamsBuffer(); }
    SPHParams getParams() const { return m_sphParams; }
    
    // Set grid parameters
    void setGridParams(const GridParams& params) { m_gridParams = params; }
    GridParams getGridParams() const { return m_gridParams; }
    
    // Get/Set specific parameters
    void setSmoothingRadius(float radius);
    void setRestDensity(float density);
    void setPressureConstant(float k);
    void setViscosity(float viscosity);
    void setSurfaceTension(float tension);

private:
    void createComputePipelines();
    void createBuffers();
    void createDescriptorSets();
    void updateParamsBuffer();
    
    // Dispatch compute shaders
    void dispatchSpatialHashing(VkCommandBuffer cmd, VkBuffer particleBuffer, uint32_t particleCount);
    void dispatchSPHForces(VkCommandBuffer cmd, VkBuffer particleBuffer, uint32_t particleCount, float deltaTime);
    
    VulkanContext* m_context;
    uint32_t m_maxParticles;
    
    // SPH parameters
    SPHParams m_sphParams;
    GridParams m_gridParams;
    
    // Vulkan resources
    VkBuffer m_sphParamsBuffer = VK_NULL_HANDLE;
    VkDeviceMemory m_sphParamsMemory = VK_NULL_HANDLE;
    
    VkBuffer m_gridBuffer = VK_NULL_HANDLE;
    VkDeviceMemory m_gridMemory = VK_NULL_HANDLE;
    
    VkBuffer m_indexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory m_indexMemory = VK_NULL_HANDLE;
    
    VkBuffer m_neighborBuffer = VK_NULL_HANDLE;
    VkDeviceMemory m_neighborMemory = VK_NULL_HANDLE;
    
    // Compute pipelines
    VkPipeline m_spatialHashPipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_spatialHashPipelineLayout = VK_NULL_HANDLE;
    
    VkPipeline m_sphPipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_sphPipelineLayout = VK_NULL_HANDLE;
    
    // Descriptor sets
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_spatialHashDescriptorSetLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_sphDescriptorSetLayout = VK_NULL_HANDLE;
    VkDescriptorSet m_spatialHashDescriptorSet = VK_NULL_HANDLE;
    VkDescriptorSet m_sphDescriptorSet = VK_NULL_HANDLE;
    
    // Shader modules
    VkShaderModule m_spatialHashShader = VK_NULL_HANDLE;
    VkShaderModule m_sphShader = VK_NULL_HANDLE;
};

} // namespace plasma