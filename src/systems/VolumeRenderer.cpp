#include "VolumeRenderer.h"
#include "../renderer/VulkanContext.h"
#include <iostream>
#include <fstream>
#include <array>
#include <cstring>
#include <algorithm>

namespace plasma {

VolumeRenderer::VolumeRenderer(VulkanContext* context, const VolumeParams& params)
    : m_context(context), m_params(params) {
    
    std::cout << "Creating volume renderer..." << std::endl;
    std::cout << "Grid: " << m_params.gridDimensions.x << "x" << m_params.gridDimensions.y << "x" << m_params.gridDimensions.z << std::endl;
    std::cout << "Density splat mode: " << (m_useAtomicScatter ? "per-particle atomic scatter" : "voxel gather (fallback)") << std::endl;
    
    m_useAtomicScatter = m_context->supportsShaderAtomicFloat();

    createDensityGrid();
    createDensitySplatPipeline();
    createVolumeRenderPipeline();
    createDescriptorSets();
    
    std::cout << "Volume renderer created successfully!" << std::endl;
}

VolumeRenderer::~VolumeRenderer() {
    cleanup();
}

void VolumeRenderer::createDensityGrid() {
    // Calculate 3D texture dimensions
    uint32_t width = m_params.gridDimensions.x;
    uint32_t height = m_params.gridDimensions.y;
    uint32_t depth = m_params.gridDimensions.z;
    
    // Create 3D image for density storage
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_3D;
    imageInfo.extent.width = width;
    imageInfo.extent.height = height;
    imageInfo.extent.depth = depth;
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = VK_FORMAT_R32_SFLOAT;  // Single channel float
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    
    if (vkCreateImage(m_context->getDevice(), &imageInfo, nullptr, &m_densityImage) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create density image!");
    }
    
    // Allocate memory for image
    VkMemoryRequirements memRequirements;
    vkGetImageMemoryRequirements(m_context->getDevice(), m_densityImage, &memRequirements);
    
    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = findMemoryType(memRequirements.memoryTypeBits, 
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    
    if (vkAllocateMemory(m_context->getDevice(), &allocInfo, nullptr, &m_densityImageMemory) != VK_SUCCESS) {
        throw std::runtime_error("Failed to allocate density image memory!");
    }
    
    vkBindImageMemory(m_context->getDevice(), m_densityImage, m_densityImageMemory, 0);
    
    // Create image view for storage access
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = m_densityImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_3D;
    viewInfo.format = VK_FORMAT_R32_SFLOAT;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;
    
    if (vkCreateImageView(m_context->getDevice(), &viewInfo, nullptr, &m_densityImageView) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create density image view!");
    }
    
    // Create sampler for volume rendering
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.anisotropyEnable = VK_FALSE;
    samplerInfo.maxAnisotropy = 1.0f;
    samplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    samplerInfo.unnormalizedCoordinates = VK_FALSE;
    samplerInfo.compareEnable = VK_FALSE;
    samplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.mipLodBias = 0.0f;
    samplerInfo.minLod = 0.0f;
    samplerInfo.maxLod = 0.0f;
    
    if (vkCreateSampler(m_context->getDevice(), &samplerInfo, nullptr, &m_densitySampler) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create density sampler!");
    }
}

void VolumeRenderer::createDensitySplatPipeline() {
    // Load density splat compute shader (choose optimal implementation)
    // If VK_EXT_shader_atomic_float is supported, use per-particle atomic scatter which is O(N * r^3)
    // Otherwise, fall back to voxel-gather (O(V * N))
    const char* splatPath = m_useAtomicScatter ?
        "shaders/density_splat_scatter.comp.spv" :
        "shaders/density_splat.comp.spv";
    auto computeShaderCode = readFile(splatPath);
    
    VkShaderModuleCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    createInfo.codeSize = computeShaderCode.size();
    createInfo.pCode = reinterpret_cast<const uint32_t*>(computeShaderCode.data());
    
    if (vkCreateShaderModule(m_context->getDevice(), &createInfo, nullptr, &m_densitySplatShader) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create density splat shader module!");
    }
    
    // Create descriptor set layout
    std::array<VkDescriptorSetLayoutBinding, 2> bindings{};
    
    // Binding 0: Particle buffer
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    
    // Binding 1: Density image
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    
    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT;
    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutInfo.pBindings = bindings.data();
    
    if (vkCreateDescriptorSetLayout(m_context->getDevice(), &layoutInfo, nullptr, 
        &m_densitySplatDescriptorSetLayout) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create density splat descriptor set layout!");
    }
    
    // Push constants
    VkPushConstantRange pushConstantRange{};
    pushConstantRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushConstantRange.offset = 0;
    pushConstantRange.size = sizeof(DensityPushConstants);
    
    // Create pipeline layout
    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &m_densitySplatDescriptorSetLayout;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushConstantRange;
    
    if (vkCreatePipelineLayout(m_context->getDevice(), &pipelineLayoutInfo, nullptr, 
        &m_densitySplatPipelineLayout) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create density splat pipeline layout!");
    }
    
    // Create compute pipeline
    VkPipelineShaderStageCreateInfo shaderStageInfo{};
    shaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shaderStageInfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    shaderStageInfo.module = m_densitySplatShader;
    shaderStageInfo.pName = "main";
    
    VkComputePipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipelineInfo.stage = shaderStageInfo;
    pipelineInfo.layout = m_densitySplatPipelineLayout;
    
    if (vkCreateComputePipelines(m_context->getDevice(), VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, 
        &m_densitySplatPipeline) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create density splat pipeline!");
    }
}

void VolumeRenderer::createVolumeRenderPipeline() {
    // Load shaders
    auto vertShaderCode = readFile("shaders/volume.vert.spv");
    auto fragShaderCode = readFile("shaders/volume.frag.spv");
    
    VkShaderModuleCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    
    // Vertex shader
    createInfo.codeSize = vertShaderCode.size();
    createInfo.pCode = reinterpret_cast<const uint32_t*>(vertShaderCode.data());
    if (vkCreateShaderModule(m_context->getDevice(), &createInfo, nullptr, &m_volumeVertShader) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create volume vertex shader module!");
    }
    
    // Fragment shader
    createInfo.codeSize = fragShaderCode.size();
    createInfo.pCode = reinterpret_cast<const uint32_t*>(fragShaderCode.data());
    if (vkCreateShaderModule(m_context->getDevice(), &createInfo, nullptr, &m_volumeFragShader) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create volume fragment shader module!");
    }
    
    // Create descriptor set layout for volume rendering
    VkDescriptorSetLayoutBinding samplerLayoutBinding{};
    samplerLayoutBinding.binding = 0;
    samplerLayoutBinding.descriptorCount = 1;
    samplerLayoutBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    samplerLayoutBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    
    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT;
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &samplerLayoutBinding;
    
    if (vkCreateDescriptorSetLayout(m_context->getDevice(), &layoutInfo, nullptr, 
        &m_volumeDescriptorSetLayout) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create volume descriptor set layout!");
    }
    
    // Push constants for volume rendering
    VkPushConstantRange pushConstantRange{};
    pushConstantRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    pushConstantRange.offset = 0;
    pushConstantRange.size = sizeof(VolumePushConstants);
    
    // Pipeline layout
    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &m_volumeDescriptorSetLayout;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushConstantRange;
    
    if (vkCreatePipelineLayout(m_context->getDevice(), &pipelineLayoutInfo, nullptr, 
        &m_volumePipelineLayout) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create volume pipeline layout!");
    }
    
    // Shader stages
    VkPipelineShaderStageCreateInfo shaderStages[2] = {};
    
    shaderStages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shaderStages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    shaderStages[0].module = m_volumeVertShader;
    shaderStages[0].pName = "main";
    
    shaderStages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shaderStages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    shaderStages[1].module = m_volumeFragShader;
    shaderStages[1].pName = "main";
    
    // No vertex input (fullscreen triangle generated in vertex shader)
    VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
    vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInputInfo.vertexBindingDescriptionCount = 0;
    vertexInputInfo.vertexAttributeDescriptionCount = 0;
    
    // Input assembly
    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    inputAssembly.primitiveRestartEnable = VK_FALSE;
    
    // Viewport (dynamic)
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
    rasterizer.cullMode = VK_CULL_MODE_NONE;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer.depthBiasEnable = VK_FALSE;
    
    // Multisampling
    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.sampleShadingEnable = VK_FALSE;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    
    // Color blending (additive for volumetric effect)
    VkPipelineColorBlendAttachmentState colorBlendAttachment{};
    colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | 
                                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    colorBlendAttachment.blendEnable = VK_TRUE;
    colorBlendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    colorBlendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
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
    
    // Color attachment format for dynamic rendering
    VkFormat colorFormat = m_context->getSwapChainImageFormat();
    VkPipelineRenderingCreateInfo renderingInfo{};
    renderingInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    renderingInfo.colorAttachmentCount = 1;
    renderingInfo.pColorAttachmentFormats = &colorFormat;
    
    // Create graphics pipeline
    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.pNext = &renderingInfo; // Dynamic rendering
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = shaderStages;
    pipelineInfo.pVertexInputState = &vertexInputInfo;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisampling;
    pipelineInfo.pColorBlendState = &colorBlending;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = m_volumePipelineLayout;
    pipelineInfo.basePipelineHandle = VK_NULL_HANDLE;
    
    if (vkCreateGraphicsPipelines(m_context->getDevice(), VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, 
        &m_volumePipeline) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create volume graphics pipeline!");
    }
}

void VolumeRenderer::createDescriptorSets() {
    // Push descriptors eliminate the need for descriptor pools, sets, and updates!
    // All descriptor data is pushed directly into command buffers during rendering.
    // This function is now a no-op but kept for API compatibility.
}

void VolumeRenderer::updateDensityGrid(VkCommandBuffer cmd, VkBuffer particleBuffer, uint32_t particleCount) {
    // 1) Clear density image to zero each frame to avoid accumulation artifacts
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = m_densityImage;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;

    // Previous layout might be SHADER_READ_ONLY_OPTIMAL from last frame; we don't need its contents.
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);

    VkClearColorValue zero{}; // all zeros
    VkImageSubresourceRange clearRange{};
    clearRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    clearRange.baseMipLevel = 0;
    clearRange.levelCount = 1;
    clearRange.baseArrayLayer = 0;
    clearRange.layerCount = 1;
    vkCmdClearColorImage(cmd, m_densityImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &clearRange);

    // Transition to GENERAL for compute writes
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);
    
    // Push descriptors directly to command buffer (Vulkan 1.4)
    VkDescriptorBufferInfo bufferInfo{};
    bufferInfo.buffer = particleBuffer;
    bufferInfo.offset = 0;
    bufferInfo.range = VK_WHOLE_SIZE;
    
    VkDescriptorImageInfo imageInfo{};
    imageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    imageInfo.imageView = m_densityImageView;
    
    std::array<VkWriteDescriptorSet, 2> descriptorWrites{};
    
    descriptorWrites[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptorWrites[0].dstSet = VK_NULL_HANDLE; // Ignored for push descriptors
    descriptorWrites[0].dstBinding = 0;
    descriptorWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    descriptorWrites[0].descriptorCount = 1;
    descriptorWrites[0].pBufferInfo = &bufferInfo;
    
    descriptorWrites[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptorWrites[1].dstSet = VK_NULL_HANDLE; // Ignored for push descriptors
    descriptorWrites[1].dstBinding = 1;
    descriptorWrites[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    descriptorWrites[1].descriptorCount = 1;
    descriptorWrites[1].pImageInfo = &imageInfo;
    
    // Bind compute pipeline and push descriptors (no more vkUpdateDescriptorSets!)
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_densitySplatPipeline);
    vkCmdPushDescriptorSet(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_densitySplatPipelineLayout, 
        0, static_cast<uint32_t>(descriptorWrites.size()), descriptorWrites.data());
    
    // Push constants
    DensityPushConstants pushConstants{};
    pushConstants.gridOrigin = m_params.gridOrigin;
    pushConstants.voxelSize = m_params.voxelSize;
    pushConstants.gridDimensions = m_params.gridDimensions;
    pushConstants.particleCount = particleCount;
    pushConstants.splatRadius = m_params.splatRadius;
    
    vkCmdPushConstants(cmd, m_densitySplatPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 
        0, sizeof(DensityPushConstants), &pushConstants);
    
    // Dispatch
    if (m_useAtomicScatter) {
        // Per-particle scatter: 128 threads per group
        uint32_t groups = (particleCount + 127u) / 128u;
        vkCmdDispatch(cmd, groups, 1, 1);
    } else {
        // Fallback voxel-gather: 4x4x4 workgroups over the volume
        uint32_t groupsX = (m_params.gridDimensions.x + 3) / 4;
        uint32_t groupsY = (m_params.gridDimensions.y + 3) / 4;
        uint32_t groupsZ = (m_params.gridDimensions.z + 3) / 4;
        vkCmdDispatch(cmd, groupsX, groupsY, groupsZ);
    }
    
    // Barrier for volume rendering
    barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 
        0, 0, nullptr, 0, nullptr, 1, &barrier);
}

void VolumeRenderer::render(VkCommandBuffer cmd, const glm::mat4& viewProj, const glm::vec3& cameraPos, bool highQuality) {
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
    
    // Bind volume rendering pipeline and push descriptors
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_volumePipeline);
    
    // Push descriptor for density texture (Vulkan 1.4)
    VkDescriptorImageInfo imageInfo{};
    imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageInfo.imageView = m_densityImageView;
    imageInfo.sampler = m_densitySampler;
    
    VkWriteDescriptorSet descriptorWrite{};
    descriptorWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptorWrite.dstSet = VK_NULL_HANDLE; // Ignored for push descriptors
    descriptorWrite.dstBinding = 0;
    descriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    descriptorWrite.descriptorCount = 1;
    descriptorWrite.pImageInfo = &imageInfo;
    
    vkCmdPushDescriptorSet(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_volumePipelineLayout, 
        0, 1, &descriptorWrite);
    
    // Push constants for ray marching with quality override
    VolumePushConstants pushConstants{};
    pushConstants.viewProjInv = glm::inverse(viewProj);
    pushConstants.cameraPos = cameraPos;
    pushConstants.gridOrigin = m_params.gridOrigin;
    pushConstants.voxelSize = m_params.voxelSize;
    pushConstants.gridDimensions = m_params.gridDimensions;
    
    // Override ray marching quality for high-quality mode
    if (highQuality) {
        VolumeParams hqParams = VolumeParams::getRecordingQuality();
        pushConstants.maxSteps = hqParams.maxRaySteps;     // 256 instead of 128
        pushConstants.stepSize = hqParams.rayStepSize;     // 0.1 instead of 0.2  
        pushConstants.densityScale = hqParams.densityScale; // 0.5 instead of 0.7
    } else {
        pushConstants.maxSteps = m_params.maxRaySteps;
        pushConstants.stepSize = m_params.rayStepSize;
        pushConstants.densityScale = m_params.densityScale;
    }
    
    vkCmdPushConstants(cmd, m_volumePipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 
        0, sizeof(VolumePushConstants), &pushConstants);
    
    // Draw fullscreen triangle
    vkCmdDraw(cmd, 3, 1, 0, 0);
}

void VolumeRenderer::cleanup() {
    // No descriptor pool needed with push descriptors!
    
    if (m_densitySplatDescriptorSetLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(m_context->getDevice(), m_densitySplatDescriptorSetLayout, nullptr);
        m_densitySplatDescriptorSetLayout = VK_NULL_HANDLE;
    }
    
    if (m_volumeDescriptorSetLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(m_context->getDevice(), m_volumeDescriptorSetLayout, nullptr);
        m_volumeDescriptorSetLayout = VK_NULL_HANDLE;
    }
    
    if (m_densitySplatPipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(m_context->getDevice(), m_densitySplatPipeline, nullptr);
        m_densitySplatPipeline = VK_NULL_HANDLE;
    }
    
    if (m_densitySplatPipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(m_context->getDevice(), m_densitySplatPipelineLayout, nullptr);
        m_densitySplatPipelineLayout = VK_NULL_HANDLE;
    }
    
    if (m_volumePipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(m_context->getDevice(), m_volumePipeline, nullptr);
        m_volumePipeline = VK_NULL_HANDLE;
    }
    
    if (m_volumePipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(m_context->getDevice(), m_volumePipelineLayout, nullptr);
        m_volumePipelineLayout = VK_NULL_HANDLE;
    }
    
    if (m_densitySplatShader != VK_NULL_HANDLE) {
        vkDestroyShaderModule(m_context->getDevice(), m_densitySplatShader, nullptr);
        m_densitySplatShader = VK_NULL_HANDLE;
    }
    
    if (m_volumeVertShader != VK_NULL_HANDLE) {
        vkDestroyShaderModule(m_context->getDevice(), m_volumeVertShader, nullptr);
        m_volumeVertShader = VK_NULL_HANDLE;
    }
    
    if (m_volumeFragShader != VK_NULL_HANDLE) {
        vkDestroyShaderModule(m_context->getDevice(), m_volumeFragShader, nullptr);
        m_volumeFragShader = VK_NULL_HANDLE;
    }
    
    if (m_densitySampler != VK_NULL_HANDLE) {
        vkDestroySampler(m_context->getDevice(), m_densitySampler, nullptr);
        m_densitySampler = VK_NULL_HANDLE;
    }
    
    if (m_densityImageView != VK_NULL_HANDLE) {
        vkDestroyImageView(m_context->getDevice(), m_densityImageView, nullptr);
        m_densityImageView = VK_NULL_HANDLE;
    }
    
    if (m_densityImage != VK_NULL_HANDLE) {
        vkDestroyImage(m_context->getDevice(), m_densityImage, nullptr);
        m_densityImage = VK_NULL_HANDLE;
    }
    
    if (m_densityImageMemory != VK_NULL_HANDLE) {
        vkFreeMemory(m_context->getDevice(), m_densityImageMemory, nullptr);
        m_densityImageMemory = VK_NULL_HANDLE;
    }
}

uint32_t VolumeRenderer::findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) {
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

VkShaderModule VolumeRenderer::createShaderModule(const std::vector<char>& code) {
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

std::vector<char> VolumeRenderer::readFile(const std::string& filename) {
    std::ifstream file(filename, std::ios::ate | std::ios::binary);
    
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open file: " + filename);
    }
    
    size_t fileSize = (size_t) file.tellg();
    std::vector<char> buffer(fileSize);
    
    file.seekg(0);
    file.read(buffer.data(), fileSize);
    file.close();
    
    return buffer;
}

} // namespace plasma
