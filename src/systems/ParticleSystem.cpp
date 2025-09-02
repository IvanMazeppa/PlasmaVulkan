#include "ParticleSystem.h"
#include "../renderer/VulkanContext.h"
#include <iostream>
#include <random>
#include <array>
#include <cstring>

namespace plasma {

ParticleSystem::ParticleSystem(VulkanContext* context, uint32_t particleCount)
    : m_context(context), m_particleCount(particleCount), m_activeParticleCount(particleCount) {
    
    std::cout << "Creating particle system with " << particleCount << " particles..." << std::endl;
    
    createParticleBuffer();
    createComputePipeline();
    createSPHPipeline();
    createGraphicsPipeline();
    createDescriptorSets();
    
    // Initialize particles with random positions
    initializeParticles();
    
    std::cout << "Particle system created successfully!" << std::endl;
}

ParticleSystem::~ParticleSystem() {
    cleanup();
}

void ParticleSystem::createParticleBuffer() {
    // Calculate buffer size
    VkDeviceSize bufferSize = sizeof(Particle) * m_particleCount;
    
    // Create particle buffer (using raw Vulkan for now since VMA is disabled)
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = bufferSize;
    bufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | 
                       VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                       VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    
    if (vkCreateBuffer(m_context->getDevice(), &bufferInfo, nullptr, &m_particleBuffer) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create particle buffer!");
    }
    
    // Allocate memory for buffer
    VkMemoryRequirements memRequirements;
    vkGetBufferMemoryRequirements(m_context->getDevice(), m_particleBuffer, &memRequirements);
    
    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = findMemoryType(memRequirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    
    if (vkAllocateMemory(m_context->getDevice(), &allocInfo, nullptr, &m_particleBufferMemory) != VK_SUCCESS) {
        throw std::runtime_error("Failed to allocate particle buffer memory!");
    }
    
    vkBindBufferMemory(m_context->getDevice(), m_particleBuffer, m_particleBufferMemory, 0);
}

void ParticleSystem::createComputePipeline() {
    // Load compute shader
    auto computeShaderCode = readFile("shaders/particle.comp.spv");
    
    VkShaderModuleCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    createInfo.codeSize = computeShaderCode.size();
    createInfo.pCode = reinterpret_cast<const uint32_t*>(computeShaderCode.data());
    
    VkShaderModule computeShaderModule;
    if (vkCreateShaderModule(m_context->getDevice(), &createInfo, nullptr, &computeShaderModule) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create compute shader module!");
    }
    
    // Create compute pipeline layout
    VkDescriptorSetLayoutBinding layoutBinding{};
    layoutBinding.binding = 0;
    layoutBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    layoutBinding.descriptorCount = 1;
    layoutBinding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    
    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &layoutBinding;
    
    vkCreateDescriptorSetLayout(m_context->getDevice(), &layoutInfo, nullptr, &m_computeDescriptorSetLayout);
    
    // Push constants for time and deltaTime
    VkPushConstantRange pushConstantRange{};
    pushConstantRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushConstantRange.offset = 0;
    pushConstantRange.size = sizeof(ComputePushConstants);
    
    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &m_computeDescriptorSetLayout;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushConstantRange;
    
    vkCreatePipelineLayout(m_context->getDevice(), &pipelineLayoutInfo, nullptr, &m_computePipelineLayout);
    
    // Create compute pipeline
    VkPipelineShaderStageCreateInfo shaderStageInfo{};
    shaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shaderStageInfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    shaderStageInfo.module = computeShaderModule;
    shaderStageInfo.pName = "main";
    
    VkComputePipelineCreateInfo computePipelineInfo{};
    computePipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    computePipelineInfo.stage = shaderStageInfo;
    computePipelineInfo.layout = m_computePipelineLayout;
    
    vkCreateComputePipelines(m_context->getDevice(), VK_NULL_HANDLE, 1, 
        &computePipelineInfo, nullptr, &m_computePipeline);
    
    vkDestroyShaderModule(m_context->getDevice(), computeShaderModule, nullptr);
}

void ParticleSystem::createSPHPipeline() {
    // Load SPH compute shader
    auto sphShaderCode = readFile("shaders/sph_simple.comp.spv");
    
    VkShaderModuleCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    createInfo.codeSize = sphShaderCode.size();
    createInfo.pCode = reinterpret_cast<const uint32_t*>(sphShaderCode.data());
    
    VkShaderModule sphShaderModule;
    if (vkCreateShaderModule(m_context->getDevice(), &createInfo, nullptr, &sphShaderModule) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create SPH shader module!");
    }
    
    // Create SPH pipeline layout (same as regular compute)
    VkDescriptorSetLayoutBinding layoutBinding{};
    layoutBinding.binding = 0;
    layoutBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    layoutBinding.descriptorCount = 1;
    layoutBinding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    
    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &layoutBinding;
    
    vkCreateDescriptorSetLayout(m_context->getDevice(), &layoutInfo, nullptr, &m_sphDescriptorSetLayout);
    
    // Push constants for SPH parameters
    VkPushConstantRange pushConstantRange{};
    pushConstantRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushConstantRange.offset = 0;
    pushConstantRange.size = sizeof(SPHPushConstants);
    
    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &m_sphDescriptorSetLayout;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushConstantRange;
    
    vkCreatePipelineLayout(m_context->getDevice(), &pipelineLayoutInfo, nullptr, &m_sphPipelineLayout);
    
    // Create SPH compute pipeline
    VkPipelineShaderStageCreateInfo shaderStageInfo{};
    shaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shaderStageInfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    shaderStageInfo.module = sphShaderModule;
    shaderStageInfo.pName = "main";
    
    VkComputePipelineCreateInfo computePipelineInfo{};
    computePipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    computePipelineInfo.stage = shaderStageInfo;
    computePipelineInfo.layout = m_sphPipelineLayout;
    
    vkCreateComputePipelines(m_context->getDevice(), VK_NULL_HANDLE, 1, 
        &computePipelineInfo, nullptr, &m_sphPipeline);
    
    vkDestroyShaderModule(m_context->getDevice(), sphShaderModule, nullptr);
}

void ParticleSystem::createGraphicsPipeline() {
    // Load shaders
    auto vertShaderCode = readFile("shaders/particle.vert.spv");
    auto fragShaderCode = readFile("shaders/particle.frag.spv");
    
    VkShaderModule vertShaderModule = createShaderModule(vertShaderCode);
    VkShaderModule fragShaderModule = createShaderModule(fragShaderCode);
    
    VkPipelineShaderStageCreateInfo vertShaderStageInfo{};
    vertShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    vertShaderStageInfo.stage = VK_SHADER_STAGE_VERTEX_BIT;
    vertShaderStageInfo.module = vertShaderModule;
    vertShaderStageInfo.pName = "main";
    
    VkPipelineShaderStageCreateInfo fragShaderStageInfo{};
    fragShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    fragShaderStageInfo.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    fragShaderStageInfo.module = fragShaderModule;
    fragShaderStageInfo.pName = "main";
    
    VkPipelineShaderStageCreateInfo shaderStages[] = {vertShaderStageInfo, fragShaderStageInfo};
    
    // Vertex input - position and velocity from particle struct
    VkVertexInputBindingDescription bindingDescription{};
    bindingDescription.binding = 0;
    bindingDescription.stride = sizeof(Particle);
    bindingDescription.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    
    std::array<VkVertexInputAttributeDescription, 3> attributeDescriptions{};
    attributeDescriptions[0].binding = 0;
    attributeDescriptions[0].location = 0;
    attributeDescriptions[0].format = VK_FORMAT_R32G32B32_SFLOAT;
    attributeDescriptions[0].offset = offsetof(Particle, position);
    
    attributeDescriptions[1].binding = 0;
    attributeDescriptions[1].location = 1;
    attributeDescriptions[1].format = VK_FORMAT_R32G32B32_SFLOAT;
    attributeDescriptions[1].offset = offsetof(Particle, velocity);
    
    attributeDescriptions[2].binding = 0;
    attributeDescriptions[2].location = 2;
    attributeDescriptions[2].format = VK_FORMAT_R32_SFLOAT;
    attributeDescriptions[2].offset = offsetof(Particle, temperature);
    
    VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
    vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInputInfo.vertexBindingDescriptionCount = 1;
    vertexInputInfo.pVertexBindingDescriptions = &bindingDescription;
    vertexInputInfo.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributeDescriptions.size());
    vertexInputInfo.pVertexAttributeDescriptions = attributeDescriptions.data();
    
    // Input assembly - render as points
    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    inputAssembly.primitiveRestartEnable = VK_FALSE;
    
    // Viewport and scissor
    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = 1920.0f;  // Will be dynamic
    viewport.height = 1080.0f;
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    
    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = {1920, 1080};
    
    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.pViewports = &viewport;
    viewportState.scissorCount = 1;
    viewportState.pScissors = &scissor;
    
    // Rasterizer
    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.depthClampEnable = VK_FALSE;
    rasterizer.rasterizerDiscardEnable = VK_FALSE;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = VK_CULL_MODE_NONE;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer.depthBiasEnable = VK_FALSE;
    
    // Multisampling
    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.sampleShadingEnable = VK_FALSE;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    
    // Color blending - additive for glow effect
    VkPipelineColorBlendAttachmentState colorBlendAttachment{};
    colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    colorBlendAttachment.blendEnable = VK_TRUE;
    colorBlendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    colorBlendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;  // Additive blending
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
    std::vector<VkDynamicState> dynamicStates = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR
    };
    
    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
    dynamicState.pDynamicStates = dynamicStates.data();
    
    // Push constants for MVP matrix
    VkPushConstantRange pushConstantRange{};
    pushConstantRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pushConstantRange.offset = 0;
    pushConstantRange.size = sizeof(GraphicsPushConstants);
    
    // Pipeline layout
    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushConstantRange;
    
    vkCreatePipelineLayout(m_context->getDevice(), &pipelineLayoutInfo, nullptr, &m_graphicsPipelineLayout);
    
    // Create the graphics pipeline
    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = shaderStages;
    pipelineInfo.pVertexInputState = &vertexInputInfo;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisampling;
    pipelineInfo.pColorBlendState = &colorBlending;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = m_graphicsPipelineLayout;
    
    // For dynamic rendering
    VkPipelineRenderingCreateInfo renderingInfo{};
    renderingInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    renderingInfo.colorAttachmentCount = 1;
    VkFormat colorFormat = m_context->getSwapChainImageFormat();
    renderingInfo.pColorAttachmentFormats = &colorFormat;
    pipelineInfo.pNext = &renderingInfo;
    
    vkCreateGraphicsPipelines(m_context->getDevice(), VK_NULL_HANDLE, 1,
        &pipelineInfo, nullptr, &m_graphicsPipeline);
    
    vkDestroyShaderModule(m_context->getDevice(), vertShaderModule, nullptr);
    vkDestroyShaderModule(m_context->getDevice(), fragShaderModule, nullptr);
}

void ParticleSystem::createDescriptorSets() {
    // Create descriptor pool for both compute and SPH pipelines
    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    poolSize.descriptorCount = 2; // One for compute, one for SPH
    
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    poolInfo.maxSets = 2;
    
    vkCreateDescriptorPool(m_context->getDevice(), &poolInfo, nullptr, &m_descriptorPool);
    
    // Allocate compute descriptor set
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = m_descriptorPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &m_computeDescriptorSetLayout;
    
    vkAllocateDescriptorSets(m_context->getDevice(), &allocInfo, &m_computeDescriptorSet);
    
    // Allocate SPH descriptor set
    allocInfo.pSetLayouts = &m_sphDescriptorSetLayout;
    vkAllocateDescriptorSets(m_context->getDevice(), &allocInfo, &m_sphDescriptorSet);
    
    // Create buffer info for both descriptor sets
    VkDescriptorBufferInfo bufferInfo{};
    bufferInfo.buffer = m_particleBuffer;
    bufferInfo.offset = 0;
    bufferInfo.range = sizeof(Particle) * m_particleCount;
    
    // Update compute descriptor set
    VkWriteDescriptorSet descriptorWrites[2] = {};
    
    descriptorWrites[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptorWrites[0].dstSet = m_computeDescriptorSet;
    descriptorWrites[0].dstBinding = 0;
    descriptorWrites[0].dstArrayElement = 0;
    descriptorWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    descriptorWrites[0].descriptorCount = 1;
    descriptorWrites[0].pBufferInfo = &bufferInfo;
    
    // Update SPH descriptor set
    descriptorWrites[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptorWrites[1].dstSet = m_sphDescriptorSet;
    descriptorWrites[1].dstBinding = 0;
    descriptorWrites[1].dstArrayElement = 0;
    descriptorWrites[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    descriptorWrites[1].descriptorCount = 1;
    descriptorWrites[1].pBufferInfo = &bufferInfo;
    
    vkUpdateDescriptorSets(m_context->getDevice(), 2, descriptorWrites, 0, nullptr);
}

void ParticleSystem::initializeParticles() {
    std::vector<Particle> particles(m_particleCount);
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_real_distribution<float> angleDist(0.0f, 2.0f * 3.14159f);
    std::uniform_real_distribution<float> radiusDist(0.0f, 1.0f);
    std::normal_distribution<float> velDist(0.0f, 0.5f);
    std::uniform_real_distribution<float> heightDist(-0.5f, 0.5f);
    
    for (uint32_t i = 0; i < m_particleCount; i++) {
        // Create a more cloud-like distribution with varying density
        float theta = angleDist(gen);
        float phi = acos(1.0f - 2.0f * radiusDist(gen)); // Proper spherical distribution
        
        // Use a power function for radius to create density variation
        float r = pow(radiusDist(gen), 0.5f) * 6.0f; // Denser in center, sparse at edges
        
        // Add some disc bias for accretion disc tendency
        float discBias = 1.0f - abs(heightDist(gen)) * 0.3f;
        
        particles[i].position = glm::vec3(
            r * sin(phi) * cos(theta) * discBias,
            r * cos(phi) * heightDist(gen) * 2.0f, // Flatter distribution
            r * sin(phi) * sin(theta) * discBias
        );
        
        // Give particles some initial orbital velocity
        glm::vec3 toCenter = -particles[i].position;
        if (length(toCenter) > 0.1f) {
            glm::vec3 tangent = normalize(cross(toCenter, glm::vec3(0, 1, 0)));
            float orbitalSpeed = sqrt(2.0f / (length(toCenter) + 1.0f)) * 0.5f;
            particles[i].velocity = tangent * orbitalSpeed;
        }
        
        // Add random perturbation
        particles[i].velocity += glm::vec3(
            velDist(gen) * 0.2f,
            velDist(gen) * 0.1f,
            velDist(gen) * 0.2f
        );
        
        particles[i].temperature = 0.5f + radiusDist(gen) * 0.5f;
        particles[i].density = 1.0f;
    }
    
    // Copy to GPU buffer
    void* data;
    vkMapMemory(m_context->getDevice(), m_particleBufferMemory, 0, 
        sizeof(Particle) * m_particleCount, 0, &data);
    memcpy(data, particles.data(), sizeof(Particle) * m_particleCount);
    vkUnmapMemory(m_context->getDevice(), m_particleBufferMemory);
}

void ParticleSystem::update(VkCommandBuffer commandBuffer, float deltaTime, float time) {
    if (m_sphMode) {
        // Use SPH compute pipeline
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_sphPipeline);
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
            m_sphPipelineLayout, 0, 1, &m_sphDescriptorSet, 0, nullptr);
        
        // Update SPH push constants with current time values
        m_sphParams.deltaTime = deltaTime;
        m_sphParams.time = time;
        m_sphParams.particleCount = m_activeParticleCount;
        
        vkCmdPushConstants(commandBuffer, m_sphPipelineLayout,
            VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(SPHPushConstants), &m_sphParams);
    } else {
        // Use regular compute pipeline
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_computePipeline);
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
            m_computePipelineLayout, 0, 1, &m_computeDescriptorSet, 0, nullptr);
        
        ComputePushConstants pushConstants{};
        pushConstants.deltaTime = deltaTime;
        pushConstants.time = time;
        pushConstants.particleCount = m_activeParticleCount;
        pushConstants.gravityStrength = m_gravityStrength;
        pushConstants.gravityCenter = m_gravityCenter;
        pushConstants.turbulenceStrength = m_turbulenceStrength;
        pushConstants.dampingFactor = m_dampingFactor;
        pushConstants.constraintShape = m_constraintShape;
        pushConstants.constraintRadius = m_constraintRadius;
        pushConstants.constraintThickness = m_constraintThickness;
        pushConstants.blackHoleMass = m_blackHoleMass;
        pushConstants.alphaViscosity = m_alphaViscosity;
        pushConstants.angularMomentumBoost = m_angularMomentumBoost;
        
        vkCmdPushConstants(commandBuffer, m_computePipelineLayout,
            VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ComputePushConstants), &pushConstants);
    }
    
    // Dispatch with 64 particles per workgroup
    uint32_t groupCount = (m_activeParticleCount + 63) / 64;
    vkCmdDispatch(commandBuffer, groupCount, 1, 1);
    
    // Memory barrier to ensure compute shader finishes before rendering
    VkMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
    
    vkCmdPipelineBarrier(commandBuffer,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,
        0, 1, &barrier, 0, nullptr, 0, nullptr);
}

void ParticleSystem::render(VkCommandBuffer commandBuffer, const glm::mat4& viewProj, 
                           VkImageView depthImageView, const glm::vec2& screenSize,
                           float softParticleFactor) {
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_graphicsPipeline);
    
    // Set viewport and scissor
    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = static_cast<float>(m_context->getSwapChainExtent().width);
    viewport.height = static_cast<float>(m_context->getSwapChainExtent().height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
    
    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = m_context->getSwapChainExtent();
    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
    
    // Push constants
    GraphicsPushConstants pushConstants{};
    pushConstants.viewProj = viewProj;
    pushConstants.particleSize = 2.0f; // Much larger particles for volumetric effect
    pushConstants.softParticleFactor = softParticleFactor;
    pushConstants.screenSize = screenSize;
    
    vkCmdPushConstants(commandBuffer, m_graphicsPipelineLayout,
        VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(GraphicsPushConstants), &pushConstants);
    
    // Bind vertex buffer
    VkBuffer vertexBuffers[] = {m_particleBuffer};
    VkDeviceSize offsets[] = {0};
    vkCmdBindVertexBuffers(commandBuffer, 0, 1, vertexBuffers, offsets);
    
    // Draw particles
    vkCmdDraw(commandBuffer, m_activeParticleCount, 1, 0, 0);
}

void ParticleSystem::setSPHParameters(float smoothingRadius, float restDensity, 
                                     float pressureConstant, float viscosity, float mass) {
    m_sphParams.smoothingRadius = smoothingRadius;
    m_sphParams.restDensity = restDensity;
    m_sphParams.pressureConstant = pressureConstant;
    m_sphParams.viscosity = viscosity;
    m_sphParams.mass = mass;
}

void ParticleSystem::setActiveParticleCount(uint32_t count) {
    m_activeParticleCount = std::min(count, m_particleCount); // Can't exceed allocated particles
    std::cout << "Active particles: " << m_activeParticleCount << " / " << m_particleCount << std::endl;
}

void ParticleSystem::cleanup() {
    vkDestroyDescriptorPool(m_context->getDevice(), m_descriptorPool, nullptr);
    
    // Clean up regular compute pipeline
    vkDestroyDescriptorSetLayout(m_context->getDevice(), m_computeDescriptorSetLayout, nullptr);
    vkDestroyPipeline(m_context->getDevice(), m_computePipeline, nullptr);
    vkDestroyPipelineLayout(m_context->getDevice(), m_computePipelineLayout, nullptr);
    
    // Clean up SPH pipeline
    vkDestroyDescriptorSetLayout(m_context->getDevice(), m_sphDescriptorSetLayout, nullptr);
    vkDestroyPipeline(m_context->getDevice(), m_sphPipeline, nullptr);
    vkDestroyPipelineLayout(m_context->getDevice(), m_sphPipelineLayout, nullptr);
    
    // Clean up graphics pipeline
    vkDestroyPipeline(m_context->getDevice(), m_graphicsPipeline, nullptr);
    vkDestroyPipelineLayout(m_context->getDevice(), m_graphicsPipelineLayout, nullptr);
    
    // Clean up buffer
    vkDestroyBuffer(m_context->getDevice(), m_particleBuffer, nullptr);
    vkFreeMemory(m_context->getDevice(), m_particleBufferMemory, nullptr);
}

uint32_t ParticleSystem::findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties memProperties;
    vkGetPhysicalDeviceMemoryProperties(m_context->getPhysicalDevice(), &memProperties);
    
    for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
        if ((typeFilter & (1 << i)) && 
            (memProperties.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    
    throw std::runtime_error("Failed to find suitable memory type!");
}

VkShaderModule ParticleSystem::createShaderModule(const std::vector<char>& code) {
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

std::vector<char> ParticleSystem::readFile(const std::string& filename) {
    std::ifstream file(filename, std::ios::ate | std::ios::binary);
    
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open file: " + filename);
    }
    
    size_t fileSize = (size_t)file.tellg();
    std::vector<char> buffer(fileSize);
    
    file.seekg(0);
    file.read(buffer.data(), fileSize);
    file.close();
    
    return buffer;
}

} // namespace plasma