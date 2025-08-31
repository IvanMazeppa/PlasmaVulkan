#pragma once

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <volk.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <vector>
#include <string>
#include <fstream>

namespace plasma {

class VulkanContext;

class ParticleSystem {
public:
    // Particle structure matching compute shader
    struct Particle {
        glm::vec3 position;
        float temperature;
        glm::vec3 velocity;
        float density;
    };
    
    // Push constants for compute shader
    struct ComputePushConstants {
        float deltaTime;
        float time;
        uint32_t particleCount;
        float gravityStrength;     // Runtime controllable gravity
        glm::vec3 gravityCenter;   // Center of gravity attraction
        float turbulenceStrength;  // Noise/chaos factor
        float dampingFactor;       // Velocity damping
        uint32_t constraintShape;  // 0=NONE, 1=SPHERE, 2=DISC, 3=TORUS
        float constraintRadius;    // Main radius for all shapes
        float constraintThickness; // For disc thickness, torus tube radius
        float _padding[1];
    };
    
    // Push constants for SPH shader
    struct SPHPushConstants {
        float deltaTime;
        float time;
        uint32_t particleCount;
        float smoothingRadius;    // h
        float restDensity;        // ρ₀  
        float pressureConstant;   // k
        float viscosity;          // μ
        float mass;               // m
    };
    
    // Push constants for graphics shader
    struct GraphicsPushConstants {
        glm::mat4 viewProj;
        float particleSize;
        float _padding[3];
    };
    
    ParticleSystem(VulkanContext* context, uint32_t particleCount = 100000);
    ~ParticleSystem();
    
    // Update particles with compute shader
    void update(VkCommandBuffer commandBuffer, float deltaTime, float time);
    
    // Render particles
    void render(VkCommandBuffer commandBuffer, const glm::mat4& viewProj);
    
    // SPH controls
    void setSPHMode(bool enabled) { m_sphMode = enabled; }
    bool isSPHMode() const { return m_sphMode; }
    
    // SPH parameter controls
    void setSPHParameters(float smoothingRadius, float restDensity, 
                         float pressureConstant, float viscosity, float mass);
    
    // Performance controls
    void setActiveParticleCount(uint32_t count);
    uint32_t getActiveParticleCount() const { return m_activeParticleCount; }
    uint32_t getParticleCount() const { return m_particleCount; }
    
    // Buffer access for volumetric rendering
    VkBuffer getParticleBuffer() const { return m_particleBuffer; }
    
    // Physics parameter controls
    void setGravityStrength(float strength) { m_gravityStrength = strength; }
    float getGravityStrength() const { return m_gravityStrength; }
    
    void setGravityCenter(const glm::vec3& center) { m_gravityCenter = center; }
    glm::vec3 getGravityCenter() const { return m_gravityCenter; }
    
    void setTurbulenceStrength(float strength) { m_turbulenceStrength = strength; }
    float getTurbulenceStrength() const { return m_turbulenceStrength; }
    
    void setDampingFactor(float damping) { m_dampingFactor = damping; }
    float getDampingFactor() const { return m_dampingFactor; }
    
    // Constraint controls
    void setConstraintShape(uint32_t shape) { m_constraintShape = shape; }
    uint32_t getConstraintShape() const { return m_constraintShape; }
    
    void setConstraintRadius(float radius) { m_constraintRadius = radius; }
    float getConstraintRadius() const { return m_constraintRadius; }
    
    void setConstraintThickness(float thickness) { m_constraintThickness = thickness; }
    float getConstraintThickness() const { return m_constraintThickness; }
    
private:
    void createParticleBuffer();
    void createComputePipeline();
    void createSPHPipeline();
    void createGraphicsPipeline();
    void createDescriptorSets();
    void initializeParticles();
    void cleanup();
    
    // Helper functions
    uint32_t findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties);
    VkShaderModule createShaderModule(const std::vector<char>& code);
    std::vector<char> readFile(const std::string& filename);
    
    VulkanContext* m_context;
    uint32_t m_particleCount;          // Total allocated particles
    uint32_t m_activeParticleCount;    // Currently active particles
    
    // Vulkan resources
    VkBuffer m_particleBuffer = VK_NULL_HANDLE;
    VkDeviceMemory m_particleBufferMemory = VK_NULL_HANDLE;
    
    // Compute pipeline
    VkPipeline m_computePipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_computePipelineLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_computeDescriptorSetLayout = VK_NULL_HANDLE;
    VkDescriptorSet m_computeDescriptorSet = VK_NULL_HANDLE;
    
    // Graphics pipeline
    VkPipeline m_graphicsPipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_graphicsPipelineLayout = VK_NULL_HANDLE;
    
    // Descriptor pool
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
    
    // SPH mode and parameters
    bool m_sphMode = false;
    SPHPushConstants m_sphParams = {
        0.0f,    // deltaTime
        0.0f,    // time  
        0,       // particleCount
        0.5f,    // smoothingRadius
        1000.0f, // restDensity
        200.0f,  // pressureConstant
        0.01f,   // viscosity
        0.02f    // mass
    };
    
    // SPH compute pipeline
    VkPipeline m_sphPipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_sphPipelineLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_sphDescriptorSetLayout = VK_NULL_HANDLE;
    VkDescriptorSet m_sphDescriptorSet = VK_NULL_HANDLE;
    
    // Runtime physics parameters
    float m_gravityStrength = 0.6f;              // Default gravity strength
    glm::vec3 m_gravityCenter = {0.0f, 0.0f, 0.0f}; // Center of attraction
    float m_turbulenceStrength = 0.0f;           // Chaos/randomness factor
    float m_dampingFactor = 0.999f;              // Velocity damping (1.0 = no damping)
    
    // Constraint parameters
    uint32_t m_constraintShape = 0;              // 0=NONE, 1=SPHERE, 2=DISC, 3=TORUS
    float m_constraintRadius = 15.0f;            // Main radius for all shapes
    float m_constraintThickness = 3.0f;          // For disc thickness, torus tube radius
};

} // namespace plasma