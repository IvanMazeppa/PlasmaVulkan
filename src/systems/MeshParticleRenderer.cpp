#include "MeshParticleRenderer.h"
#include "../renderer/VulkanContext.h"
#include <iostream>
#include <fstream>
#include <stdexcept>
#include <array>

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
    
    std::array<VkDescriptorSetLayoutBinding, 1> bindings = {
        particleBufferBinding
    };
    
    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.flags = 0; // Regular descriptor sets, not push descriptors
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
    // Create descriptor pool for mesh shader particle buffer binding
    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    poolSize.descriptorCount = 1; // We only need 1 storage buffer descriptor
    
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
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
    
    // Push constants
    MeshPushConstants pushConstants{};
    pushConstants.viewProj = viewProj;
    pushConstants.cameraPos = cameraPos;
    pushConstants.particleSize = particleSize;
    pushConstants.particleCount = particleCount;
    pushConstants.time = time;
    
    vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_MESH_BIT_EXT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(MeshPushConstants), &pushConstants);
    
    // Draw mesh tasks (this replaces vkCmdDraw!)
    // Calculate number of mesh workgroups needed
    uint32_t particlesPerWorkgroup = 32; // Matches local_size_x in mesh shader  
    uint32_t numWorkgroups = (particleCount + particlesPerWorkgroup - 1) / particlesPerWorkgroup;
    
    // 📚 MCP Source: vkCmdDrawMeshTasksEXT generates geometry directly on GPU!
    vkCmdDrawMeshTasksEXT(cmd, numWorkgroups, 1, 1);
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

} // namespace plasma