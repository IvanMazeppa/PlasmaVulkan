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
}

RTVolumeRenderer::~RTVolumeRenderer() {
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