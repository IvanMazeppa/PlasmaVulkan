#include "RTVolumeRenderer.h"
#include "../renderer/VulkanContext.h"
#include <iostream>
#include <fstream>
#include <vector>
#include <stdexcept>
#include <algorithm>
#include <cmath>

RTVolumeRenderer::RTVolumeRenderer(plasma::VulkanContext* context)
    : m_context(context), m_extent(context->getSwapChainExtent()) {
    std::cout << "[RTV] RTVolumeRenderer initializing..." << std::endl;

    try {
        // Calculate mip levels for 3D density grid
        uint32_t maxDim = std::max({m_params.gridDimensions.x, m_params.gridDimensions.y, m_params.gridDimensions.z});
        m_densityMipLevels = static_cast<uint32_t>(std::floor(std::log2(maxDim))) + 1;

        std::cout << "[RTV] Density grid: " << m_params.gridDimensions.x << "x"
                  << m_params.gridDimensions.y << "x" << m_params.gridDimensions.z
                  << " (" << m_densityMipLevels << " mip levels)" << std::endl;

        createDensityGrid();
        createDensitySplatPipeline();
        createHDRTarget();
        createCompositePass();

        std::cout << "[RTV] RTVolumeRenderer initialized successfully!" << std::endl;
    } catch (const std::exception& e) {
        std::cout << "[RTV] Initialization failed: " << e.what() << std::endl;
        // CRITICAL: Wait for GPU to finish any pending operations before cleanup
        vkDeviceWaitIdle(m_context->getDevice());
        // Clean up any partially created resources
        cleanup();
        throw; // Re-throw so Application can handle gracefully
    }
}

RTVolumeRenderer::~RTVolumeRenderer() {
    // CRITICAL: Wait for GPU to finish any pending operations before cleanup
    vkDeviceWaitIdle(m_context->getDevice());
    cleanup();
}

void RTVolumeRenderer::renderToHDR(VkCommandBuffer cmd, const glm::mat4& viewProj, const glm::vec3& cameraPos) {
    // RTV-2001: For now, just clear the HDR target to black
    // This will be replaced with actual ray marching in RTV-2003

    VkImageMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
    barrier.srcAccessMask = 0;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = m_hdrImage;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;

    VkDependencyInfo dependencyInfo{};
    dependencyInfo.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependencyInfo.imageMemoryBarrierCount = 1;
    dependencyInfo.pImageMemoryBarriers = &barrier;

    vkCmdPipelineBarrier2(cmd, &dependencyInfo);

    // Clear to black (will show as black screen until ray marcher is implemented)
    VkClearColorValue clearColor = {{0.0f, 0.0f, 0.0f, 0.0f}};
    VkImageSubresourceRange clearRange{};
    clearRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    clearRange.baseMipLevel = 0;
    clearRange.levelCount = 1;
    clearRange.baseArrayLayer = 0;
    clearRange.layerCount = 1;

    vkCmdClearColorImage(cmd, m_hdrImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clearColor, 1, &clearRange);

    // Transition to shader read optimal for composite pass
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    vkCmdPipelineBarrier2(cmd, &dependencyInfo);
}

void RTVolumeRenderer::updateDensityGrid(VkCommandBuffer cmd, VkBuffer particleBuffer, uint32_t particleCount) {
    // Clear density grid to zero first
    VkClearColorValue clearColor = {{0.0f, 0.0f, 0.0f, 0.0f}};
    VkImageSubresourceRange clearRange{};
    clearRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    clearRange.baseMipLevel = 0;
    clearRange.levelCount = 1;  // Only clear mip level 0
    clearRange.baseArrayLayer = 0;
    clearRange.layerCount = 1;

    // Transition to TRANSFER_DST for clearing
    VkImageMemoryBarrier2 clearBarrier{};
    clearBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    clearBarrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    clearBarrier.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
    clearBarrier.dstStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
    clearBarrier.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    clearBarrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    clearBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    clearBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    clearBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    clearBarrier.image = m_densityImage;
    clearBarrier.subresourceRange = clearRange;

    VkDependencyInfo clearDependency{};
    clearDependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    clearDependency.imageMemoryBarrierCount = 1;
    clearDependency.pImageMemoryBarriers = &clearBarrier;

    vkCmdPipelineBarrier2(cmd, &clearDependency);

    // Clear the base mip level
    vkCmdClearColorImage(cmd, m_densityImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clearColor, 1, &clearRange);

    // Transition back to GENERAL for compute shader access
    clearBarrier.srcStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
    clearBarrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    clearBarrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    clearBarrier.dstAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
    clearBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    clearBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;

    vkCmdPipelineBarrier2(cmd, &clearDependency);

    // Bind density splat compute pipeline
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_densitySplatPipeline);

    // Set up push constants
    DensityPushConstants pushConstants{};
    pushConstants.gridOrigin = m_params.gridOrigin;
    pushConstants.voxelSize = m_params.voxelSize;
    pushConstants.gridDimensions = m_params.gridDimensions;
    pushConstants.particleCount = particleCount;
    pushConstants.splatRadius = m_params.splatRadius;

    vkCmdPushConstants(cmd, m_densitySplatPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pushConstants), &pushConstants);

    // TODO: Bind descriptor sets (particle buffer + density grid)
    // This will be implemented when we integrate with the particle system

    // Dispatch compute shader
    uint32_t workGroupSize = 64;  // Must match local_size_x in shader
    uint32_t workGroups = (particleCount + workGroupSize - 1) / workGroupSize;

    if (workGroups > 0) {  // Guard against empty dispatch
        vkCmdDispatch(cmd, workGroups, 1, 1);
    }

    // Memory barrier after density splatting
    VkMemoryBarrier2 memoryBarrier{};
    memoryBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    memoryBarrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    memoryBarrier.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
    memoryBarrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    memoryBarrier.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;

    VkDependencyInfo dependency{};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers = &memoryBarrier;

    vkCmdPipelineBarrier2(cmd, &dependency);

    static uint32_t frameCount = 0;
    if (frameCount % 120 == 0) {  // Throttled logging
        std::cout << "[RTV] Density grid updated - " << particleCount << " particles, "
                  << workGroups << " workgroups" << std::endl;
    }
    frameCount++;
}

void RTVolumeRenderer::generateMipChain(VkCommandBuffer cmd) {
    // Generate mip chain using vkCmdBlitImage for downsampling
    for (uint32_t mip = 1; mip < m_densityMipLevels; mip++) {
        // Transition source mip to TRANSFER_SRC
        VkImageMemoryBarrier2 srcBarrier{};
        srcBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        srcBarrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        srcBarrier.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
        srcBarrier.dstStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT;
        srcBarrier.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        srcBarrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        srcBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        srcBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        srcBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        srcBarrier.image = m_densityImage;
        srcBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        srcBarrier.subresourceRange.baseMipLevel = mip - 1;
        srcBarrier.subresourceRange.levelCount = 1;
        srcBarrier.subresourceRange.baseArrayLayer = 0;
        srcBarrier.subresourceRange.layerCount = 1;

        // Transition destination mip to TRANSFER_DST
        VkImageMemoryBarrier2 dstBarrier{};
        dstBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        dstBarrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        dstBarrier.srcAccessMask = 0;
        dstBarrier.dstStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT;
        dstBarrier.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        dstBarrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        dstBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        dstBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        dstBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        dstBarrier.image = m_densityImage;
        dstBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        dstBarrier.subresourceRange.baseMipLevel = mip;
        dstBarrier.subresourceRange.levelCount = 1;
        dstBarrier.subresourceRange.baseArrayLayer = 0;
        dstBarrier.subresourceRange.layerCount = 1;

        VkImageMemoryBarrier2 barriers[] = {srcBarrier, dstBarrier};
        VkDependencyInfo dependency{};
        dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dependency.imageMemoryBarrierCount = 2;
        dependency.pImageMemoryBarriers = barriers;

        vkCmdPipelineBarrier2(cmd, &dependency);

        // Blit from previous mip level to current mip level
        VkImageBlit blit{};
        blit.srcOffsets[0] = {0, 0, 0};
        blit.srcOffsets[1] = {
            static_cast<int32_t>(m_params.gridDimensions.x >> (mip - 1)),
            static_cast<int32_t>(m_params.gridDimensions.y >> (mip - 1)),
            static_cast<int32_t>(m_params.gridDimensions.z >> (mip - 1))
        };
        blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.srcSubresource.mipLevel = mip - 1;
        blit.srcSubresource.baseArrayLayer = 0;
        blit.srcSubresource.layerCount = 1;

        blit.dstOffsets[0] = {0, 0, 0};
        blit.dstOffsets[1] = {
            static_cast<int32_t>(m_params.gridDimensions.x >> mip),
            static_cast<int32_t>(m_params.gridDimensions.y >> mip),
            static_cast<int32_t>(m_params.gridDimensions.z >> mip)
        };
        blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.dstSubresource.mipLevel = mip;
        blit.dstSubresource.baseArrayLayer = 0;
        blit.dstSubresource.layerCount = 1;

        vkCmdBlitImage(cmd, m_densityImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       m_densityImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       1, &blit, VK_FILTER_LINEAR);

        // Transition both mip levels back to GENERAL
        srcBarrier.srcStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT;
        srcBarrier.srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        srcBarrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        srcBarrier.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
        srcBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        srcBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;

        dstBarrier.srcStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT;
        dstBarrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        dstBarrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        dstBarrier.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
        dstBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        dstBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;

        vkCmdPipelineBarrier2(cmd, &dependency);
    }

    static uint32_t frameCount = 0;
    if (frameCount % 120 == 0) {  // Throttled logging
        std::cout << "[RTV] Mip chain generated (" << m_densityMipLevels << " levels)" << std::endl;
    }
    frameCount++;
}

void RTVolumeRenderer::composite(VkCommandBuffer cmd) {
    // RTV-2001: Simple fullscreen pass to composite HDR to swapchain
    // This is a placeholder - will be enhanced with proper tone mapping later

    // For now, just log that composite would happen
    static uint32_t frameCount = 0;
    if (frameCount % 120 == 0) { // Throttled logging
        std::cout << "[RTV] Composite pass (HDR->swapchain) - frame " << frameCount << std::endl;
    }
    frameCount++;

    // TODO: Implement fullscreen triangle composite pass
    // This will copy/tone-map m_hdrImage to the current swapchain image
}

void RTVolumeRenderer::onSwapchainResized(const VkExtent2D& newExtent) {
    std::cout << "[RTV] Swapchain resized to " << newExtent.width << "x" << newExtent.height << std::endl;

    if (newExtent.width != m_extent.width || newExtent.height != m_extent.height) {
        m_extent = newExtent;

        // Cleanup old HDR target
        if (m_hdrImageView != VK_NULL_HANDLE) {
            vkDestroyImageView(m_context->getDevice(), m_hdrImageView, nullptr);
            m_hdrImageView = VK_NULL_HANDLE;
        }
        if (m_hdrImage != VK_NULL_HANDLE) {
            vkDestroyImage(m_context->getDevice(), m_hdrImage, nullptr);
            m_hdrImage = VK_NULL_HANDLE;
        }
        if (m_hdrMemory != VK_NULL_HANDLE) {
            vkFreeMemory(m_context->getDevice(), m_hdrMemory, nullptr);
            m_hdrMemory = VK_NULL_HANDLE;
        }

        // Recreate HDR target with new dimensions
        createHDRTarget();

        std::cout << "[RTV] HDR target recreated for new extent" << std::endl;
    }
}

void RTVolumeRenderer::createDensityGrid() {
    VkDevice device = m_context->getDevice();

    // Create 3D density image (R16_SFLOAT for good precision and performance)
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_3D;
    imageInfo.extent.width = m_params.gridDimensions.x;
    imageInfo.extent.height = m_params.gridDimensions.y;
    imageInfo.extent.depth = m_params.gridDimensions.z;
    imageInfo.mipLevels = m_densityMipLevels;
    imageInfo.arrayLayers = 1;
    imageInfo.format = VK_FORMAT_R16_SFLOAT;  // Single channel 16-bit float for density
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateImage(device, &imageInfo, nullptr, &m_densityImage) != VK_SUCCESS) {
        throw std::runtime_error("[RTV] Failed to create 3D density image!");
    }

    // Allocate memory for density image
    VkMemoryRequirements memRequirements;
    vkGetImageMemoryRequirements(device, m_densityImage, &memRequirements);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = findMemoryType(memRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    if (vkAllocateMemory(device, &allocInfo, nullptr, &m_densityMemory) != VK_SUCCESS) {
        throw std::runtime_error("[RTV] Failed to allocate 3D density image memory!");
    }

    vkBindImageMemory(device, m_densityImage, m_densityMemory, 0);

    // Create image view for the density grid
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = m_densityImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_3D;
    viewInfo.format = VK_FORMAT_R16_SFLOAT;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = m_densityMipLevels;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;

    if (vkCreateImageView(device, &viewInfo, nullptr, &m_densityImageView) != VK_SUCCESS) {
        throw std::runtime_error("[RTV] Failed to create 3D density image view!");
    }

    // Create sampler for density grid
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    samplerInfo.anisotropyEnable = VK_FALSE;
    samplerInfo.maxAnisotropy = 1.0f;
    samplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;  // Zero density outside grid
    samplerInfo.unnormalizedCoordinates = VK_FALSE;
    samplerInfo.compareEnable = VK_FALSE;
    samplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.mipLodBias = 0.0f;
    samplerInfo.minLod = 0.0f;
    samplerInfo.maxLod = static_cast<float>(m_densityMipLevels);

    if (vkCreateSampler(device, &samplerInfo, nullptr, &m_densitySampler) != VK_SUCCESS) {
        throw std::runtime_error("[RTV] Failed to create 3D density sampler!");
    }

    // Initial transition to GENERAL layout for compute shader writes
    VkCommandBuffer cmd = m_context->beginSingleTimeCommands();

    VkImageMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
    barrier.srcAccessMask = 0;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;  // For compute shader storage access
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = m_densityImage;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = m_densityMipLevels;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;

    VkDependencyInfo dependencyInfo{};
    dependencyInfo.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependencyInfo.imageMemoryBarrierCount = 1;
    dependencyInfo.pImageMemoryBarriers = &barrier;

    vkCmdPipelineBarrier2(cmd, &dependencyInfo);

    m_context->endSingleTimeCommands(cmd);

    std::cout << "[RTV] 3D density grid created: " << m_params.gridDimensions.x << "x"
              << m_params.gridDimensions.y << "x" << m_params.gridDimensions.z
              << " (R16_SFLOAT, " << m_densityMipLevels << " mip levels)" << std::endl;
}

void RTVolumeRenderer::createDensitySplatPipeline() {
    VkDevice device = m_context->getDevice();

    // Load density splat compute shader
    createShaderModule("shaders/density_splat_rt.comp.spv", m_densitySplatShader);

    // Create descriptor set layout for density splatting
    VkDescriptorSetLayoutBinding bindings[2] = {};

    // Binding 0: Particle buffer (readonly)
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    // Binding 1: 3D density grid (storage image)
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 2;
    layoutInfo.pBindings = bindings;

    if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &m_densitySplatDescriptorLayout) != VK_SUCCESS) {
        throw std::runtime_error("[RTV] Failed to create density splat descriptor set layout!");
    }

    // Create pipeline layout with push constants
    VkPushConstantRange pushConstantRange{};
    pushConstantRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushConstantRange.offset = 0;
    pushConstantRange.size = sizeof(DensityPushConstants);

    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &m_densitySplatDescriptorLayout;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushConstantRange;

    if (vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &m_densitySplatPipelineLayout) != VK_SUCCESS) {
        throw std::runtime_error("[RTV] Failed to create density splat pipeline layout!");
    }

    // Create compute pipeline
    VkComputePipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipelineInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipelineInfo.stage.module = m_densitySplatShader;
    pipelineInfo.stage.pName = "main";
    pipelineInfo.layout = m_densitySplatPipelineLayout;

    if (vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_densitySplatPipeline) != VK_SUCCESS) {
        throw std::runtime_error("[RTV] Failed to create density splat compute pipeline!");
    }

    std::cout << "[RTV] Density splat compute pipeline created successfully" << std::endl;
}

void RTVolumeRenderer::createHDRTarget() {
    VkDevice device = m_context->getDevice();

    // Create HDR image (R16G16B16A16_SFLOAT)
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent.width = m_extent.width;
    imageInfo.extent.height = m_extent.height;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateImage(device, &imageInfo, nullptr, &m_hdrImage) != VK_SUCCESS) {
        throw std::runtime_error("[RTV] Failed to create HDR image!");
    }

    // Allocate memory
    VkMemoryRequirements memRequirements;
    vkGetImageMemoryRequirements(device, m_hdrImage, &memRequirements);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = findMemoryType(memRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    if (vkAllocateMemory(device, &allocInfo, nullptr, &m_hdrMemory) != VK_SUCCESS) {
        throw std::runtime_error("[RTV] Failed to allocate HDR image memory!");
    }

    vkBindImageMemory(device, m_hdrImage, m_hdrMemory, 0);

    // Create image view
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = m_hdrImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;

    if (vkCreateImageView(device, &viewInfo, nullptr, &m_hdrImageView) != VK_SUCCESS) {
        throw std::runtime_error("[RTV] Failed to create HDR image view!");
    }

    // Create sampler
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.anisotropyEnable = VK_FALSE;
    samplerInfo.maxAnisotropy = 1.0f;
    samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    samplerInfo.unnormalizedCoordinates = VK_FALSE;
    samplerInfo.compareEnable = VK_FALSE;
    samplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;

    if (vkCreateSampler(device, &samplerInfo, nullptr, &m_hdrSampler) != VK_SUCCESS) {
        throw std::runtime_error("[RTV] Failed to create HDR sampler!");
    }

    std::cout << "[RTV] HDR target created: " << m_extent.width << "x" << m_extent.height
              << " (R16G16B16A16_SFLOAT)" << std::endl;
}

void RTVolumeRenderer::createCompositePass() {
    // RTV-2001: Placeholder for composite pipeline creation
    // This will be implemented properly when we add the fullscreen shaders
    std::cout << "[RTV] Composite pass placeholder created" << std::endl;
}

void RTVolumeRenderer::createShaderModule(const std::string& filename, VkShaderModule& shaderModule) {
    std::ifstream file(filename, std::ios::ate | std::ios::binary);

    if (!file.is_open()) {
        throw std::runtime_error("[RTV] Failed to open shader file: " + filename);
    }

    size_t fileSize = (size_t) file.tellg();
    std::vector<char> buffer(fileSize);

    file.seekg(0);
    file.read(buffer.data(), fileSize);
    file.close();

    VkShaderModuleCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    createInfo.codeSize = buffer.size();
    createInfo.pCode = reinterpret_cast<const uint32_t*>(buffer.data());

    if (vkCreateShaderModule(m_context->getDevice(), &createInfo, nullptr, &shaderModule) != VK_SUCCESS) {
        throw std::runtime_error("[RTV] Failed to create shader module: " + filename);
    }
}

void RTVolumeRenderer::cleanup() {
    VkDevice device = m_context->getDevice();

    // Cleanup density splat pipeline
    if (m_densitySplatShader != VK_NULL_HANDLE) {
        vkDestroyShaderModule(device, m_densitySplatShader, nullptr);
    }
    if (m_densitySplatDescriptorLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device, m_densitySplatDescriptorLayout, nullptr);
    }
    if (m_densitySplatPipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device, m_densitySplatPipelineLayout, nullptr);
    }
    if (m_densitySplatPipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(device, m_densitySplatPipeline, nullptr);
    }

    // Cleanup density grid
    if (m_densitySampler != VK_NULL_HANDLE) {
        vkDestroySampler(device, m_densitySampler, nullptr);
    }
    if (m_densityImageView != VK_NULL_HANDLE) {
        vkDestroyImageView(device, m_densityImageView, nullptr);
    }
    if (m_densityImage != VK_NULL_HANDLE) {
        vkDestroyImage(device, m_densityImage, nullptr);
    }
    if (m_densityMemory != VK_NULL_HANDLE) {
        vkFreeMemory(device, m_densityMemory, nullptr);
    }

    // Cleanup composite pipeline
    if (m_compositeFragShader != VK_NULL_HANDLE) {
        vkDestroyShaderModule(device, m_compositeFragShader, nullptr);
    }
    if (m_compositeVertShader != VK_NULL_HANDLE) {
        vkDestroyShaderModule(device, m_compositeVertShader, nullptr);
    }
    if (m_compositeDescriptorLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device, m_compositeDescriptorLayout, nullptr);
    }
    if (m_compositePipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device, m_compositePipelineLayout, nullptr);
    }
    if (m_compositePipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(device, m_compositePipeline, nullptr);
    }

    // Cleanup HDR target
    if (m_hdrSampler != VK_NULL_HANDLE) {
        vkDestroySampler(device, m_hdrSampler, nullptr);
    }
    if (m_hdrImageView != VK_NULL_HANDLE) {
        vkDestroyImageView(device, m_hdrImageView, nullptr);
    }
    if (m_hdrImage != VK_NULL_HANDLE) {
        vkDestroyImage(device, m_hdrImage, nullptr);
    }
    if (m_hdrMemory != VK_NULL_HANDLE) {
        vkFreeMemory(device, m_hdrMemory, nullptr);
    }
}

uint32_t RTVolumeRenderer::findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties memProperties;
    vkGetPhysicalDeviceMemoryProperties(m_context->getPhysicalDevice(), &memProperties);

    for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
        if ((typeFilter & (1 << i)) &&
            (memProperties.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }

    throw std::runtime_error("[RTV] Failed to find suitable memory type!");
}