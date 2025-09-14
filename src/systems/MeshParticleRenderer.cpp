#include "MeshParticleRenderer.h"
#include "../renderer/VulkanContext.h"
#include <iostream>
#include <fstream>
#include <stdexcept>
#include <array>
#include <chrono>

namespace plasma {

MeshParticleRenderer::MeshParticleRenderer(VulkanContext* context) 
    : m_context(context), m_isSupported(false) {
    
    // Check if mesh shaders are supported
    m_isSupported = m_context->supportsMeshShaders();
    
    if (!m_isSupported) {
        std::cout << "Mesh shaders not supported - falling back to traditional rendering" << std::endl;
        return;
    }
    
    std::cout << "Creating mesh shader particle renderer..." << std::endl;
    
    try {
        // Query mesh shader properties
        VkPhysicalDeviceMeshShaderPropertiesEXT meshShaderProps{};
        meshShaderProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_PROPERTIES_EXT;
        
        VkPhysicalDeviceProperties2 deviceProps{};
        deviceProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        deviceProps.pNext = &meshShaderProps;
        
        vkGetPhysicalDeviceProperties2(m_context->getPhysicalDevice(), &deviceProps);
        
        m_maxMeshWorkGroupInvocations = meshShaderProps.maxMeshWorkGroupInvocations;
        m_maxMeshWorkGroupSizeX = meshShaderProps.maxMeshWorkGroupSize[0];
        m_maxMeshOutputVertices = meshShaderProps.maxMeshOutputVertices;
        m_maxMeshOutputPrimitives = meshShaderProps.maxMeshOutputPrimitives;
        
        std::cout << "Mesh shader properties:" << std::endl;
        std::cout << "  Max work group invocations: " << m_maxMeshWorkGroupInvocations << std::endl;
        std::cout << "  Max work group size X: " << m_maxMeshWorkGroupSizeX << std::endl;
        std::cout << "  Max output vertices: " << m_maxMeshOutputVertices << std::endl;
        std::cout << "  Max output primitives: " << m_maxMeshOutputPrimitives << std::endl;
        
        createDescriptorSetLayout();
        createPipelineLayout();
        loadShaders();
        createPipeline();
        createDescriptorPool();
        allocateDescriptorSet();

        // Job 1002: Initialize acceleration structures for RT occluders
        if (m_context->supportsAccelerationStructure() && m_context->supportsRayQuery()) {
            createAccelerationStructures();
        }

        std::cout << "Mesh shader particle renderer created successfully!" << std::endl;
        
    } catch (const std::exception& e) {
        std::cerr << "Failed to create mesh particle renderer: " << e.what() << std::endl;
        m_isSupported = false;
        cleanup();
    }
}

MeshParticleRenderer::~MeshParticleRenderer() {
    cleanup();
}

bool MeshParticleRenderer::isSupported() const {
    return m_isSupported;
}

void MeshParticleRenderer::createDescriptorSetLayout() {
    // Descriptor set layout for particle buffer binding
    VkDescriptorSetLayoutBinding particleBufferBinding{};
    particleBufferBinding.binding = 0;
    particleBufferBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    particleBufferBinding.descriptorCount = 1;
    particleBufferBinding.stageFlags = VK_SHADER_STAGE_MESH_BIT_EXT;
    particleBufferBinding.pImmutableSamplers = nullptr;

    // Job 1003: Binding 3 for RT acceleration structure (when RT enabled)
    bool rtEnabled = m_context->supportsAccelerationStructure() && m_context->supportsRayQuery();
    std::vector<VkDescriptorSetLayoutBinding> bindings = { particleBufferBinding };

    if (rtEnabled) {
        VkDescriptorSetLayoutBinding tlasBinding{};
        tlasBinding.binding = 3;
        tlasBinding.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
        tlasBinding.descriptorCount = 1;
        tlasBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        tlasBinding.pImmutableSamplers = nullptr;
        bindings.push_back(tlasBinding);
    }

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.flags = 0; // Use regular descriptor sets - will push only TLAS via vkCmdPushDescriptorSetKHR
    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutInfo.pBindings = bindings.data();
    
    if (vkCreateDescriptorSetLayout(m_context->getDevice(), &layoutInfo, nullptr, &m_descriptorSetLayout) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create mesh particle descriptor set layout!");
    }
}

void MeshParticleRenderer::createPipelineLayout() {
    // Push constant range for mesh shader parameters
    VkPushConstantRange pushConstantRange{};
    pushConstantRange.stageFlags = VK_SHADER_STAGE_MESH_BIT_EXT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pushConstantRange.offset = 0;
    pushConstantRange.size = sizeof(MeshPushConstants);
    
    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &m_descriptorSetLayout;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushConstantRange;
    
    if (vkCreatePipelineLayout(m_context->getDevice(), &pipelineLayoutInfo, nullptr, &m_pipelineLayout) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create mesh particle pipeline layout!");
    }
}

void MeshParticleRenderer::loadShaders() {
    // Load mesh shader
    auto meshShaderCode = readFile("shaders/particle_mesh.mesh.spv");
    m_meshShader = createShaderModule(meshShaderCode);
    
    // Load SPH mesh shader (if it exists)
    try {
        auto sphMeshShaderCode = readFile("shaders/sph_mesh_experimental.mesh.spv");
        m_sphMeshShader = createShaderModule(sphMeshShaderCode);
        std::cout << "SPH mesh shader loaded successfully!" << std::endl;
    } catch (const std::exception& e) {
        std::cout << "SPH mesh shader not found (will be compiled on first build)" << std::endl;
        m_sphMeshShader = VK_NULL_HANDLE;
    }
    
    // Load fragment shader
    auto fragShaderCode = readFile("shaders/particle_mesh.frag.spv");
    m_fragShader = createShaderModule(fragShaderCode);
}

void MeshParticleRenderer::createDescriptorPool() {
    // Create descriptor pool for mesh shader particle buffer binding + optional RT AS
    bool rtEnabled = m_context->supportsAccelerationStructure() && m_context->supportsRayQuery();

    std::vector<VkDescriptorPoolSize> poolSizes;

    // Storage buffer for particles (always needed)
    VkDescriptorPoolSize storageBufferSize{};
    storageBufferSize.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    storageBufferSize.descriptorCount = 1;
    poolSizes.push_back(storageBufferSize);

    // Acceleration structure (if RT enabled)
    if (rtEnabled) {
        VkDescriptorPoolSize asSize{};
        asSize.type = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
        asSize.descriptorCount = 1;
        poolSizes.push_back(asSize);
    }

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes = poolSizes.data();
    poolInfo.maxSets = 1; // We only need 1 descriptor set
    
    if (vkCreateDescriptorPool(m_context->getDevice(), &poolInfo, nullptr, &m_descriptorPool) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create mesh shader descriptor pool!");
    }
}

void MeshParticleRenderer::allocateDescriptorSet() {
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = m_descriptorPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &m_descriptorSetLayout;
    
    if (vkAllocateDescriptorSets(m_context->getDevice(), &allocInfo, &m_descriptorSet) != VK_SUCCESS) {
        throw std::runtime_error("Failed to allocate mesh shader descriptor set!");
    }
}

void MeshParticleRenderer::createPipeline() {
    // Shader stage creation
    VkPipelineShaderStageCreateInfo meshShaderStageInfo{};
    meshShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    meshShaderStageInfo.stage = VK_SHADER_STAGE_MESH_BIT_EXT;
    meshShaderStageInfo.module = m_meshShader;
    meshShaderStageInfo.pName = "main";
    
    VkPipelineShaderStageCreateInfo fragShaderStageInfo{};
    fragShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    fragShaderStageInfo.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    fragShaderStageInfo.module = m_fragShader;
    fragShaderStageInfo.pName = "main";
    
    std::array<VkPipelineShaderStageCreateInfo, 2> shaderStages = {
        meshShaderStageInfo, fragShaderStageInfo
    };
    
    // No vertex input (mesh shader generates vertices!)
    VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
    vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInputInfo.vertexBindingDescriptionCount = 0;
    vertexInputInfo.vertexAttributeDescriptionCount = 0;
    
    // Input assembly (not used with mesh shaders, but required)
    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    inputAssembly.primitiveRestartEnable = VK_FALSE;
    
    // Viewport state (dynamic)
    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;
    
    // Rasterization
    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.depthClampEnable = VK_FALSE;
    rasterizer.rasterizerDiscardEnable = VK_FALSE;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = VK_CULL_MODE_NONE; // Particles visible from both sides
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer.depthBiasEnable = VK_FALSE;
    
    // Multisampling
    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.sampleShadingEnable = VK_FALSE;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    
    // Color blending (additive for glow effect)
    VkPipelineColorBlendAttachmentState colorBlendAttachment{};
    colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | 
                                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    colorBlendAttachment.blendEnable = VK_TRUE;
    colorBlendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    colorBlendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE; // Additive blending
    colorBlendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
    colorBlendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    colorBlendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    colorBlendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
    
    VkPipelineColorBlendStateCreateInfo colorBlending{};
    colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlending.logicOpEnable = VK_FALSE;
    colorBlending.attachmentCount = 1;
    colorBlending.pAttachments = &colorBlendAttachment;
    
    // Dynamic state
    std::array<VkDynamicState, 2> dynamicStates = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR
    };
    
    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
    dynamicState.pDynamicStates = dynamicStates.data();
    
    // Depth stencil (simple depth testing)
    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = VK_FALSE; // Don't write depth for transparent particles
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;
    depthStencil.depthBoundsTestEnable = VK_FALSE;
    depthStencil.stencilTestEnable = VK_FALSE;
    
    // Rendering info for dynamic rendering
    VkFormat colorFormat = m_context->getSwapChainImageFormat();
    VkPipelineRenderingCreateInfo renderingInfo{};
    renderingInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    renderingInfo.colorAttachmentCount = 1;
    renderingInfo.pColorAttachmentFormats = &colorFormat;
    renderingInfo.depthAttachmentFormat = VK_FORMAT_UNDEFINED; // No depth buffer for now
    
    // Graphics pipeline creation
    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.pNext = &renderingInfo;
    pipelineInfo.stageCount = static_cast<uint32_t>(shaderStages.size());
    pipelineInfo.pStages = shaderStages.data();
    pipelineInfo.pVertexInputState = &vertexInputInfo;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisampling;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pColorBlendState = &colorBlending;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = m_pipelineLayout;
    pipelineInfo.renderPass = VK_NULL_HANDLE; // Using dynamic rendering
    pipelineInfo.subpass = 0;
    
    if (vkCreateGraphicsPipelines(m_context->getDevice(), VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_pipeline) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create mesh particle graphics pipeline!");
    }
    
    // Create SPH mesh shader pipeline if shader is available
    if (m_sphMeshShader != VK_NULL_HANDLE) {
        // Create separate pipeline layout for SPH with different push constants
        VkPushConstantRange sphPushConstantRange{};
        sphPushConstantRange.stageFlags = VK_SHADER_STAGE_MESH_BIT_EXT | VK_SHADER_STAGE_FRAGMENT_BIT;
        sphPushConstantRange.offset = 0;
        sphPushConstantRange.size = sizeof(SPHMeshPushConstants);
        
        VkPipelineLayoutCreateInfo sphPipelineLayoutInfo{};
        sphPipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        sphPipelineLayoutInfo.setLayoutCount = 1;
        sphPipelineLayoutInfo.pSetLayouts = &m_descriptorSetLayout;
        sphPipelineLayoutInfo.pushConstantRangeCount = 1;
        sphPipelineLayoutInfo.pPushConstantRanges = &sphPushConstantRange;
        
        if (vkCreatePipelineLayout(m_context->getDevice(), &sphPipelineLayoutInfo, nullptr, &m_sphPipelineLayout) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create SPH mesh particle pipeline layout!");
        }
        
        // Create SPH shader stages
        VkPipelineShaderStageCreateInfo sphMeshShaderStageInfo{};
        sphMeshShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        sphMeshShaderStageInfo.stage = VK_SHADER_STAGE_MESH_BIT_EXT;
        sphMeshShaderStageInfo.module = m_sphMeshShader;
        sphMeshShaderStageInfo.pName = "main";
        
        // Reuse fragment shader
        VkPipelineShaderStageCreateInfo sphFragShaderStageInfo{};
        sphFragShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        sphFragShaderStageInfo.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        sphFragShaderStageInfo.module = m_fragShader;
        sphFragShaderStageInfo.pName = "main";
        
        std::array<VkPipelineShaderStageCreateInfo, 2> sphShaderStages = {
            sphMeshShaderStageInfo, sphFragShaderStageInfo
        };
        
        // Create SPH pipeline with same state but different shaders and layout
        VkGraphicsPipelineCreateInfo sphPipelineInfo{};
        sphPipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        sphPipelineInfo.pNext = &renderingInfo;
        sphPipelineInfo.stageCount = static_cast<uint32_t>(sphShaderStages.size());
        sphPipelineInfo.pStages = sphShaderStages.data();
        sphPipelineInfo.pVertexInputState = &vertexInputInfo;
        sphPipelineInfo.pInputAssemblyState = &inputAssembly;
        sphPipelineInfo.pViewportState = &viewportState;
        sphPipelineInfo.pRasterizationState = &rasterizer;
        sphPipelineInfo.pMultisampleState = &multisampling;
        sphPipelineInfo.pDepthStencilState = &depthStencil;
        sphPipelineInfo.pColorBlendState = &colorBlending;
        sphPipelineInfo.pDynamicState = &dynamicState;
        sphPipelineInfo.layout = m_sphPipelineLayout;
        sphPipelineInfo.renderPass = VK_NULL_HANDLE;
        sphPipelineInfo.subpass = 0;
        
        if (vkCreateGraphicsPipelines(m_context->getDevice(), VK_NULL_HANDLE, 1, &sphPipelineInfo, nullptr, &m_sphPipeline) != VK_SUCCESS) {
            std::cerr << "Failed to create SPH mesh shader pipeline - falling back to regular mesh shader" << std::endl;
            m_sphPipeline = VK_NULL_HANDLE;
        } else {
            std::cout << "🚀 SPH mesh shader pipeline created successfully!" << std::endl;
        }
    } else {
        std::cout << "SPH mesh shader not loaded - advanced SPH mode will fallback to regular mesh shader" << std::endl;
    }
}

void MeshParticleRenderer::render(VkCommandBuffer cmd, const glm::mat4& viewProj, const glm::vec3& cameraPos,
                                  VkBuffer particleBuffer, uint32_t particleCount, float particleSize, float time) {
    if (!m_isSupported) {
        return; // Fallback to traditional rendering
    }
    
    // Set dynamic viewport and scissor
    VkExtent2D extent = m_context->getSwapChainExtent();
    
    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = static_cast<float>(extent.width);
    viewport.height = static_cast<float>(extent.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    
    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = extent;
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    
    // Bind mesh shader pipeline
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);
    
    // TODO: For now, assume particle buffer doesn't change. In production,
    // we'd want to update descriptor sets outside render loop or use multiple sets.
    // Static particle buffer binding (updated once during initialization)
    static VkBuffer lastParticleBuffer = VK_NULL_HANDLE;
    if (lastParticleBuffer != particleBuffer) {
        VkDescriptorBufferInfo bufferInfo{};
        bufferInfo.buffer = particleBuffer;
        bufferInfo.offset = 0;
        bufferInfo.range = VK_WHOLE_SIZE;
        
        VkWriteDescriptorSet descriptorWrite{};
        descriptorWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        descriptorWrite.dstSet = m_descriptorSet;
        descriptorWrite.dstBinding = 0;
        descriptorWrite.dstArrayElement = 0;
        descriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        descriptorWrite.descriptorCount = 1;
        descriptorWrite.pBufferInfo = &bufferInfo;
        
        vkUpdateDescriptorSets(m_context->getDevice(), 1, &descriptorWrite, 0, nullptr);
        lastParticleBuffer = particleBuffer;
    }
    
    // Bind descriptor set
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 0, 1, &m_descriptorSet, 0, nullptr);

    // Job 1003: Push TLAS descriptor if RT is available
    bool rtEnabled = m_context->supportsAccelerationStructure() && m_context->supportsRayQuery() && m_rtShadowsEnabled;
    if (rtEnabled && m_topLevelAS != VK_NULL_HANDLE) {
        // Push TLAS descriptor to binding 3
        VkWriteDescriptorSetAccelerationStructureKHR asWrite{};
        asWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
        asWrite.pNext = nullptr;
        asWrite.accelerationStructureCount = 1;
        asWrite.pAccelerationStructures = &m_topLevelAS;

        VkWriteDescriptorSet descriptorWrite{};
        descriptorWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        descriptorWrite.pNext = &asWrite;
        descriptorWrite.dstSet = VK_NULL_HANDLE; // Push descriptor
        descriptorWrite.dstBinding = 3;
        descriptorWrite.dstArrayElement = 0;
        descriptorWrite.descriptorCount = 1;
        descriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;

        vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 0, 1, &descriptorWrite);

        // Throttled per-frame log
        static uint32_t frameCounter = 0;
        if ((frameCounter++ % 60) == 0) {
            std::cout << "Pushed TLAS at binding 3 (AS_KHR)" << std::endl;
        }
    }

    // Push constants
    MeshPushConstants pushConstants{};
    pushConstants.viewProj = viewProj;
    pushConstants.cameraPos = cameraPos;
    pushConstants.particleSize = particleSize;
    pushConstants.particleCount = particleCount;
    pushConstants.time = time;
    pushConstants.rtEnabled = rtEnabled ? 1u : 0u;
    // Job 1005: Light state
    pushConstants.lightDirection = m_lightDirection;
    pushConstants.lightIntensity = m_lightIntensity;
    pushConstants.occlusionAmplify = m_occlusionAmplifyEnabled ? 1u : 0u;
    
    vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_MESH_BIT_EXT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(MeshPushConstants), &pushConstants);
    
    // Draw mesh tasks (this replaces vkCmdDraw!)
    // Calculate number of mesh workgroups needed
    uint32_t particlesPerWorkgroup = 32; // Matches local_size_x in mesh shader  
    uint32_t numWorkgroups = (particleCount + particlesPerWorkgroup - 1) / particlesPerWorkgroup;
    
    // 📚 MCP Source: vkCmdDrawMeshTasksEXT generates geometry directly on GPU!
    vkCmdDrawMeshTasksEXT(cmd, numWorkgroups, 1, 1);
}

// Job 1005: Light control methods implementation
void MeshParticleRenderer::adjustLightDirection(float deltaTheta, float deltaPhi) {
    // Convert current direction to spherical coordinates
    float theta = atan2(m_lightDirection.z, m_lightDirection.x);
    float phi = asin(m_lightDirection.y);

    // Apply deltas
    theta += deltaTheta;
    phi += deltaPhi;

    // Clamp phi to prevent gimbal lock
    phi = glm::clamp(phi, -glm::pi<float>() * 0.4f, glm::pi<float>() * 0.4f);

    // Convert back to Cartesian
    m_lightDirection.x = cos(phi) * cos(theta);
    m_lightDirection.y = sin(phi);
    m_lightDirection.z = cos(phi) * sin(theta);

    m_lightDirection = glm::normalize(m_lightDirection);

    // Print current light state
    static auto lastPrint = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastPrint).count() > 200) {
        std::cout << "[LIGHT] Direction: (" << m_lightDirection.x << ", " << m_lightDirection.y
                  << ", " << m_lightDirection.z << "), Intensity: " << m_lightIntensity
                  << ", Amplify: " << (m_occlusionAmplifyEnabled ? "ON" : "OFF") << std::endl;
        lastPrint = now;
    }
}

void MeshParticleRenderer::adjustLightIntensity(float delta) {
    m_lightIntensity = glm::clamp(m_lightIntensity + delta, 0.1f, 3.0f);

    // Print current light state
    static auto lastPrint = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastPrint).count() > 200) {
        std::cout << "[LIGHT] Direction: (" << m_lightDirection.x << ", " << m_lightDirection.y
                  << ", " << m_lightDirection.z << "), Intensity: " << m_lightIntensity
                  << ", Amplify: " << (m_occlusionAmplifyEnabled ? "ON" : "OFF") << std::endl;
        lastPrint = now;
    }
}

void MeshParticleRenderer::toggleOcclusionAmplify() {
    m_occlusionAmplifyEnabled = !m_occlusionAmplifyEnabled;
    std::cout << "[LIGHT] Occlusion amplify: " << (m_occlusionAmplifyEnabled ? "ENABLED" : "DISABLED") << std::endl;
}

void MeshParticleRenderer::renderSPH(VkCommandBuffer cmd, const glm::mat4& viewProj, const glm::vec3& cameraPos,
                                     VkBuffer particleBuffer, uint32_t particleCount, float particleSize, float time,
                                     float smoothingRadius, float restDensity, float pressureConstant, float viscosity) {
    
    // Check if SPH pipeline is available
    if (m_sphPipeline == VK_NULL_HANDLE) {
        // Fallback to regular mesh shader if SPH not available
        render(cmd, viewProj, cameraPos, particleBuffer, particleCount, particleSize, time);
        return;
    }
    
    // Set viewport and scissor
    VkExtent2D extent = m_context->getSwapChainExtent();
    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = static_cast<float>(extent.width);
    viewport.height = static_cast<float>(extent.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    
    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = extent;
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    
    // Bind SPH mesh shader pipeline
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_sphPipeline);
    
    // Update descriptor set if buffer changed
    static VkBuffer lastParticleBuffer = VK_NULL_HANDLE;
    if (lastParticleBuffer != particleBuffer) {
        VkDescriptorBufferInfo bufferInfo{};
        bufferInfo.buffer = particleBuffer;
        bufferInfo.offset = 0;
        bufferInfo.range = VK_WHOLE_SIZE;
        
        VkWriteDescriptorSet descriptorWrite{};
        descriptorWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        descriptorWrite.dstSet = m_descriptorSet;
        descriptorWrite.dstBinding = 0;
        descriptorWrite.dstArrayElement = 0;
        descriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        descriptorWrite.descriptorCount = 1;
        descriptorWrite.pBufferInfo = &bufferInfo;
        
        vkUpdateDescriptorSets(m_context->getDevice(), 1, &descriptorWrite, 0, nullptr);
        lastParticleBuffer = particleBuffer;
    }
    
    // Bind descriptor set
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_sphPipelineLayout, 0, 1, &m_descriptorSet, 0, nullptr);
    
    // Push constants with SPH parameters
    SPHMeshPushConstants pushConstants{};
    pushConstants.viewProj = viewProj;
    pushConstants.cameraPos = cameraPos;
    pushConstants.particleSize = particleSize;
    pushConstants.particleCount = particleCount;
    pushConstants.time = time;
    pushConstants.smoothingRadius = smoothingRadius;
    pushConstants.restDensity = restDensity;
    pushConstants.pressureConstant = pressureConstant;
    pushConstants.viscosity = viscosity;
    pushConstants.mass = 1.0f; // Standard mass
    
    vkCmdPushConstants(cmd, m_sphPipelineLayout, VK_SHADER_STAGE_MESH_BIT_EXT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(SPHMeshPushConstants), &pushConstants);
    
    // Draw mesh tasks with SPH physics
    uint32_t particlesPerWorkgroup = 32; // Matches local_size_x in SPH mesh shader  
    uint32_t numWorkgroups = (particleCount + particlesPerWorkgroup - 1) / particlesPerWorkgroup;
    
    // 🚀 Revolutionary SPH-Mesh shader: GPU physics + rendering in one pass!
    vkCmdDrawMeshTasksEXT(cmd, numWorkgroups, 1, 1);
}

void MeshParticleRenderer::cleanup() {
    VkDevice device = m_context->getDevice();

    // Job 1002: Safe teardown order - device wait first
    if (device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device);
    }

    // Clean up acceleration structures first
    cleanupAccelerationStructures();

    if (m_pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(device, m_pipeline, nullptr);
        m_pipeline = VK_NULL_HANDLE;
    }
    
    if (m_sphPipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(device, m_sphPipeline, nullptr);
        m_sphPipeline = VK_NULL_HANDLE;
    }
    
    if (m_pipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device, m_pipelineLayout, nullptr);
        m_pipelineLayout = VK_NULL_HANDLE;
    }
    
    if (m_sphPipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device, m_sphPipelineLayout, nullptr);
        m_sphPipelineLayout = VK_NULL_HANDLE;
    }
    
    if (m_descriptorSetLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device, m_descriptorSetLayout, nullptr);
        m_descriptorSetLayout = VK_NULL_HANDLE;
    }
    
    // Descriptor pool automatically frees all allocated descriptor sets when destroyed
    if (m_descriptorPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device, m_descriptorPool, nullptr);
        m_descriptorPool = VK_NULL_HANDLE;
        m_descriptorSet = VK_NULL_HANDLE; // Automatically freed with pool
    }
    
    if (m_meshShader != VK_NULL_HANDLE) {
        vkDestroyShaderModule(device, m_meshShader, nullptr);
        m_meshShader = VK_NULL_HANDLE;
    }
    
    if (m_sphMeshShader != VK_NULL_HANDLE) {
        vkDestroyShaderModule(device, m_sphMeshShader, nullptr);
        m_sphMeshShader = VK_NULL_HANDLE;
    }
    
    if (m_fragShader != VK_NULL_HANDLE) {
        vkDestroyShaderModule(device, m_fragShader, nullptr);
        m_fragShader = VK_NULL_HANDLE;
    }
}

VkShaderModule MeshParticleRenderer::createShaderModule(const std::vector<char>& code) {
    VkShaderModuleCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    createInfo.codeSize = code.size();
    createInfo.pCode = reinterpret_cast<const uint32_t*>(code.data());
    
    VkShaderModule shaderModule;
    if (vkCreateShaderModule(m_context->getDevice(), &createInfo, nullptr, &shaderModule) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create shader module!");
    }
    
    return shaderModule;
}

std::vector<char> MeshParticleRenderer::readFile(const std::string& filename) {
    std::ifstream file(filename, std::ios::ate | std::ios::binary);
    
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open file: " + filename);
    }
    
    size_t fileSize = static_cast<size_t>(file.tellg());
    std::vector<char> buffer(fileSize);
    
    file.seekg(0);
    file.read(buffer.data(), fileSize);
    file.close();

    return buffer;
}

// Job 1002: Acceleration structure implementation
void MeshParticleRenderer::createAccelerationStructures() {
    std::cout << "Creating acceleration structures for RT occluders..." << std::endl;

    try {
        createOccluderGeometry();
        buildBLAS();
        buildTLAS();
        std::cout << "Acceleration structures created successfully!" << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "Failed to create acceleration structures: " << e.what() << std::endl;
        cleanupAccelerationStructures();
        throw;
    }
}

void MeshParticleRenderer::createOccluderGeometry() {
    VkDevice device = m_context->getDevice();

    // Job 1004: Generate proper icosphere with 2 subdivisions
    auto [sphereVertices, sphereIndices] = generateIcosphere(3.0f, 2);

    // Job 1004: Generate disc/annulus mesh in XZ plane
    auto [discVertices, discIndices] = generateDisc(1.0f, 6.0f, 64);

    // Create vertex buffers with device address capability
    VkBufferUsageFlags vertexUsage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                                     VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                                     VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                                     VK_BUFFER_USAGE_TRANSFER_DST_BIT;

    VkBufferUsageFlags indexUsage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                    VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                                    VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                                    VK_BUFFER_USAGE_TRANSFER_DST_BIT;

    std::cout << "  Creating sphere and disc geometry buffers..." << std::endl;
    std::cout << "  Sphere: " << sphereVertices.size() << " vertices, " << sphereIndices.size() << " indices" << std::endl;
    std::cout << "  Disc: " << discVertices.size() << " vertices, " << discIndices.size() << " indices" << std::endl;

    // Create sphere vertex buffer
    createBufferWithData(device, sphereVertices.data(), sphereVertices.size() * sizeof(glm::vec3),
                        vertexUsage, m_sphereVertexBuffer, m_sphereVertexMemory);

    // Create sphere index buffer
    createBufferWithData(device, sphereIndices.data(), sphereIndices.size() * sizeof(uint32_t),
                        indexUsage, m_sphereIndexBuffer, m_sphereIndexMemory);

    // Create disc vertex buffer
    createBufferWithData(device, discVertices.data(), discVertices.size() * sizeof(glm::vec3),
                        vertexUsage, m_discVertexBuffer, m_discVertexMemory);

    // Create disc index buffer
    createBufferWithData(device, discIndices.data(), discIndices.size() * sizeof(uint32_t),
                        indexUsage, m_discIndexBuffer, m_discIndexMemory);

    // Store geometry counts for BLAS building
    m_sphereVertexCount = sphereVertices.size();
    m_sphereIndexCount = sphereIndices.size();
    m_discVertexCount = discVertices.size();
    m_discIndexCount = discIndices.size();

    // Get device addresses for BLAS building
    VkBufferDeviceAddressInfo sphereVertexAddressInfo{};
    sphereVertexAddressInfo.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    sphereVertexAddressInfo.buffer = m_sphereVertexBuffer;
    m_sphereVertexAddress = vkGetBufferDeviceAddress(device, &sphereVertexAddressInfo);

    VkBufferDeviceAddressInfo sphereIndexAddressInfo{};
    sphereIndexAddressInfo.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    sphereIndexAddressInfo.buffer = m_sphereIndexBuffer;
    m_sphereIndexAddress = vkGetBufferDeviceAddress(device, &sphereIndexAddressInfo);

    VkBufferDeviceAddressInfo discVertexAddressInfo{};
    discVertexAddressInfo.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    discVertexAddressInfo.buffer = m_discVertexBuffer;
    m_discVertexAddress = vkGetBufferDeviceAddress(device, &discVertexAddressInfo);

    VkBufferDeviceAddressInfo discIndexAddressInfo{};
    discIndexAddressInfo.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    discIndexAddressInfo.buffer = m_discIndexBuffer;
    m_discIndexAddress = vkGetBufferDeviceAddress(device, &discIndexAddressInfo);

    std::cout << "  Sphere vertex device address: 0x" << std::hex << m_sphereVertexAddress << std::dec << std::endl;
    std::cout << "  Disc vertex device address: 0x" << std::hex << m_discVertexAddress << std::dec << std::endl;
}

// Job 1004: Icosphere generation with subdivision
std::pair<std::vector<glm::vec3>, std::vector<uint32_t>> MeshParticleRenderer::generateIcosphere(float radius, int subdivisions) {
    // Start with icosahedron vertices
    const float t = (1.0f + std::sqrt(5.0f)) / 2.0f; // Golden ratio

    std::vector<glm::vec3> vertices = {
        {-1, t, 0}, {1, t, 0}, {-1, -t, 0}, {1, -t, 0},
        {0, -1, t}, {0, 1, t}, {0, -1, -t}, {0, 1, -t},
        {t, 0, -1}, {t, 0, 1}, {-t, 0, -1}, {-t, 0, 1}
    };

    // Normalize to unit sphere
    for (auto& vertex : vertices) {
        vertex = normalize(vertex);
    }

    // Icosahedron faces
    std::vector<uint32_t> indices = {
        0,11,5, 0,5,1, 0,1,7, 0,7,10, 0,10,11,
        1,5,9, 5,11,4, 11,10,2, 10,7,6, 7,1,8,
        3,9,4, 3,4,2, 3,2,6, 3,6,8, 3,8,9,
        4,9,5, 2,4,11, 6,2,10, 8,6,7, 9,8,1
    };

    // Subdivide faces
    for (int i = 0; i < subdivisions; ++i) {
        std::vector<uint32_t> newIndices;

        for (size_t j = 0; j < indices.size(); j += 3) {
            uint32_t v1 = indices[j];
            uint32_t v2 = indices[j + 1];
            uint32_t v3 = indices[j + 2];

            // Create midpoint vertices
            glm::vec3 mid12 = normalize(vertices[v1] + vertices[v2]);
            glm::vec3 mid23 = normalize(vertices[v2] + vertices[v3]);
            glm::vec3 mid31 = normalize(vertices[v3] + vertices[v1]);

            uint32_t mid12Idx = vertices.size();
            uint32_t mid23Idx = vertices.size() + 1;
            uint32_t mid31Idx = vertices.size() + 2;

            vertices.push_back(mid12);
            vertices.push_back(mid23);
            vertices.push_back(mid31);

            // Create 4 new triangles
            newIndices.insert(newIndices.end(), {v1, mid12Idx, mid31Idx});
            newIndices.insert(newIndices.end(), {v2, mid23Idx, mid12Idx});
            newIndices.insert(newIndices.end(), {v3, mid31Idx, mid23Idx});
            newIndices.insert(newIndices.end(), {mid12Idx, mid23Idx, mid31Idx});
        }

        indices = newIndices;
    }

    // Scale to desired radius
    for (auto& vertex : vertices) {
        vertex *= radius;
    }

    return {vertices, indices};
}

// Job 1004: Disc/annulus generation in XZ plane
std::pair<std::vector<glm::vec3>, std::vector<uint32_t>> MeshParticleRenderer::generateDisc(float innerRadius, float outerRadius, int segments) {
    std::vector<glm::vec3> vertices;
    std::vector<uint32_t> indices;
    const float PI = 3.14159265359f;

    // Generate ring vertices
    for (int i = 0; i <= segments; ++i) {
        float angle = 2.0f * PI * i / segments;
        float cosA = std::cos(angle);
        float sinA = std::sin(angle);

        // Inner ring vertex
        vertices.push_back({innerRadius * cosA, 0.0f, innerRadius * sinA});
        // Outer ring vertex
        vertices.push_back({outerRadius * cosA, 0.0f, outerRadius * sinA});
    }

    // Generate quad indices (as triangles)
    for (int i = 0; i < segments; ++i) {
        uint32_t innerCurrent = i * 2;
        uint32_t outerCurrent = i * 2 + 1;
        uint32_t innerNext = ((i + 1) % (segments + 1)) * 2;
        uint32_t outerNext = ((i + 1) % (segments + 1)) * 2 + 1;

        // First triangle
        indices.insert(indices.end(), {innerCurrent, outerCurrent, outerNext});
        // Second triangle
        indices.insert(indices.end(), {innerCurrent, outerNext, innerNext});
    }

    return {vertices, indices};
}

// Job 1004: Helper to create buffer with data (since VMA is disabled)
void MeshParticleRenderer::createBufferWithData(VkDevice device, const void* data, VkDeviceSize size,
                                                VkBufferUsageFlags usage, VkBuffer& buffer, VkDeviceMemory& memory) {
    // Create buffer
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateBuffer(device, &bufferInfo, nullptr, &buffer) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create buffer for occluder geometry!");
    }

    // Get memory requirements
    VkMemoryRequirements memRequirements;
    vkGetBufferMemoryRequirements(device, buffer, &memRequirements);

    // Allocate memory with device address bit for shader device address buffers
    VkMemoryAllocateFlagsInfo allocFlagsInfo{};
    allocFlagsInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
    allocFlagsInfo.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.pNext = &allocFlagsInfo;  // Required for device address buffers
    allocInfo.allocationSize = memRequirements.size;

    // Find memory type that supports host visible + coherent
    VkPhysicalDeviceMemoryProperties memProperties;
    vkGetPhysicalDeviceMemoryProperties(m_context->getPhysicalDevice(), &memProperties);

    uint32_t memoryTypeIndex = UINT32_MAX;
    VkMemoryPropertyFlags requiredProperties = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

    for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
        if ((memRequirements.memoryTypeBits & (1 << i)) &&
            (memProperties.memoryTypes[i].propertyFlags & requiredProperties) == requiredProperties) {
            memoryTypeIndex = i;
            break;
        }
    }

    if (memoryTypeIndex == UINT32_MAX) {
        throw std::runtime_error("Failed to find suitable memory type for buffer!");
    }

    allocInfo.memoryTypeIndex = memoryTypeIndex;

    if (vkAllocateMemory(device, &allocInfo, nullptr, &memory) != VK_SUCCESS) {
        throw std::runtime_error("Failed to allocate buffer memory!");
    }

    vkBindBufferMemory(device, buffer, memory, 0);

    // Copy data
    void* mappedData;
    vkMapMemory(device, memory, 0, size, 0, &mappedData);
    memcpy(mappedData, data, size);
    vkUnmapMemory(device, memory);
}

// Job 1004: Helper to create buffer for acceleration structures
void MeshParticleRenderer::createBufferForAS(VkDevice device, VkDeviceSize size, VkBuffer& buffer, VkDeviceMemory& memory) {
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateBuffer(device, &bufferInfo, nullptr, &buffer) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create AS buffer!");
    }

    VkMemoryRequirements memRequirements;
    vkGetBufferMemoryRequirements(device, buffer, &memRequirements);

    // Device address flag required for AS buffers too
    VkMemoryAllocateFlagsInfo allocFlagsInfo{};
    allocFlagsInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
    allocFlagsInfo.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.pNext = &allocFlagsInfo;
    allocInfo.allocationSize = memRequirements.size;

    // Find device-local memory for AS
    VkPhysicalDeviceMemoryProperties memProperties;
    vkGetPhysicalDeviceMemoryProperties(m_context->getPhysicalDevice(), &memProperties);

    uint32_t memoryTypeIndex = UINT32_MAX;
    VkMemoryPropertyFlags requiredProperties = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

    for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
        if ((memRequirements.memoryTypeBits & (1 << i)) &&
            (memProperties.memoryTypes[i].propertyFlags & requiredProperties) == requiredProperties) {
            memoryTypeIndex = i;
            break;
        }
    }

    if (memoryTypeIndex == UINT32_MAX) {
        // Fallback to any available memory type
        for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
            if (memRequirements.memoryTypeBits & (1 << i)) {
                memoryTypeIndex = i;
                break;
            }
        }
    }

    if (memoryTypeIndex == UINT32_MAX) {
        throw std::runtime_error("Failed to find suitable memory type for AS buffer!");
    }

    allocInfo.memoryTypeIndex = memoryTypeIndex;

    if (vkAllocateMemory(device, &allocInfo, nullptr, &memory) != VK_SUCCESS) {
        throw std::runtime_error("Failed to allocate AS buffer memory!");
    }

    vkBindBufferMemory(device, buffer, memory, 0);
}

// Job 1004: Command buffer helpers for AS building
VkCommandBuffer MeshParticleRenderer::beginSingleTimeCommands() {
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandPool = m_context->getCommandPool();
    allocInfo.commandBufferCount = 1;

    VkCommandBuffer commandBuffer;
    vkAllocateCommandBuffers(m_context->getDevice(), &allocInfo, &commandBuffer);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    vkBeginCommandBuffer(commandBuffer, &beginInfo);

    return commandBuffer;
}

void MeshParticleRenderer::endSingleTimeCommands(VkCommandBuffer commandBuffer) {
    vkEndCommandBuffer(commandBuffer);

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffer;

    vkQueueSubmit(m_context->getGraphicsQueue(), 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(m_context->getGraphicsQueue());

    vkFreeCommandBuffers(m_context->getDevice(), m_context->getCommandPool(), 1, &commandBuffer);
}

void MeshParticleRenderer::buildBLAS() {
    std::cout << "  Building BLAS for sphere and disc occluders..." << std::endl;

    VkDevice device = m_context->getDevice();

    // Build sphere BLAS
    {
        // Setup geometry data for sphere
        VkAccelerationStructureGeometryTrianglesDataKHR sphereTriangles{};
        sphereTriangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
        sphereTriangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
        sphereTriangles.vertexData.deviceAddress = m_sphereVertexAddress;
        sphereTriangles.vertexStride = sizeof(glm::vec3);
        sphereTriangles.indexType = VK_INDEX_TYPE_UINT32;
        sphereTriangles.indexData.deviceAddress = m_sphereIndexAddress;
        sphereTriangles.transformData.deviceAddress = 0; // No transform matrix

        VkAccelerationStructureGeometryKHR sphereGeometry{};
        sphereGeometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
        sphereGeometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        sphereGeometry.geometry.triangles = sphereTriangles;
        sphereGeometry.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;

        // Build geometry info
        VkAccelerationStructureBuildGeometryInfoKHR sphereBuildInfo{};
        sphereBuildInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
        sphereBuildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        sphereBuildInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        sphereBuildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        sphereBuildInfo.geometryCount = 1;
        sphereBuildInfo.pGeometries = &sphereGeometry;

        // Get size requirements
        uint32_t primitiveCount = m_sphereIndexCount / 3;
        VkAccelerationStructureBuildSizesInfoKHR sphereSizeInfo{};
        sphereSizeInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
        vkGetAccelerationStructureBuildSizesKHR(device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                                               &sphereBuildInfo, &primitiveCount, &sphereSizeInfo);

        // Create BLAS buffer
        createBufferForAS(device, sphereSizeInfo.accelerationStructureSize, m_sphereBLASBuffer, m_sphereBLASMemory);

        // Create acceleration structure
        VkAccelerationStructureCreateInfoKHR sphereCreateInfo{};
        sphereCreateInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
        sphereCreateInfo.buffer = m_sphereBLASBuffer;
        sphereCreateInfo.size = sphereSizeInfo.accelerationStructureSize;
        sphereCreateInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;

        if (vkCreateAccelerationStructureKHR(device, &sphereCreateInfo, nullptr, &m_sphereBLAS) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create sphere BLAS!");
        }

        // Create scratch buffer
        VkBuffer scratchBuffer;
        VkDeviceMemory scratchMemory;
        createBufferForAS(device, sphereSizeInfo.buildScratchSize, scratchBuffer, scratchMemory);

        VkBufferDeviceAddressInfo scratchAddressInfo{};
        scratchAddressInfo.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
        scratchAddressInfo.buffer = scratchBuffer;
        VkDeviceAddress scratchAddress = vkGetBufferDeviceAddress(device, &scratchAddressInfo);

        // Update build info with AS and scratch
        sphereBuildInfo.dstAccelerationStructure = m_sphereBLAS;
        sphereBuildInfo.scratchData.deviceAddress = scratchAddress;

        // Build range info
        VkAccelerationStructureBuildRangeInfoKHR sphereRangeInfo{};
        sphereRangeInfo.primitiveCount = primitiveCount;
        sphereRangeInfo.primitiveOffset = 0;
        sphereRangeInfo.firstVertex = 0;
        sphereRangeInfo.transformOffset = 0;

        const VkAccelerationStructureBuildRangeInfoKHR* pSphereRangeInfo = &sphereRangeInfo;

        // Record build commands
        VkCommandBuffer cmdBuffer = beginSingleTimeCommands();
        vkCmdBuildAccelerationStructuresKHR(cmdBuffer, 1, &sphereBuildInfo, &pSphereRangeInfo);
        endSingleTimeCommands(cmdBuffer);

        // Cleanup scratch buffer
        vkDestroyBuffer(device, scratchBuffer, nullptr);
        vkFreeMemory(device, scratchMemory, nullptr);
    }

    // Build disc BLAS (similar process)
    {
        // Setup geometry data for disc
        VkAccelerationStructureGeometryTrianglesDataKHR discTriangles{};
        discTriangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
        discTriangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
        discTriangles.vertexData.deviceAddress = m_discVertexAddress;
        discTriangles.vertexStride = sizeof(glm::vec3);
        discTriangles.indexType = VK_INDEX_TYPE_UINT32;
        discTriangles.indexData.deviceAddress = m_discIndexAddress;
        discTriangles.transformData.deviceAddress = 0;

        VkAccelerationStructureGeometryKHR discGeometry{};
        discGeometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
        discGeometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        discGeometry.geometry.triangles = discTriangles;
        discGeometry.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;

        VkAccelerationStructureBuildGeometryInfoKHR discBuildInfo{};
        discBuildInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
        discBuildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        discBuildInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        discBuildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        discBuildInfo.geometryCount = 1;
        discBuildInfo.pGeometries = &discGeometry;

        uint32_t discPrimitiveCount = m_discIndexCount / 3;
        VkAccelerationStructureBuildSizesInfoKHR discSizeInfo{};
        discSizeInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
        vkGetAccelerationStructureBuildSizesKHR(device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                                               &discBuildInfo, &discPrimitiveCount, &discSizeInfo);

        createBufferForAS(device, discSizeInfo.accelerationStructureSize, m_discBLASBuffer, m_discBLASMemory);

        VkAccelerationStructureCreateInfoKHR discCreateInfo{};
        discCreateInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
        discCreateInfo.buffer = m_discBLASBuffer;
        discCreateInfo.size = discSizeInfo.accelerationStructureSize;
        discCreateInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;

        if (vkCreateAccelerationStructureKHR(device, &discCreateInfo, nullptr, &m_discBLAS) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create disc BLAS!");
        }

        VkBuffer scratchBuffer;
        VkDeviceMemory scratchMemory;
        createBufferForAS(device, discSizeInfo.buildScratchSize, scratchBuffer, scratchMemory);

        VkBufferDeviceAddressInfo scratchAddressInfo{};
        scratchAddressInfo.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
        scratchAddressInfo.buffer = scratchBuffer;
        VkDeviceAddress scratchAddress = vkGetBufferDeviceAddress(device, &scratchAddressInfo);

        discBuildInfo.dstAccelerationStructure = m_discBLAS;
        discBuildInfo.scratchData.deviceAddress = scratchAddress;

        VkAccelerationStructureBuildRangeInfoKHR discRangeInfo{};
        discRangeInfo.primitiveCount = discPrimitiveCount;
        discRangeInfo.primitiveOffset = 0;
        discRangeInfo.firstVertex = 0;
        discRangeInfo.transformOffset = 0;

        const VkAccelerationStructureBuildRangeInfoKHR* pDiscRangeInfo = &discRangeInfo;

        VkCommandBuffer cmdBuffer = beginSingleTimeCommands();
        vkCmdBuildAccelerationStructuresKHR(cmdBuffer, 1, &discBuildInfo, &pDiscRangeInfo);
        endSingleTimeCommands(cmdBuffer);

        vkDestroyBuffer(device, scratchBuffer, nullptr);
        vkFreeMemory(device, scratchMemory, nullptr);
    }

    std::cout << "  BLAS build complete for both sphere and disc" << std::endl;
}

void MeshParticleRenderer::buildTLAS() {
    std::cout << "  Building TLAS with occluder instances..." << std::endl;

    VkDevice device = m_context->getDevice();

    // Get BLAS device addresses
    VkAccelerationStructureDeviceAddressInfoKHR sphereAddressInfo{};
    sphereAddressInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
    sphereAddressInfo.accelerationStructure = m_sphereBLAS;
    VkDeviceAddress sphereBLASAddress = vkGetAccelerationStructureDeviceAddressKHR(device, &sphereAddressInfo);

    VkAccelerationStructureDeviceAddressInfoKHR discAddressInfo{};
    discAddressInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
    discAddressInfo.accelerationStructure = m_discBLAS;
    VkDeviceAddress discBLASAddress = vkGetAccelerationStructureDeviceAddressKHR(device, &discAddressInfo);

    // Create TLAS instances with proper transforms
    std::vector<VkAccelerationStructureInstanceKHR> instances(2);

    // Instance 0: Sphere at origin with radius scale
    instances[0].transform.matrix[0][0] = 1.0f; // Scale X
    instances[0].transform.matrix[0][1] = 0.0f;
    instances[0].transform.matrix[0][2] = 0.0f;
    instances[0].transform.matrix[0][3] = 0.0f; // Translation X

    instances[0].transform.matrix[1][0] = 0.0f;
    instances[0].transform.matrix[1][1] = 1.0f; // Scale Y
    instances[0].transform.matrix[1][2] = 0.0f;
    instances[0].transform.matrix[1][3] = 0.0f; // Translation Y

    instances[0].transform.matrix[2][0] = 0.0f;
    instances[0].transform.matrix[2][1] = 0.0f;
    instances[0].transform.matrix[2][2] = 1.0f; // Scale Z
    instances[0].transform.matrix[2][3] = 0.0f; // Translation Z

    instances[0].instanceCustomIndex = 0;
    instances[0].mask = 0xFF;
    instances[0].instanceShaderBindingTableRecordOffset = 0;
    instances[0].flags = 0;
    instances[0].accelerationStructureReference = sphereBLASAddress;

    // Instance 1: Disc tilted 25 degrees around X axis
    float tiltAngle = 25.0f * (3.14159265359f / 180.0f); // Convert to radians
    float cosAngle = std::cos(tiltAngle);
    float sinAngle = std::sin(tiltAngle);

    // Row-major rotation matrix around X-axis
    instances[1].transform.matrix[0][0] = 1.0f;
    instances[1].transform.matrix[0][1] = 0.0f;
    instances[1].transform.matrix[0][2] = 0.0f;
    instances[1].transform.matrix[0][3] = 0.0f; // Translation X

    instances[1].transform.matrix[1][0] = 0.0f;
    instances[1].transform.matrix[1][1] = cosAngle;
    instances[1].transform.matrix[1][2] = -sinAngle;
    instances[1].transform.matrix[1][3] = 0.0f; // Translation Y

    instances[1].transform.matrix[2][0] = 0.0f;
    instances[1].transform.matrix[2][1] = sinAngle;
    instances[1].transform.matrix[2][2] = cosAngle;
    instances[1].transform.matrix[2][3] = 0.0f; // Translation Z

    instances[1].instanceCustomIndex = 1;
    instances[1].mask = 0xFF;
    instances[1].instanceShaderBindingTableRecordOffset = 0;
    instances[1].flags = 0;
    instances[1].accelerationStructureReference = discBLASAddress;

    // Create instances buffer
    VkDeviceSize instancesSize = sizeof(VkAccelerationStructureInstanceKHR) * instances.size();
    createBufferWithData(device, instances.data(), instancesSize,
                        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                        m_instancesBuffer, m_instancesMemory);

    VkBufferDeviceAddressInfo instancesAddressInfo{};
    instancesAddressInfo.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    instancesAddressInfo.buffer = m_instancesBuffer;
    VkDeviceAddress instancesAddress = vkGetBufferDeviceAddress(device, &instancesAddressInfo);

    // Setup TLAS geometry
    VkAccelerationStructureGeometryInstancesDataKHR instancesData{};
    instancesData.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    instancesData.arrayOfPointers = VK_FALSE;
    instancesData.data.deviceAddress = instancesAddress;

    VkAccelerationStructureGeometryKHR tlasGeometry{};
    tlasGeometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
    tlasGeometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    tlasGeometry.geometry.instances = instancesData;

    // Build geometry info
    VkAccelerationStructureBuildGeometryInfoKHR tlasBuildInfo{};
    tlasBuildInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
    tlasBuildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    tlasBuildInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    tlasBuildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    tlasBuildInfo.geometryCount = 1;
    tlasBuildInfo.pGeometries = &tlasGeometry;

    // Get size requirements
    uint32_t instanceCount = static_cast<uint32_t>(instances.size());
    VkAccelerationStructureBuildSizesInfoKHR tlasSizeInfo{};
    tlasSizeInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
    vkGetAccelerationStructureBuildSizesKHR(device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                                           &tlasBuildInfo, &instanceCount, &tlasSizeInfo);

    // Create TLAS buffer
    createBufferForAS(device, tlasSizeInfo.accelerationStructureSize, m_topLevelASBuffer, m_topLevelASMemory);

    // Create acceleration structure
    VkAccelerationStructureCreateInfoKHR tlasCreateInfo{};
    tlasCreateInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
    tlasCreateInfo.buffer = m_topLevelASBuffer;
    tlasCreateInfo.size = tlasSizeInfo.accelerationStructureSize;
    tlasCreateInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;

    if (vkCreateAccelerationStructureKHR(device, &tlasCreateInfo, nullptr, &m_topLevelAS) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create TLAS!");
    }

    // Create scratch buffer
    VkBuffer scratchBuffer;
    VkDeviceMemory scratchMemory;
    createBufferForAS(device, tlasSizeInfo.buildScratchSize, scratchBuffer, scratchMemory);

    VkBufferDeviceAddressInfo scratchAddressInfo{};
    scratchAddressInfo.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    scratchAddressInfo.buffer = scratchBuffer;
    VkDeviceAddress scratchAddress = vkGetBufferDeviceAddress(device, &scratchAddressInfo);

    // Update build info with AS and scratch
    tlasBuildInfo.dstAccelerationStructure = m_topLevelAS;
    tlasBuildInfo.scratchData.deviceAddress = scratchAddress;

    // Build range info
    VkAccelerationStructureBuildRangeInfoKHR tlasRangeInfo{};
    tlasRangeInfo.primitiveCount = instanceCount;
    tlasRangeInfo.primitiveOffset = 0;
    tlasRangeInfo.firstVertex = 0;
    tlasRangeInfo.transformOffset = 0;

    const VkAccelerationStructureBuildRangeInfoKHR* pTlasRangeInfo = &tlasRangeInfo;

    // Record build commands
    VkCommandBuffer cmdBuffer = beginSingleTimeCommands();
    vkCmdBuildAccelerationStructuresKHR(cmdBuffer, 1, &tlasBuildInfo, &pTlasRangeInfo);
    endSingleTimeCommands(cmdBuffer);

    // Cleanup scratch buffer
    vkDestroyBuffer(device, scratchBuffer, nullptr);
    vkFreeMemory(device, scratchMemory, nullptr);

    // Get and log TLAS device address as required by job acceptance criteria
    VkAccelerationStructureDeviceAddressInfoKHR tlasAddressInfo{};
    tlasAddressInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
    tlasAddressInfo.accelerationStructure = m_topLevelAS;
    VkDeviceAddress tlasAddress = vkGetAccelerationStructureDeviceAddressKHR(device, &tlasAddressInfo);

    std::cout << "  TLAS device address: 0x" << std::hex << tlasAddress << std::dec << std::endl;
    std::cout << "  Sphere instance transform: identity at origin" << std::endl;
    std::cout << "  Disc instance transform: 25° tilt around X-axis" << std::endl;
}

void MeshParticleRenderer::cleanupAccelerationStructures() {
    VkDevice device = m_context->getDevice();

    if (device == VK_NULL_HANDLE) return;

    std::cout << "Cleaning up acceleration structures..." << std::endl;

    // Job 1002: Safe destruction order - TLAS first, then BLAS, then buffers
    if (m_topLevelAS != VK_NULL_HANDLE) {
        vkDestroyAccelerationStructureKHR(device, m_topLevelAS, nullptr);
        m_topLevelAS = VK_NULL_HANDLE;
    }

    if (m_sphereBLAS != VK_NULL_HANDLE) {
        vkDestroyAccelerationStructureKHR(device, m_sphereBLAS, nullptr);
        m_sphereBLAS = VK_NULL_HANDLE;
    }

    if (m_discBLAS != VK_NULL_HANDLE) {
        vkDestroyAccelerationStructureKHR(device, m_discBLAS, nullptr);
        m_discBLAS = VK_NULL_HANDLE;
    }

    // Free all AS and geometry buffers
    if (m_topLevelASBuffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(device, m_topLevelASBuffer, nullptr);
        m_topLevelASBuffer = VK_NULL_HANDLE;
    }

    if (m_sphereBLASBuffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(device, m_sphereBLASBuffer, nullptr);
        m_sphereBLASBuffer = VK_NULL_HANDLE;
    }

    if (m_discBLASBuffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(device, m_discBLASBuffer, nullptr);
        m_discBLASBuffer = VK_NULL_HANDLE;
    }

    if (m_instancesBuffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(device, m_instancesBuffer, nullptr);
        m_instancesBuffer = VK_NULL_HANDLE;
    }

    if (m_sphereVertexBuffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(device, m_sphereVertexBuffer, nullptr);
        m_sphereVertexBuffer = VK_NULL_HANDLE;
    }

    if (m_sphereIndexBuffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(device, m_sphereIndexBuffer, nullptr);
        m_sphereIndexBuffer = VK_NULL_HANDLE;
    }

    if (m_discVertexBuffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(device, m_discVertexBuffer, nullptr);
        m_discVertexBuffer = VK_NULL_HANDLE;
    }

    if (m_discIndexBuffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(device, m_discIndexBuffer, nullptr);
        m_discIndexBuffer = VK_NULL_HANDLE;
    }

    // Job 1004: Free all device memory allocations
    if (m_topLevelASMemory != VK_NULL_HANDLE) {
        vkFreeMemory(device, m_topLevelASMemory, nullptr);
        m_topLevelASMemory = VK_NULL_HANDLE;
    }

    if (m_sphereBLASMemory != VK_NULL_HANDLE) {
        vkFreeMemory(device, m_sphereBLASMemory, nullptr);
        m_sphereBLASMemory = VK_NULL_HANDLE;
    }

    if (m_discBLASMemory != VK_NULL_HANDLE) {
        vkFreeMemory(device, m_discBLASMemory, nullptr);
        m_discBLASMemory = VK_NULL_HANDLE;
    }

    if (m_instancesMemory != VK_NULL_HANDLE) {
        vkFreeMemory(device, m_instancesMemory, nullptr);
        m_instancesMemory = VK_NULL_HANDLE;
    }

    if (m_sphereVertexMemory != VK_NULL_HANDLE) {
        vkFreeMemory(device, m_sphereVertexMemory, nullptr);
        m_sphereVertexMemory = VK_NULL_HANDLE;
    }

    if (m_sphereIndexMemory != VK_NULL_HANDLE) {
        vkFreeMemory(device, m_sphereIndexMemory, nullptr);
        m_sphereIndexMemory = VK_NULL_HANDLE;
    }

    if (m_discVertexMemory != VK_NULL_HANDLE) {
        vkFreeMemory(device, m_discVertexMemory, nullptr);
        m_discVertexMemory = VK_NULL_HANDLE;
    }

    if (m_discIndexMemory != VK_NULL_HANDLE) {
        vkFreeMemory(device, m_discIndexMemory, nullptr);
        m_discIndexMemory = VK_NULL_HANDLE;
    }

    std::cout << "Acceleration structures cleanup complete" << std::endl;
}

} // namespace plasma