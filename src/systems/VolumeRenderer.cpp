#include "VolumeRenderer.h"
#include "../renderer/VulkanContext.h"
#include <iostream>
#include <fstream>
#include <array>
#include <cstring>
#include <algorithm>
#include <format>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

namespace plasma {

VolumeRenderer::VolumeRenderer(VulkanContext* context, const VolumeParams& params)
    : m_context(context), m_params(params) {
    
    std::cout << "Creating volume renderer..." << std::endl;
    std::cout << "Grid: " << m_params.gridDimensions.x << "x" << m_params.gridDimensions.y << "x" << m_params.gridDimensions.z << std::endl;
    std::cout << "Density splat mode: " << (m_useAtomicScatter ? "per-particle atomic scatter" : "voxel gather (fallback)") << std::endl;
    
    m_useAtomicScatter = m_context->supportsShaderAtomicFloat();

    createDensityGrid();
    createSTBNTexture();
    createOpticalDepthLUT();
    createTAAResources();
    createDensitySplatPipeline();
    createVolumeRenderPipeline();
    createTAAPipeline();
    createDescriptorSets();

    // Initialize ray tracing for hardware RT shadows
    createAccelerationStructures();

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
    
    // Calculate mip levels for 3D texture (cone-stepped raymarch optimization)
    uint32_t maxDim = std::max({width, height, depth});
    m_densityMipLevels = static_cast<uint32_t>(std::floor(std::log2(maxDim))) + 1;
    
    std::cout << "Creating density grid with " << m_densityMipLevels << " mip levels for LOD sampling" << std::endl;
    
    // Create 3D image for density storage with full mip chain
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_3D;
    imageInfo.extent.width = width;
    imageInfo.extent.height = height;
    imageInfo.extent.depth = depth;
    imageInfo.mipLevels = m_densityMipLevels;  // Full mip chain for cone stepping
    imageInfo.arrayLayers = 1;
    imageInfo.format = VK_FORMAT_R32_SFLOAT;  // Full precision needed for atomic operations
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
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
    
    // Create image view for storage access (full mip chain)
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = m_densityImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_3D;
    viewInfo.format = VK_FORMAT_R32_SFLOAT;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = m_densityMipLevels;  // Access all mip levels
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;
    
    if (vkCreateImageView(m_context->getDevice(), &viewInfo, nullptr, &m_densityImageView) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create density image view!");
    }

    // Create separate storage view for compute shader (mip 0 only, layout GENERAL)
    VkImageViewCreateInfo storageViewInfo = viewInfo;
    storageViewInfo.subresourceRange.levelCount = 1;  // Only mip 0 for compute storage

    if (vkCreateImageView(m_context->getDevice(), &storageViewInfo, nullptr, &m_densityStorageView) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create density storage image view!");
    }

    // Create sampler for volume rendering with mip mapping support
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;  // Linear mip interpolation
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.mipLodBias = 0.0f;
    samplerInfo.anisotropyEnable = VK_FALSE;
    samplerInfo.maxAnisotropy = 1.0f;
    samplerInfo.compareEnable = VK_FALSE;
    samplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;
    samplerInfo.minLod = 0.0f;
    samplerInfo.maxLod = static_cast<float>(m_densityMipLevels - 1);  // Allow access to all mips
    samplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    samplerInfo.unnormalizedCoordinates = VK_FALSE;
    
    if (vkCreateSampler(m_context->getDevice(), &samplerInfo, nullptr, &m_densitySampler) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create density sampler!");
    }
    
    // One-time initialization: clear and transition ALL mip levels to prevent first-frame crashes
    // This ensures safe sampling even when updateDensityGrid() hasn't run yet
    VkCommandBuffer cmd = m_context->beginSingleTimeCommands();
    
    // Step 1: Transition ALL mip levels from UNDEFINED to TRANSFER_DST for clearing
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = m_densityImage;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = m_densityMipLevels; // All mip levels
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);
    
    // Step 2: Clear ALL mip levels to zero (safe initial state)
    VkClearColorValue clearValue{}; // All zeros
    VkImageSubresourceRange clearRange{};
    clearRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    clearRange.baseMipLevel = 0;
    clearRange.levelCount = m_densityMipLevels; // Clear ALL mip levels
    clearRange.baseArrayLayer = 0;
    clearRange.layerCount = 1;
    
    vkCmdClearColorImage(cmd, m_densityImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clearValue, 1, &clearRange);
    
    // Step 3: Transition ALL mip levels to SHADER_READ_ONLY_OPTIMAL for sampling
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);
    
    m_context->endSingleTimeCommands(cmd);
    
    std::cout << "Density image initialized: all " << m_densityMipLevels << " mip levels cleared and transitioned to SHADER_READ_ONLY_OPTIMAL" << std::endl;
}

void VolumeRenderer::createSTBNTexture() {
    std::cout << "Loading STBN (Spatiotemporal Blue Noise) textures..." << std::endl;
    
    // Create 2D array image for 64 STBN layers
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent.width = STBN_SIZE;
    imageInfo.extent.height = STBN_SIZE;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = STBN_LAYERS;
    imageInfo.format = VK_FORMAT_R8_UNORM;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    
    if (vkCreateImage(m_context->getDevice(), &imageInfo, nullptr, &m_stbnImage) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create STBN image!");
    }
    
    // Allocate memory
    VkMemoryRequirements memRequirements;
    vkGetImageMemoryRequirements(m_context->getDevice(), m_stbnImage, &memRequirements);
    
    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = findMemoryType(memRequirements.memoryTypeBits, 
                                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    
    if (vkAllocateMemory(m_context->getDevice(), &allocInfo, nullptr, &m_stbnMemory) != VK_SUCCESS) {
        throw std::runtime_error("Failed to allocate STBN image memory!");
    }
    
    vkBindImageMemory(m_context->getDevice(), m_stbnImage, m_stbnMemory, 0);
    
    // Load and upload all 64 STBN texture layers
    VkCommandBuffer cmd = m_context->beginSingleTimeCommands();
    
    // Transition image to transfer destination
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = m_stbnImage;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = STBN_LAYERS;
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                        0, 0, nullptr, 0, nullptr, 1, &barrier);
    
    // Create single large staging buffer for all layers
    VkDeviceSize totalImageSize = STBN_SIZE * STBN_SIZE * STBN_LAYERS;
    VkBuffer stagingBuffer;
    VkDeviceMemory stagingBufferMemory;
    
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = totalImageSize;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    
    vkCreateBuffer(m_context->getDevice(), &bufferInfo, nullptr, &stagingBuffer);
    
    VkMemoryRequirements bufMemRequirements;
    vkGetBufferMemoryRequirements(m_context->getDevice(), stagingBuffer, &bufMemRequirements);
    
    VkMemoryAllocateInfo bufAllocInfo{};
    bufAllocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    bufAllocInfo.allocationSize = bufMemRequirements.size;
    bufAllocInfo.memoryTypeIndex = findMemoryType(bufMemRequirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    
    vkAllocateMemory(m_context->getDevice(), &bufAllocInfo, nullptr, &stagingBufferMemory);
    vkBindBufferMemory(m_context->getDevice(), stagingBuffer, stagingBufferMemory, 0);
    
    // Map staging buffer once
    void* data;
    vkMapMemory(m_context->getDevice(), stagingBufferMemory, 0, totalImageSize, 0, &data);
    
    // Load each STBN layer
    VkDeviceSize layerSize = STBN_SIZE * STBN_SIZE;
    for (uint32_t layer = 0; layer < STBN_LAYERS; ++layer) {
        std::string filename = std::format("assets/STBN/stbn_scalar_2Dx1Dx1D_128x128x64x1_{}.png", layer);
        
        int width, height, channels;
        unsigned char* pixels = stbi_load(filename.c_str(), &width, &height, &channels, 1);
        
        if (!pixels) {
            vkUnmapMemory(m_context->getDevice(), stagingBufferMemory);
            vkDestroyBuffer(m_context->getDevice(), stagingBuffer, nullptr);
            vkFreeMemory(m_context->getDevice(), stagingBufferMemory, nullptr);
            throw std::runtime_error("Failed to load STBN texture: " + filename);
        }
        
        if (width != STBN_SIZE || height != STBN_SIZE) {
            stbi_image_free(pixels);
            vkUnmapMemory(m_context->getDevice(), stagingBufferMemory);
            vkDestroyBuffer(m_context->getDevice(), stagingBuffer, nullptr);
            vkFreeMemory(m_context->getDevice(), stagingBufferMemory, nullptr);
            throw std::runtime_error("Unexpected STBN texture size: " + filename);
        }
        
        // Copy pixels to staging buffer at correct offset
        memcpy(static_cast<char*>(data) + layer * layerSize, pixels, layerSize);
        stbi_image_free(pixels);
        
        // Copy buffer to image layer
        VkBufferImageCopy region{};
        region.bufferOffset = layer * layerSize;
        region.bufferRowLength = 0;
        region.bufferImageHeight = 0;
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.mipLevel = 0;
        region.imageSubresource.baseArrayLayer = layer;
        region.imageSubresource.layerCount = 1;
        region.imageOffset = {0, 0, 0};
        region.imageExtent = {STBN_SIZE, STBN_SIZE, 1};
        
        vkCmdCopyBufferToImage(cmd, stagingBuffer, m_stbnImage, 
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    }
    
    vkUnmapMemory(m_context->getDevice(), stagingBufferMemory);
    
    // Transition image to shader read
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                        0, 0, nullptr, 0, nullptr, 1, &barrier);
    
    m_context->endSingleTimeCommands(cmd);
    
    // Clean up staging buffer after command buffer execution
    vkDestroyBuffer(m_context->getDevice(), stagingBuffer, nullptr);
    vkFreeMemory(m_context->getDevice(), stagingBufferMemory, nullptr);
    
    // Create image view
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = m_stbnImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    viewInfo.format = VK_FORMAT_R8_UNORM;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = STBN_LAYERS;
    
    if (vkCreateImageView(m_context->getDevice(), &viewInfo, nullptr, &m_stbnImageView) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create STBN image view!");
    }
    
    // Create sampler (nearest, repeat)
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_NEAREST;
    samplerInfo.minFilter = VK_FILTER_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.anisotropyEnable = VK_FALSE;
    samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    samplerInfo.unnormalizedCoordinates = VK_FALSE;
    samplerInfo.compareEnable = VK_FALSE;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.minLod = 0.0f;
    samplerInfo.maxLod = 0.0f;
    
    if (vkCreateSampler(m_context->getDevice(), &samplerInfo, nullptr, &m_stbnSampler) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create STBN sampler!");
    }
    
    std::cout << "STBN textures loaded successfully (" << STBN_LAYERS << " layers)" << std::endl;
}

void VolumeRenderer::createOpticalDepthLUT() {
    std::cout << "Creating preintegrated optical depth LUT..." << std::endl;
    
    // Generate preintegrated optical depth LUT data
    // f(tau) = (1 - exp(-tau)) / tau for tau > 0, f(0) = 1
    std::vector<float> lutData(OPTICAL_DEPTH_LUT_SIZE);
    
    for (uint32_t i = 0; i < OPTICAL_DEPTH_LUT_SIZE; ++i) {
        float tau = (float)i / (OPTICAL_DEPTH_LUT_SIZE - 1) * 8.0f; // Map [0, 8] optical depth range
        
        if (tau < 1e-6f) {
            // Handle tau ≈ 0: lim(tau->0) (1-e^(-tau))/tau = 1
            lutData[i] = 1.0f;
        } else {
            // Preintegrated segment: f(tau) = (1 - exp(-tau)) / tau
            lutData[i] = (1.0f - expf(-tau)) / tau;
        }
    }
    
    // Create 1D image for LUT
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_1D;
    imageInfo.extent.width = OPTICAL_DEPTH_LUT_SIZE;
    imageInfo.extent.height = 1;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = VK_FORMAT_R32_SFLOAT; // Single-channel float
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    
    if (vkCreateImage(m_context->getDevice(), &imageInfo, nullptr, &m_opticalDepthLUT) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create optical depth LUT image!");
    }
    
    // Allocate memory
    VkMemoryRequirements memRequirements;
    vkGetImageMemoryRequirements(m_context->getDevice(), m_opticalDepthLUT, &memRequirements);
    
    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = findMemoryType(memRequirements.memoryTypeBits, 
                                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    
    if (vkAllocateMemory(m_context->getDevice(), &allocInfo, nullptr, &m_opticalDepthLUTMemory) != VK_SUCCESS) {
        throw std::runtime_error("Failed to allocate optical depth LUT memory!");
    }
    
    vkBindImageMemory(m_context->getDevice(), m_opticalDepthLUT, m_opticalDepthLUTMemory, 0);
    
    // Upload LUT data
    VkCommandBuffer cmd = m_context->beginSingleTimeCommands();
    
    // Transition to transfer destination
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = m_opticalDepthLUT;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                        0, 0, nullptr, 0, nullptr, 1, &barrier);
    
    // Create staging buffer
    VkDeviceSize bufferSize = sizeof(float) * OPTICAL_DEPTH_LUT_SIZE;
    VkBuffer stagingBuffer;
    VkDeviceMemory stagingBufferMemory;
    
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = bufferSize;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    
    vkCreateBuffer(m_context->getDevice(), &bufferInfo, nullptr, &stagingBuffer);
    
    VkMemoryRequirements bufMemRequirements;
    vkGetBufferMemoryRequirements(m_context->getDevice(), stagingBuffer, &bufMemRequirements);
    
    VkMemoryAllocateInfo bufAllocInfo{};
    bufAllocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    bufAllocInfo.allocationSize = bufMemRequirements.size;
    bufAllocInfo.memoryTypeIndex = findMemoryType(bufMemRequirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    
    vkAllocateMemory(m_context->getDevice(), &bufAllocInfo, nullptr, &stagingBufferMemory);
    vkBindBufferMemory(m_context->getDevice(), stagingBuffer, stagingBufferMemory, 0);
    
    // Copy LUT data to staging buffer
    void* data;
    vkMapMemory(m_context->getDevice(), stagingBufferMemory, 0, bufferSize, 0, &data);
    memcpy(data, lutData.data(), bufferSize);
    vkUnmapMemory(m_context->getDevice(), stagingBufferMemory);
    
    // Copy from staging buffer to image
    VkBufferImageCopy region{};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {OPTICAL_DEPTH_LUT_SIZE, 1, 1};
    
    vkCmdCopyBufferToImage(cmd, stagingBuffer, m_opticalDepthLUT, 
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    
    // Transition to shader read
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                        0, 0, nullptr, 0, nullptr, 1, &barrier);
    
    m_context->endSingleTimeCommands(cmd);
    
    // Clean up staging buffer
    vkDestroyBuffer(m_context->getDevice(), stagingBuffer, nullptr);
    vkFreeMemory(m_context->getDevice(), stagingBufferMemory, nullptr);
    
    // Create image view
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = m_opticalDepthLUT;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_1D;
    viewInfo.format = VK_FORMAT_R32_SFLOAT;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;
    
    if (vkCreateImageView(m_context->getDevice(), &viewInfo, nullptr, &m_opticalDepthLUTView) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create optical depth LUT image view!");
    }
    
    // Create sampler (linear interpolation for smooth LUT access)
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE; // Clamp for LUT
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.anisotropyEnable = VK_FALSE;
    samplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    samplerInfo.unnormalizedCoordinates = VK_FALSE;
    samplerInfo.compareEnable = VK_FALSE;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.minLod = 0.0f;
    samplerInfo.maxLod = 0.0f;
    
    if (vkCreateSampler(m_context->getDevice(), &samplerInfo, nullptr, &m_opticalDepthLUTSampler) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create optical depth LUT sampler!");
    }
    
    std::cout << "Optical depth LUT created successfully (" << OPTICAL_DEPTH_LUT_SIZE << " entries)" << std::endl;
}

void VolumeRenderer::createTAAResources() {
    std::cout << "Creating TAA (Temporal Anti-Aliasing) resources..." << std::endl;
    
    // Get swap chain extent for TAA history buffer size
    VkExtent2D extent = m_context->getSwapChainExtent();
    m_taaExtent = extent;  // Store for consistent usage
    
    // Create TAA history image (RGB16F for high precision temporal accumulation)
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent.width = extent.width;
    imageInfo.extent.height = extent.height;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = VK_FORMAT_R16G16B16A16_SFLOAT; // High precision for accumulation
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    
    if (vkCreateImage(m_context->getDevice(), &imageInfo, nullptr, &m_taaHistoryImage) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create TAA history image!");
    }
    
    // Allocate memory for TAA history image
    VkMemoryRequirements memRequirements;
    vkGetImageMemoryRequirements(m_context->getDevice(), m_taaHistoryImage, &memRequirements);
    
    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = findMemoryType(memRequirements.memoryTypeBits, 
                                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    
    if (vkAllocateMemory(m_context->getDevice(), &allocInfo, nullptr, &m_taaHistoryMemory) != VK_SUCCESS) {
        throw std::runtime_error("Failed to allocate TAA history memory!");
    }
    
    vkBindImageMemory(m_context->getDevice(), m_taaHistoryImage, m_taaHistoryMemory, 0);
    
    // Initialize TAA history image to black
    VkCommandBuffer cmd = m_context->beginSingleTimeCommands();
    
    // Transition to clear destination
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = m_taaHistoryImage;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                        0, 0, nullptr, 0, nullptr, 1, &barrier);
    
    // Clear to black (transparent)
    VkClearColorValue clearColor = {{0.0f, 0.0f, 0.0f, 0.0f}};
    VkImageSubresourceRange clearRange{};
    clearRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    clearRange.baseMipLevel = 0;
    clearRange.levelCount = 1;
    clearRange.baseArrayLayer = 0;
    clearRange.layerCount = 1;
    
    vkCmdClearColorImage(cmd, m_taaHistoryImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clearColor, 1, &clearRange);
    
    // Transition to shader read optimal
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                        0, 0, nullptr, 0, nullptr, 1, &barrier);
    
    m_context->endSingleTimeCommands(cmd);
    
    // Create image view for TAA history
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = m_taaHistoryImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;
    
    if (vkCreateImageView(m_context->getDevice(), &viewInfo, nullptr, &m_taaHistoryImageView) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create TAA history image view!");
    }
    
    // Create sampler for TAA history (linear filtering for smooth temporal blending)
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.anisotropyEnable = VK_FALSE;
    samplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    samplerInfo.unnormalizedCoordinates = VK_FALSE;
    samplerInfo.compareEnable = VK_FALSE;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.minLod = 0.0f;
    samplerInfo.maxLod = 0.0f;
    
    if (vkCreateSampler(m_context->getDevice(), &samplerInfo, nullptr, &m_taaHistorySampler) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create TAA history sampler!");
    }
    
    // Create TAA current frame target (matches swapchain format for direct rendering)
    VkImageCreateInfo currentImageInfo{};
    currentImageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    currentImageInfo.imageType = VK_IMAGE_TYPE_2D;
    currentImageInfo.extent.width = extent.width;
    currentImageInfo.extent.height = extent.height;
    currentImageInfo.extent.depth = 1;
    currentImageInfo.mipLevels = 1;
    currentImageInfo.arrayLayers = 1;
    currentImageInfo.format = VK_FORMAT_R16G16B16A16_SFLOAT; // Keep entire TAA chain in linear HDR
    currentImageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    currentImageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    currentImageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    currentImageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    currentImageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    
    if (vkCreateImage(m_context->getDevice(), &currentImageInfo, nullptr, &m_taaCurrentImage) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create TAA current frame image!");
    }
    
    // Allocate memory for TAA current frame image
    VkMemoryRequirements currentMemRequirements;
    vkGetImageMemoryRequirements(m_context->getDevice(), m_taaCurrentImage, &currentMemRequirements);
    
    VkMemoryAllocateInfo currentAllocInfo{};
    currentAllocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    currentAllocInfo.allocationSize = currentMemRequirements.size;
    currentAllocInfo.memoryTypeIndex = findMemoryType(currentMemRequirements.memoryTypeBits, 
                                                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    
    if (vkAllocateMemory(m_context->getDevice(), &currentAllocInfo, nullptr, &m_taaCurrentMemory) != VK_SUCCESS) {
        throw std::runtime_error("Failed to allocate TAA current frame memory!");
    }
    
    vkBindImageMemory(m_context->getDevice(), m_taaCurrentImage, m_taaCurrentMemory, 0);
    
    // Create image view for TAA current frame
    VkImageViewCreateInfo currentViewInfo{};
    currentViewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    currentViewInfo.image = m_taaCurrentImage;
    currentViewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    currentViewInfo.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    currentViewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    currentViewInfo.subresourceRange.baseMipLevel = 0;
    currentViewInfo.subresourceRange.levelCount = 1;
    currentViewInfo.subresourceRange.baseArrayLayer = 0;
    currentViewInfo.subresourceRange.layerCount = 1;
    
    if (vkCreateImageView(m_context->getDevice(), &currentViewInfo, nullptr, &m_taaCurrentImageView) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create TAA current frame image view!");
    }
    
    // Create sampler for TAA current frame
    VkSamplerCreateInfo currentSamplerInfo{};
    currentSamplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    currentSamplerInfo.magFilter = VK_FILTER_LINEAR;
    currentSamplerInfo.minFilter = VK_FILTER_LINEAR;
    currentSamplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    currentSamplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    currentSamplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    currentSamplerInfo.anisotropyEnable = VK_FALSE;
    currentSamplerInfo.maxAnisotropy = 1.0f;
    currentSamplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    currentSamplerInfo.unnormalizedCoordinates = VK_FALSE;
    currentSamplerInfo.compareEnable = VK_FALSE;
    currentSamplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    currentSamplerInfo.minLod = 0.0f;
    currentSamplerInfo.maxLod = 0.0f;
    
    if (vkCreateSampler(m_context->getDevice(), &currentSamplerInfo, nullptr, &m_taaCurrentSampler) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create TAA current frame sampler!");
    }
    
    // Transition TAA current image to color attachment optimal using a separate command buffer
    VkCommandBuffer currentCmd = m_context->beginSingleTimeCommands();
    
    VkImageMemoryBarrier currentBarrier{};
    currentBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    currentBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    currentBarrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    currentBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    currentBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    currentBarrier.image = m_taaCurrentImage;
    currentBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    currentBarrier.subresourceRange.baseMipLevel = 0;
    currentBarrier.subresourceRange.levelCount = 1;
    currentBarrier.subresourceRange.baseArrayLayer = 0;
    currentBarrier.subresourceRange.layerCount = 1;
    currentBarrier.srcAccessMask = 0;
    currentBarrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    
    vkCmdPipelineBarrier(currentCmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &currentBarrier);
    
    m_context->endSingleTimeCommands(currentCmd);
    
    std::cout << "TAA resources created successfully (" << extent.width << "x" << extent.height << ")" << std::endl;
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
    
    // Create descriptor set layout for volume rendering (density + STBN + optical depth LUT + TLAS for RT)
    // FORCE RT support since shader now unconditionally uses TLAS
    uint32_t bindingCount = 4;  // Always include TLAS binding since shader expects it
    std::cout << "[DEBUG] createVolumeRenderPipeline: FORCING bindingCount=" << bindingCount << " (hardcoded RT support)" << std::endl;
    std::vector<VkDescriptorSetLayoutBinding> volumeBindings(bindingCount);

    // Binding 0: Density texture
    volumeBindings[0].binding = 0;
    volumeBindings[0].descriptorCount = 1;
    volumeBindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    volumeBindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    // Binding 1: STBN texture array
    volumeBindings[1].binding = 1;
    volumeBindings[1].descriptorCount = 1;
    volumeBindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    volumeBindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    // Binding 2: Optical depth LUT
    volumeBindings[2].binding = 2;
    volumeBindings[2].descriptorCount = 1;
    volumeBindings[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    volumeBindings[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    // Binding 3: TLAS for ray tracing (Stage 1: shadow catcher disc) - ALWAYS CREATED
    volumeBindings[3].binding = 3;
    volumeBindings[3].descriptorCount = 1;
    volumeBindings[3].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    volumeBindings[3].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    std::cout << "[DEBUG] HARDCODED RT descriptor layout - added TLAS binding at 3, type=ACCELERATION_STRUCTURE_KHR" << std::endl;

    // Clean up any existing descriptor set layout first
    if (m_volumeDescriptorSetLayout != VK_NULL_HANDLE) {
        std::cout << "[DEBUG] Cleaning up existing volume descriptor set layout" << std::endl;
        vkDestroyDescriptorSetLayout(m_context->getDevice(), m_volumeDescriptorSetLayout, nullptr);
        m_volumeDescriptorSetLayout = VK_NULL_HANDLE;
    }

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT;
    layoutInfo.bindingCount = bindingCount;
    layoutInfo.pBindings = volumeBindings.data();

    std::cout << "[DEBUG] Creating descriptor layout with " << bindingCount << " bindings" << std::endl;
    if (vkCreateDescriptorSetLayout(m_context->getDevice(), &layoutInfo, nullptr,
        &m_volumeDescriptorSetLayout) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create volume descriptor set layout!");
    }
    std::cout << "[DEBUG] New volume descriptor set layout created: " << m_volumeDescriptorSetLayout << std::endl;
    
    // Push constants for volume rendering
    VkPushConstantRange pushConstantRange{};
    pushConstantRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    pushConstantRange.offset = 0;
    pushConstantRange.size = sizeof(VolumePushConstants);
    
    // Clean up any existing pipeline layout first
    if (m_volumePipelineLayout != VK_NULL_HANDLE) {
        std::cout << "[DEBUG] Cleaning up existing volume pipeline layout" << std::endl;
        vkDestroyPipelineLayout(m_context->getDevice(), m_volumePipelineLayout, nullptr);
        m_volumePipelineLayout = VK_NULL_HANDLE;
    }

    // Pipeline layout
    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &m_volumeDescriptorSetLayout;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushConstantRange;

    std::cout << "[DEBUG] Creating pipeline layout using descriptor layout: " << m_volumeDescriptorSetLayout << std::endl;
    if (vkCreatePipelineLayout(m_context->getDevice(), &pipelineLayoutInfo, nullptr,
        &m_volumePipelineLayout) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create volume pipeline layout!");
    }
    std::cout << "[DEBUG] New volume pipeline layout created: " << m_volumePipelineLayout << std::endl;
    
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
    
    // Create both SDR and HDR pipelines using helper function
    VkFormat swapchainFormat = m_context->getSwapChainImageFormat();
    VkFormat hdrFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

    // Create SDR pipeline for swapchain rendering
    std::cout << "[DEBUG] Creating SDR pipeline with format: " << swapchainFormat << std::endl;
    m_volumePipeline = createVolumePipelineForFormat(swapchainFormat, "volume-SDR");

    // Create HDR pipeline for TAA targets
    std::cout << "[DEBUG] Creating HDR pipeline with format: " << hdrFormat << std::endl;
    m_volumePipelineHDR = createVolumePipelineForFormat(hdrFormat, "volume-HDR");
}

// Helper function to create volume pipeline for specific color attachment format
VkPipeline VolumeRenderer::createVolumePipelineForFormat(VkFormat colorFormat, const char* tag) {
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

    // Viewport and scissor (dynamic)
    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    // Rasterizer
    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.depthClampEnable = VK_FALSE;
    rasterizer.rasterizerDiscardEnable = VK_FALSE;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = VK_CULL_MODE_NONE;  // No face culling for fullscreen triangle
    rasterizer.frontFace = VK_FRONT_FACE_CLOCKWISE;
    rasterizer.depthBiasEnable = VK_FALSE;

    // Multisampling
    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.sampleShadingEnable = VK_FALSE;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // Color blending
    VkPipelineColorBlendAttachmentState colorBlendAttachment{};
    colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    colorBlendAttachment.blendEnable = VK_TRUE;

    // Alpha blend for volumetric rendering: src_alpha + (1-src_alpha) * dst
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

    // Dynamic states
    std::vector<VkDynamicState> dynamicStates = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR
    };

    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
    dynamicState.pDynamicStates = dynamicStates.data();

    // Color attachment format for dynamic rendering (THIS IS THE KEY DIFFERENCE)
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

    VkPipeline pipeline;
    std::cout << "[DEBUG] Creating " << tag << " pipeline with color format=" << colorFormat << " and layout=" << m_volumePipelineLayout << std::endl;
    if (vkCreateGraphicsPipelines(m_context->getDevice(), VK_NULL_HANDLE, 1, &pipelineInfo, nullptr,
        &pipeline) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create volume graphics pipeline!");
    }

    std::cout << "[DEBUG] " << tag << " pipeline created successfully" << std::endl;
    return pipeline;
}

void VolumeRenderer::createTAAPipeline() {
    // Load TAA shaders
    auto vertShaderCode = readFile("shaders/taa.vert.spv");
    auto fragShaderCode = readFile("shaders/taa.frag.spv");
    
    VkShaderModuleCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    
    // TAA vertex shader
    VkShaderModule taaVertShader;
    createInfo.codeSize = vertShaderCode.size();
    createInfo.pCode = reinterpret_cast<const uint32_t*>(vertShaderCode.data());
    if (vkCreateShaderModule(m_context->getDevice(), &createInfo, nullptr, &taaVertShader) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create TAA vertex shader module!");
    }
    
    // TAA fragment shader
    VkShaderModule taaFragShader;
    createInfo.codeSize = fragShaderCode.size();
    createInfo.pCode = reinterpret_cast<const uint32_t*>(fragShaderCode.data());
    if (vkCreateShaderModule(m_context->getDevice(), &createInfo, nullptr, &taaFragShader) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create TAA fragment shader module!");
    }
    
    // Create TAA descriptor set layout (current frame + history frame)
    std::array<VkDescriptorSetLayoutBinding, 2> taaBindings{};
    
    // Binding 0: Current frame texture (volumetric render result)
    taaBindings[0].binding = 0;
    taaBindings[0].descriptorCount = 1;
    taaBindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    taaBindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    
    // Binding 1: History frame texture (previous TAA result)  
    taaBindings[1].binding = 1;
    taaBindings[1].descriptorCount = 1;
    taaBindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    taaBindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    
    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT;
    layoutInfo.bindingCount = static_cast<uint32_t>(taaBindings.size());
    layoutInfo.pBindings = taaBindings.data();
    
    if (vkCreateDescriptorSetLayout(m_context->getDevice(), &layoutInfo, nullptr, 
        &m_taaDescriptorSetLayout) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create TAA descriptor set layout!");
    }
    
    // TAA push constants for reprojection
    struct TAAConstants {
        glm::mat4 currentToHistory;  // Reprojection matrix
        glm::vec2 screenSize;        // Screen dimensions
        float blendFactor;           // Temporal blend factor 
        uint32_t firstFrame;         // First frame flag
    };
    
    VkPushConstantRange pushConstantRange{};
    pushConstantRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    pushConstantRange.offset = 0;
    pushConstantRange.size = sizeof(TAAConstants);
    
    // TAA pipeline layout
    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &m_taaDescriptorSetLayout;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushConstantRange;
    
    if (vkCreatePipelineLayout(m_context->getDevice(), &pipelineLayoutInfo, nullptr, 
        &m_taaPipelineLayout) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create TAA pipeline layout!");
    }
    
    // TAA shader stages
    VkPipelineShaderStageCreateInfo shaderStages[2] = {};
    
    shaderStages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shaderStages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    shaderStages[0].module = taaVertShader;
    shaderStages[0].pName = "main";
    
    shaderStages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shaderStages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    shaderStages[1].module = taaFragShader;  
    shaderStages[1].pName = "main";
    
    // TAA vertex input (no attributes - procedural fullscreen triangle)
    VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
    vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInputInfo.vertexBindingDescriptionCount = 0;
    vertexInputInfo.vertexAttributeDescriptionCount = 0;
    
    // Input assembly for fullscreen triangle
    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    inputAssembly.primitiveRestartEnable = VK_FALSE;
    
    // Dynamic viewport and scissor
    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;
    
    // Rasterization (fullscreen pass)
    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.depthClampEnable = VK_FALSE;
    rasterizer.rasterizerDiscardEnable = VK_FALSE;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = VK_CULL_MODE_NONE;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer.depthBiasEnable = VK_FALSE;
    
    // No multisampling for TAA
    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.sampleShadingEnable = VK_FALSE;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    
    // Alpha blending (replace mode - TAA outputs final result)
    VkPipelineColorBlendAttachmentState colorBlendAttachment{};
    colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | 
                                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    colorBlendAttachment.blendEnable = VK_FALSE;
    
    VkPipelineColorBlendStateCreateInfo colorBlending{};
    colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlending.logicOpEnable = VK_FALSE;
    colorBlending.attachmentCount = 1;
    colorBlending.pAttachments = &colorBlendAttachment;
    
    // Dynamic states
    std::vector<VkDynamicState> dynamicStates = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR
    };
    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
    dynamicState.pDynamicStates = dynamicStates.data();
    
    // TAA rendering info (matches swapchain format)
    VkPipelineRenderingCreateInfo renderingInfo{};
    renderingInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    renderingInfo.colorAttachmentCount = 1;
    VkFormat colorFormat = m_context->getSwapChainImageFormat();
    renderingInfo.pColorAttachmentFormats = &colorFormat;
    
    // Create TAA graphics pipeline
    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.pNext = &renderingInfo;
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = shaderStages;
    pipelineInfo.pVertexInputState = &vertexInputInfo;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisampling;
    pipelineInfo.pColorBlendState = &colorBlending;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = m_taaPipelineLayout;
    pipelineInfo.renderPass = VK_NULL_HANDLE; // Using dynamic rendering
    pipelineInfo.subpass = 0;
    
    if (vkCreateGraphicsPipelines(m_context->getDevice(), VK_NULL_HANDLE, 1, &pipelineInfo, 
        nullptr, &m_taaPipeline) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create TAA graphics pipeline!");
    }
    
    // Clean up shader modules
    vkDestroyShaderModule(m_context->getDevice(), taaVertShader, nullptr);
    vkDestroyShaderModule(m_context->getDevice(), taaFragShader, nullptr);
    
    std::cout << "TAA pipeline created successfully!" << std::endl;
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
    imageInfo.imageView = m_densityStorageView;  // Use storage view for mip 0 only
    
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
    
    // Mark density as initialized to prevent first-frame crashes
    m_densityInitialized = true;
}

void VolumeRenderer::render(VkCommandBuffer cmd, const glm::mat4& viewProj, const glm::vec3& cameraPos, QualityLevel quality) {
    // Calculate reprojection matrix for TAA (previous_viewProj * inverse(current_viewProj))
    glm::mat4 currentToHistory = glm::mat4(1.0f);
    if (!m_taaFirstFrame) {
        currentToHistory = m_previousViewProjMatrix * glm::inverse(viewProj);
    }
    
    // Update previous view-projection matrix for next frame
    m_previousViewProjMatrix = viewProj;
    
    // Set dynamic viewport and scissor
    VkExtent2D extent = m_taaExtent;
    
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
    
    // Push descriptors for density + STBN + optical depth LUT (Vulkan 1.4)
    VkDescriptorImageInfo imageInfos[3] = {};
    
    // Binding 0: Density texture
    imageInfos[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageInfos[0].imageView = m_densityImageView;
    imageInfos[0].sampler = m_densitySampler;
    
    // Binding 1: STBN texture array
    imageInfos[1].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageInfos[1].imageView = m_stbnImageView;
    imageInfos[1].sampler = m_stbnSampler;
    
    // Binding 2: Optical depth LUT
    imageInfos[2].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageInfos[2].imageView = m_opticalDepthLUTView;
    imageInfos[2].sampler = m_opticalDepthLUTSampler;
    
    // Prepare descriptor writes (3 or 4 depending on RT support)
    uint32_t descriptorCount = supportsRayTracing() ? 4 : 3;
    std::vector<VkWriteDescriptorSet> descriptorWrites(descriptorCount);
    VkWriteDescriptorSetAccelerationStructureKHR accelStructInfo{};

    // Density texture descriptor
    descriptorWrites[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptorWrites[0].pNext = nullptr;
    descriptorWrites[0].dstSet = VK_NULL_HANDLE; // Ignored for push descriptors
    descriptorWrites[0].dstBinding = 0;
    descriptorWrites[0].dstArrayElement = 0;
    descriptorWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    descriptorWrites[0].descriptorCount = 1;
    descriptorWrites[0].pImageInfo = &imageInfos[0];
    descriptorWrites[0].pBufferInfo = nullptr;
    descriptorWrites[0].pTexelBufferView = nullptr;

    // STBN texture descriptor
    descriptorWrites[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptorWrites[1].pNext = nullptr;
    descriptorWrites[1].dstSet = VK_NULL_HANDLE; // Ignored for push descriptors
    descriptorWrites[1].dstBinding = 1;
    descriptorWrites[1].dstArrayElement = 0;
    descriptorWrites[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    descriptorWrites[1].descriptorCount = 1;
    descriptorWrites[1].pImageInfo = &imageInfos[1];
    descriptorWrites[1].pBufferInfo = nullptr;
    descriptorWrites[1].pTexelBufferView = nullptr;

    // Optical depth LUT descriptor
    descriptorWrites[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptorWrites[2].pNext = nullptr;
    descriptorWrites[2].dstSet = VK_NULL_HANDLE; // Ignored for push descriptors
    descriptorWrites[2].dstBinding = 2;
    descriptorWrites[2].dstArrayElement = 0;
    descriptorWrites[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    descriptorWrites[2].descriptorCount = 1;
    descriptorWrites[2].pImageInfo = &imageInfos[2];
    descriptorWrites[2].pBufferInfo = nullptr;
    descriptorWrites[2].pTexelBufferView = nullptr;

    // TLAS descriptor (binding 3) - only when RT is supported
    if (supportsRayTracing()) {
        accelStructInfo.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
        accelStructInfo.accelerationStructureCount = 1;
        accelStructInfo.pAccelerationStructures = &m_topLevelAS;

        descriptorWrites[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        descriptorWrites[3].pNext = &accelStructInfo;
        descriptorWrites[3].dstSet = VK_NULL_HANDLE; // Ignored for push descriptors
        descriptorWrites[3].dstBinding = 3;
        descriptorWrites[3].dstArrayElement = 0;
        descriptorWrites[3].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
        descriptorWrites[3].descriptorCount = 1;
        descriptorWrites[3].pImageInfo = nullptr;
        descriptorWrites[3].pBufferInfo = nullptr;
        descriptorWrites[3].pTexelBufferView = nullptr;
    }

    vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_volumePipelineLayout,
        0, descriptorCount, descriptorWrites.data());
    
    // Push constants for ray marching with quality override
    VolumePushConstants pushConstants{};
    pushConstants.viewProjInv = glm::inverse(viewProj);
    pushConstants.cameraPos = cameraPos;
    pushConstants.gridOrigin = m_params.gridOrigin;
    pushConstants.voxelSize = m_params.voxelSize;
    pushConstants.gridDimensions = m_params.gridDimensions;
    
    // Use runtime parameters for all quality modes, with quality-based multipliers
    pushConstants.densityScale = m_runtimeDensityScale;
    pushConstants.opacityScale = m_runtimeOpacityScale;
    pushConstants.emissionScale = m_runtimeEmissionScale;
    pushConstants.tempOffset = m_runtimeTempOffset;
    pushConstants.tempRange = m_runtimeTempRange;
    pushConstants.saturation = m_runtimeSaturation;
    
    // STBN temporal parameters
    pushConstants.frameIndex = m_frameIndex % STBN_LAYERS;
    
    // Cranley-Patterson offset for temporal decorrelation 
    const float phi1 = 0.618034f;  // Golden ratio - 1
    const float phi2 = 0.755769f;  // Plastic number - 1
    pushConstants.cpOffset.x = fmod(m_frameIndex * phi1 + 0.123456f, 1.0f);
    pushConstants.cpOffset.y = fmod(m_frameIndex * phi2 + 0.789012f, 1.0f);
    
    // Increment frame counter for next frame
    m_frameIndex++;
    
    // Apply quality-based adjustments to step count and size
    switch (quality) {
        case QualityLevel::High:
            pushConstants.maxSteps = m_runtimeMaxSteps * 2;     // 2x ray steps for high quality
            pushConstants.stepSize = m_runtimeStepSize * 0.5f;  // Finer steps for high quality
            break;
        case QualityLevel::Ultra:
            pushConstants.maxSteps = m_runtimeMaxSteps * 4;     // 4x ray steps for ultra quality  
            pushConstants.stepSize = m_runtimeStepSize * 0.25f; // Ultra-fine steps for recording
            break;
        default: // QualityLevel::Standard
            pushConstants.maxSteps = m_runtimeMaxSteps;         // Standard runtime values
            pushConstants.stepSize = m_runtimeStepSize;
            break;
    }
    
    vkCmdPushConstants(cmd, m_volumePipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 
        0, sizeof(VolumePushConstants), &pushConstants);
    
    // Draw fullscreen triangle
    vkCmdDraw(cmd, 3, 1, 0, 0);
}

void VolumeRenderer::renderToTAATarget(VkCommandBuffer cmd, const glm::mat4& viewProj, const glm::vec3& cameraPos, QualityLevel quality) {
    // NOTE: Do NOT update m_previousViewProjMatrix here - renderTAA() needs the previous matrix
    // Matrix will be updated after TAA processing is complete
    
    VkExtent2D extent = m_taaExtent;
    
    // Begin rendering to TAA current frame target (HDR format)
    VkRenderingAttachmentInfo colorAttachment{};
    colorAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    colorAttachment.imageView = m_taaCurrentImageView;
    colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
    
    VkRenderingInfo renderingInfo{};
    renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    renderingInfo.renderArea.offset = {0, 0};
    renderingInfo.renderArea.extent = extent;
    renderingInfo.layerCount = 1;
    renderingInfo.colorAttachmentCount = 1;
    renderingInfo.pColorAttachments = &colorAttachment;
    
    vkCmdBeginRendering(cmd, &renderingInfo);
    
    // Set dynamic viewport and scissor
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
    
    // Bind HDR volume rendering pipeline for TAA target and push descriptors
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_volumePipelineHDR);
    
    // Push descriptors for density + STBN + optical depth LUT (Vulkan 1.4)
    VkDescriptorImageInfo imageInfos[3] = {};
    
    // Binding 0: Density texture
    imageInfos[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageInfos[0].imageView = m_densityImageView;
    imageInfos[0].sampler = m_densitySampler;
    
    // Binding 1: STBN texture array
    imageInfos[1].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageInfos[1].imageView = m_stbnImageView;
    imageInfos[1].sampler = m_stbnSampler;
    
    // Binding 2: Optical depth LUT
    imageInfos[2].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageInfos[2].imageView = m_opticalDepthLUTView;
    imageInfos[2].sampler = m_opticalDepthLUTSampler;
    
    // RT-aware descriptor count
    uint32_t descriptorCount = supportsRayTracing() ? 4 : 3;
    VkWriteDescriptorSet descriptorWrites[4] = {};  // Max size for RT support
    VkWriteDescriptorSetAccelerationStructureKHR accelDescriptor{};
    
    // Density texture descriptor
    descriptorWrites[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptorWrites[0].dstBinding = 0;
    descriptorWrites[0].dstArrayElement = 0;
    descriptorWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    descriptorWrites[0].descriptorCount = 1;
    descriptorWrites[0].pImageInfo = &imageInfos[0];
    
    // STBN texture descriptor  
    descriptorWrites[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptorWrites[1].dstBinding = 1;
    descriptorWrites[1].dstArrayElement = 0;
    descriptorWrites[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    descriptorWrites[1].descriptorCount = 1;
    descriptorWrites[1].pImageInfo = &imageInfos[1];
    
    // Optical depth LUT descriptor
    descriptorWrites[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptorWrites[2].dstBinding = 2;
    descriptorWrites[2].dstArrayElement = 0;
    descriptorWrites[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    descriptorWrites[2].descriptorCount = 1;
    descriptorWrites[2].pImageInfo = &imageInfos[2];
    
    // Add TLAS descriptor when RT is supported
    if (supportsRayTracing()) {
        accelDescriptor.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
        accelDescriptor.accelerationStructureCount = 1;
        accelDescriptor.pAccelerationStructures = &m_topLevelAS;

        descriptorWrites[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        descriptorWrites[3].pNext = &accelDescriptor;
        descriptorWrites[3].dstBinding = 3;
        descriptorWrites[3].dstArrayElement = 0;
        descriptorWrites[3].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
        descriptorWrites[3].descriptorCount = 1;
    }

    // Push descriptors using KHR extension
    vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_volumePipelineLayout,
                              0, descriptorCount, descriptorWrites);
    
    // Quality-based parameter selection
    VolumeParams params;
    switch (quality) {
        case QualityLevel::High:
            params = VolumeParams::getHighQuality();
            break;
        case QualityLevel::Ultra:
            params = VolumeParams::getUltraRecordingQuality();
            break;
        default:
            params = m_params;
            break;
    }
    
    // Prepare push constants for volume ray marching
    VolumePushConstants pushConstants{};
    pushConstants.viewProjInv = glm::inverse(viewProj);
    pushConstants.cameraPos = cameraPos;
    pushConstants.gridOrigin = params.gridOrigin;
    pushConstants.voxelSize = params.voxelSize;
    pushConstants.gridDimensions = params.gridDimensions;
    
    // Use runtime parameters for all quality modes
    pushConstants.densityScale = m_runtimeDensityScale;
    pushConstants.opacityScale = m_runtimeOpacityScale;
    pushConstants.emissionScale = m_runtimeEmissionScale;
    pushConstants.tempOffset = m_runtimeTempOffset;
    pushConstants.tempRange = m_runtimeTempRange;
    pushConstants.saturation = m_runtimeSaturation;
    
    // STBN temporal parameters
    pushConstants.frameIndex = m_frameIndex % STBN_LAYERS;
    
    // Apply quality-based adjustments to step count and size
    switch (quality) {
        case QualityLevel::High:
            pushConstants.maxSteps = m_runtimeMaxSteps * 2;     // 2x ray steps for high quality
            pushConstants.stepSize = m_runtimeStepSize * 0.5f;  // Finer steps for high quality
            break;
        case QualityLevel::Ultra:
            pushConstants.maxSteps = m_runtimeMaxSteps * 4;     // 4x ray steps for ultra quality  
            pushConstants.stepSize = m_runtimeStepSize * 0.25f; // Ultra-fine steps for recording
            break;
        default:
            pushConstants.maxSteps = m_runtimeMaxSteps;
            pushConstants.stepSize = m_runtimeStepSize;
            break;
    }
    
    // Cranley-Patterson offset for temporal decorrelation 
    const float phi1 = 0.618034f;  // Golden ratio - 1
    const float phi2 = 0.755769f;  // Plastic number - 1
    pushConstants.cpOffset = glm::vec2(
        fmod(m_frameIndex * phi1, 1.0f),
        fmod(m_frameIndex * phi2, 1.0f)
    );
    
    // Push constants to shader
    vkCmdPushConstants(cmd, m_volumePipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 
        0, sizeof(VolumePushConstants), &pushConstants);
    
    // Draw fullscreen triangle
    vkCmdDraw(cmd, 3, 1, 0, 0);
    
    vkCmdEndRendering(cmd);
    
    // Transition TAA current image from color attachment to shader read
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = m_taaCurrentImage;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);
}

void VolumeRenderer::renderTAA(VkCommandBuffer cmd, const glm::mat4& viewProj, VkImageView currentFrameView) {
    // TAA push constants for reprojection
    struct TAAConstants {
        glm::mat4 currentToHistory;  // Reprojection matrix
        glm::vec2 screenSize;        // Screen dimensions
        float blendFactor;           // Temporal blend factor 
        uint32_t firstFrame;         // First frame flag
    };
    
    // Calculate reprojection matrix for TAA
    glm::mat4 currentToHistory = glm::mat4(1.0f);
    if (!m_taaFirstFrame) {
        currentToHistory = m_previousViewProjMatrix * glm::inverse(viewProj);
    }
    
    VkExtent2D extent = m_taaExtent;
    
    // Prepare TAA push constants
    TAAConstants taaConstants{};
    taaConstants.currentToHistory = currentToHistory;
    taaConstants.screenSize = glm::vec2(extent.width, extent.height);
    taaConstants.blendFactor = m_taaBlendFactor;  // Runtime-adjustable blend factor (NUM.)
    taaConstants.firstFrame = m_taaFirstFrame ? 1u : 0u;
    
    // NOTE: Viewport and scissor are already set by Application - no need to set them again
    
    // Bind TAA pipeline
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_taaPipeline);
    
    // Push TAA constants
    vkCmdPushConstants(cmd, m_taaPipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 
                       0, sizeof(TAAConstants), &taaConstants);
    
    // Push descriptors for TAA (current frame + history frame)
    VkDescriptorImageInfo imageInfos[2] = {};
    
    // Binding 0: Current frame texture (volumetric render result)
    imageInfos[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageInfos[0].imageView = currentFrameView;
    imageInfos[0].sampler = m_taaHistorySampler;
    
    // Binding 1: History frame texture (previous TAA result)
    imageInfos[1].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageInfos[1].imageView = m_taaHistoryImageView;
    imageInfos[1].sampler = m_taaHistorySampler;
    
    VkWriteDescriptorSet descriptorWrites[2] = {};
    
    // Current frame descriptor
    descriptorWrites[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptorWrites[0].dstBinding = 0;
    descriptorWrites[0].dstArrayElement = 0;
    descriptorWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    descriptorWrites[0].descriptorCount = 1;
    descriptorWrites[0].pImageInfo = &imageInfos[0];
    
    // History frame descriptor
    descriptorWrites[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptorWrites[1].dstBinding = 1;
    descriptorWrites[1].dstArrayElement = 0;
    descriptorWrites[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    descriptorWrites[1].descriptorCount = 1;
    descriptorWrites[1].pImageInfo = &imageInfos[1];
    
    // Push descriptors (Vulkan 1.1+)
    vkCmdPushDescriptorSet(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_taaPipelineLayout,
                           0, 2, descriptorWrites);
    
    // Draw fullscreen triangle (TAA shader generates vertices procedurally)
    vkCmdDraw(cmd, 3, 1, 0, 0);
    
    // Mark that we're no longer on the first frame
    m_taaFirstFrame = false;
    
    // Increment frame index for STBN temporal variation
    m_frameIndex++;
}

void VolumeRenderer::updateTAAHistory(VkCommandBuffer cmd) {
    // Copy current frame to history buffer for next frame's TAA
    VkExtent2D extent = m_taaExtent;
    
    // Transition history image from shader read to transfer destination
    VkImageMemoryBarrier historyBarrier{};
    historyBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    historyBarrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    historyBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    historyBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    historyBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    historyBarrier.image = m_taaHistoryImage;
    historyBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    historyBarrier.subresourceRange.baseMipLevel = 0;
    historyBarrier.subresourceRange.levelCount = 1;
    historyBarrier.subresourceRange.baseArrayLayer = 0;
    historyBarrier.subresourceRange.layerCount = 1;
    historyBarrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    historyBarrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    
    // Transition current image from shader read to transfer source
    VkImageMemoryBarrier currentBarrier{};
    currentBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    currentBarrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    currentBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    currentBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    currentBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    currentBarrier.image = m_taaCurrentImage;
    currentBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    currentBarrier.subresourceRange.baseMipLevel = 0;
    currentBarrier.subresourceRange.levelCount = 1;
    currentBarrier.subresourceRange.baseArrayLayer = 0;
    currentBarrier.subresourceRange.layerCount = 1;
    currentBarrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    currentBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    
    VkImageMemoryBarrier barriers[2] = {historyBarrier, currentBarrier};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 2, barriers);
    
    // Copy current frame to history
    VkImageCopy copyRegion{};
    copyRegion.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copyRegion.srcSubresource.mipLevel = 0;
    copyRegion.srcSubresource.baseArrayLayer = 0;
    copyRegion.srcSubresource.layerCount = 1;
    copyRegion.dstSubresource = copyRegion.srcSubresource;
    copyRegion.extent.width = extent.width;
    copyRegion.extent.height = extent.height;
    copyRegion.extent.depth = 1;
    
    vkCmdCopyImage(cmd, m_taaCurrentImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   m_taaHistoryImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRegion);
    
    // Transition both images back to shader read
    historyBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    historyBarrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    historyBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    historyBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    
    currentBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    currentBarrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    currentBarrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    currentBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    
    VkImageMemoryBarrier finalBarriers[2] = {historyBarrier, currentBarrier};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 2, finalBarriers);
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

    if (m_volumePipelineHDR != VK_NULL_HANDLE) {
        vkDestroyPipeline(m_context->getDevice(), m_volumePipelineHDR, nullptr);
        m_volumePipelineHDR = VK_NULL_HANDLE;
    }
    
    if (m_volumePipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(m_context->getDevice(), m_volumePipelineLayout, nullptr);
        m_volumePipelineLayout = VK_NULL_HANDLE;
    }
    
    // Clean up TAA pipeline resources
    if (m_taaPipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(m_context->getDevice(), m_taaPipeline, nullptr);
        m_taaPipeline = VK_NULL_HANDLE;
    }
    
    if (m_taaPipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(m_context->getDevice(), m_taaPipelineLayout, nullptr);
        m_taaPipelineLayout = VK_NULL_HANDLE;
    }
    
    if (m_taaDescriptorSetLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(m_context->getDevice(), m_taaDescriptorSetLayout, nullptr);
        m_taaDescriptorSetLayout = VK_NULL_HANDLE;
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

    if (m_densityStorageView != VK_NULL_HANDLE) {
        vkDestroyImageView(m_context->getDevice(), m_densityStorageView, nullptr);
        m_densityStorageView = VK_NULL_HANDLE;
    }
    
    if (m_densityImage != VK_NULL_HANDLE) {
        vkDestroyImage(m_context->getDevice(), m_densityImage, nullptr);
        m_densityImage = VK_NULL_HANDLE;
    }
    
    if (m_densityImageMemory != VK_NULL_HANDLE) {
        vkFreeMemory(m_context->getDevice(), m_densityImageMemory, nullptr);
        m_densityImageMemory = VK_NULL_HANDLE;
    }
    
    // Clean up STBN resources
    if (m_stbnSampler != VK_NULL_HANDLE) {
        vkDestroySampler(m_context->getDevice(), m_stbnSampler, nullptr);
        m_stbnSampler = VK_NULL_HANDLE;
    }
    
    if (m_stbnImageView != VK_NULL_HANDLE) {
        vkDestroyImageView(m_context->getDevice(), m_stbnImageView, nullptr);
        m_stbnImageView = VK_NULL_HANDLE;
    }
    
    if (m_stbnImage != VK_NULL_HANDLE) {
        vkDestroyImage(m_context->getDevice(), m_stbnImage, nullptr);
        m_stbnImage = VK_NULL_HANDLE;
    }
    
    if (m_stbnMemory != VK_NULL_HANDLE) {
        vkFreeMemory(m_context->getDevice(), m_stbnMemory, nullptr);
        m_stbnMemory = VK_NULL_HANDLE;
    }
    
    // Clean up optical depth LUT resources
    if (m_opticalDepthLUTSampler != VK_NULL_HANDLE) {
        vkDestroySampler(m_context->getDevice(), m_opticalDepthLUTSampler, nullptr);
        m_opticalDepthLUTSampler = VK_NULL_HANDLE;
    }
    
    if (m_opticalDepthLUTView != VK_NULL_HANDLE) {
        vkDestroyImageView(m_context->getDevice(), m_opticalDepthLUTView, nullptr);
        m_opticalDepthLUTView = VK_NULL_HANDLE;
    }
    
    if (m_opticalDepthLUT != VK_NULL_HANDLE) {
        vkDestroyImage(m_context->getDevice(), m_opticalDepthLUT, nullptr);
        m_opticalDepthLUT = VK_NULL_HANDLE;
    }
    
    if (m_opticalDepthLUTMemory != VK_NULL_HANDLE) {
        vkFreeMemory(m_context->getDevice(), m_opticalDepthLUTMemory, nullptr);
        m_opticalDepthLUTMemory = VK_NULL_HANDLE;
    }
    
    // Clean up TAA resources
    if (m_taaHistorySampler != VK_NULL_HANDLE) {
        vkDestroySampler(m_context->getDevice(), m_taaHistorySampler, nullptr);
        m_taaHistorySampler = VK_NULL_HANDLE;
    }
    
    if (m_taaHistoryImageView != VK_NULL_HANDLE) {
        vkDestroyImageView(m_context->getDevice(), m_taaHistoryImageView, nullptr);
        m_taaHistoryImageView = VK_NULL_HANDLE;
    }
    
    if (m_taaHistoryImage != VK_NULL_HANDLE) {
        vkDestroyImage(m_context->getDevice(), m_taaHistoryImage, nullptr);
        m_taaHistoryImage = VK_NULL_HANDLE;
    }
    
    if (m_taaHistoryMemory != VK_NULL_HANDLE) {
        vkFreeMemory(m_context->getDevice(), m_taaHistoryMemory, nullptr);
        m_taaHistoryMemory = VK_NULL_HANDLE;
    }
    
    // Clean up TAA current frame resources
    if (m_taaCurrentSampler != VK_NULL_HANDLE) {
        vkDestroySampler(m_context->getDevice(), m_taaCurrentSampler, nullptr);
        m_taaCurrentSampler = VK_NULL_HANDLE;
    }
    
    if (m_taaCurrentImageView != VK_NULL_HANDLE) {
        vkDestroyImageView(m_context->getDevice(), m_taaCurrentImageView, nullptr);
        m_taaCurrentImageView = VK_NULL_HANDLE;
    }
    
    if (m_taaCurrentImage != VK_NULL_HANDLE) {
        vkDestroyImage(m_context->getDevice(), m_taaCurrentImage, nullptr);
        m_taaCurrentImage = VK_NULL_HANDLE;
    }
    
    if (m_taaCurrentMemory != VK_NULL_HANDLE) {
        vkFreeMemory(m_context->getDevice(), m_taaCurrentMemory, nullptr);
        m_taaCurrentMemory = VK_NULL_HANDLE;
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

void VolumeRenderer::setRuntimeParameters(float densityScale, float opacityScale, float stepSize, 
                                        float emissionScale, int maxSteps,
                                        float tempOffset, float tempRange, float saturation) {
    m_runtimeDensityScale = densityScale;
    m_runtimeOpacityScale = opacityScale;
    m_runtimeStepSize = stepSize;
    m_runtimeEmissionScale = emissionScale;
    m_runtimeMaxSteps = maxSteps;
    m_runtimeTempOffset = tempOffset;
    m_runtimeTempRange = tempRange;
    m_runtimeSaturation = saturation;
}

void VolumeRenderer::setTAAParameters(float blendFactor) {
    m_taaBlendFactor = blendFactor;
}

void VolumeRenderer::updateTAAMatrix(const glm::mat4& viewProj) {
    // Update previous view-projection matrix after TAA processing for correct reprojection next frame
    m_previousViewProjMatrix = viewProj;
    m_taaFirstFrame = false;  // Clear first frame flag
}

void VolumeRenderer::setVolumeDetailParameters(float voxelSize) {
    // Check if voxel size actually changed
    if (std::abs(m_params.voxelSize - voxelSize) > 0.001f) {
        std::cout << "[VOLUME] Voxel size changed from " << m_params.voxelSize << " to " << voxelSize << std::endl;
        
        // Update parameters
        m_params.voxelSize = voxelSize;
        
        // Recalculate grid dimensions to maintain world bounds
        glm::vec3 worldSize = glm::vec3(60.0f); // Current world bounds: 60x60x60
        m_params.gridDimensions = glm::uvec3(
            static_cast<uint32_t>(worldSize.x / voxelSize),
            static_cast<uint32_t>(worldSize.y / voxelSize), 
            static_cast<uint32_t>(worldSize.z / voxelSize)
        );
        
        std::cout << "[VOLUME] New grid dimensions: " << m_params.gridDimensions.x << "x" 
                  << m_params.gridDimensions.y << "x" << m_params.gridDimensions.z << std::endl;
        
        // Recreate density grid with new dimensions
        recreateDensityGrid();
        
        // Reset density initialization flag - grid needs to be updated
        m_densityInitialized = false;
    }
}

void VolumeRenderer::recreateDensityGrid() {
    std::cout << "[VOLUME] Recreating density grid..." << std::endl;
    
    VkDevice device = m_context->getDevice();
    
    // Wait for any pending operations
    vkDeviceWaitIdle(device);
    
    // Clean up old density resources
    if (m_densitySampler != VK_NULL_HANDLE) {
        vkDestroySampler(device, m_densitySampler, nullptr);
    }
    if (m_densityImageView != VK_NULL_HANDLE) {
        vkDestroyImageView(device, m_densityImageView, nullptr);
    }
    if (m_densityStorageView != VK_NULL_HANDLE) {
        vkDestroyImageView(device, m_densityStorageView, nullptr);
    }
    if (m_densityImage != VK_NULL_HANDLE) {
        vkDestroyImage(device, m_densityImage, nullptr);
    }
    if (m_densityImageMemory != VK_NULL_HANDLE) {
        vkFreeMemory(device, m_densityImageMemory, nullptr);
    }
    
    // Recreate density grid with new parameters
    createDensityGrid();
    
    std::cout << "[VOLUME] Density grid recreated successfully" << std::endl;
}

void VolumeRenderer::cleanupTAAResources() {
    VkDevice device = m_context->getDevice();
    
    // Cleanup TAA current frame resources
    if (m_taaCurrentSampler != VK_NULL_HANDLE) {
        vkDestroySampler(device, m_taaCurrentSampler, nullptr);
        m_taaCurrentSampler = VK_NULL_HANDLE;
    }
    if (m_taaCurrentImageView != VK_NULL_HANDLE) {
        vkDestroyImageView(device, m_taaCurrentImageView, nullptr);
        m_taaCurrentImageView = VK_NULL_HANDLE;
    }
    if (m_taaCurrentImage != VK_NULL_HANDLE) {
        vkDestroyImage(device, m_taaCurrentImage, nullptr);
        m_taaCurrentImage = VK_NULL_HANDLE;
    }
    if (m_taaCurrentMemory != VK_NULL_HANDLE) {
        vkFreeMemory(device, m_taaCurrentMemory, nullptr);
        m_taaCurrentMemory = VK_NULL_HANDLE;
    }
    
    // Cleanup TAA history resources
    if (m_taaHistorySampler != VK_NULL_HANDLE) {
        vkDestroySampler(device, m_taaHistorySampler, nullptr);
        m_taaHistorySampler = VK_NULL_HANDLE;
    }
    if (m_taaHistoryImageView != VK_NULL_HANDLE) {
        vkDestroyImageView(device, m_taaHistoryImageView, nullptr);
        m_taaHistoryImageView = VK_NULL_HANDLE;
    }
    if (m_taaHistoryImage != VK_NULL_HANDLE) {
        vkDestroyImage(device, m_taaHistoryImage, nullptr);
        m_taaHistoryImage = VK_NULL_HANDLE;
    }
    if (m_taaHistoryMemory != VK_NULL_HANDLE) {
        vkFreeMemory(device, m_taaHistoryMemory, nullptr);
        m_taaHistoryMemory = VK_NULL_HANDLE;
    }
}

void VolumeRenderer::onSwapchainResized(VkExtent2D newExtent) {
    std::cout << "[TAA] Handling swapchain resize: " << newExtent.width << "x" << newExtent.height << std::endl;
    
    // Clean up old TAA resources
    cleanupTAAResources();
    
    // Recreate TAA resources with new extent
    createTAAResources();
    
    // Reset TAA state for first frame after resize
    m_taaFirstFrame = true;
    m_previousViewProjMatrix = glm::mat4(1.0f);
    
    std::cout << "[TAA] Resources recreated for new size" << std::endl;
}

void VolumeRenderer::compositeTAAResult(VkCommandBuffer cmd) {
    // Composite TAA result to main framebuffer as fullscreen quad
    // NOTE: All image transitions must be done outside dynamic rendering instances
    // This function should only bind pipeline and draw - no barriers allowed here

    VkExtent2D swapchainExtent = m_taaExtent;

    // Bind TAA pipeline for composition
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_taaPipeline);

    // Set up TAA constants for composition (blend factor = 0.0 means show pure history)
    struct TAAConstants {
        glm::mat4 currentToHistory;
        glm::vec2 screenSize;
        float blendFactor;
        uint32_t firstFrame;
    };

    TAAConstants constants{};
    constants.currentToHistory = glm::mat4(1.0f); // Not used for composition
    constants.screenSize = glm::vec2(swapchainExtent.width, swapchainExtent.height);
    constants.blendFactor = 0.0f;  // 0.0 = pure history, 1.0 = pure current
    constants.firstFrame = 0u;

    vkCmdPushConstants(cmd, m_taaPipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(TAAConstants), &constants);

    // Set up descriptors for BOTH textures (TAA shader expects both)
    VkDescriptorImageInfo imageInfos[2] = {};

    // Binding 0: Current frame (use history as dummy since we want pure history)
    imageInfos[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageInfos[0].imageView = m_taaHistoryImageView;
    imageInfos[0].sampler = m_taaHistorySampler;

    // Binding 1: History frame (the actual TAA result)
    imageInfos[1].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageInfos[1].imageView = m_taaHistoryImageView;
    imageInfos[1].sampler = m_taaHistorySampler;

    VkWriteDescriptorSet descriptorWrites[2] = {};
    descriptorWrites[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptorWrites[0].dstBinding = 0;
    descriptorWrites[0].dstArrayElement = 0;
    descriptorWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    descriptorWrites[0].descriptorCount = 1;
    descriptorWrites[0].pImageInfo = &imageInfos[0];

    descriptorWrites[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptorWrites[1].dstBinding = 1;
    descriptorWrites[1].dstArrayElement = 0;
    descriptorWrites[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    descriptorWrites[1].descriptorCount = 1;
    descriptorWrites[1].pImageInfo = &imageInfos[1];

    vkCmdPushDescriptorSet(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_taaPipelineLayout,
                           0, 2, descriptorWrites);

    // Draw fullscreen triangle to show TAA result
    vkCmdDraw(cmd, 3, 1, 0, 0);
}

void VolumeRenderer::generateMipChain(VkCommandBuffer cmd) {
    // Generate mip chain using vkCmdBlitImage for 3D texture downsample
    VkImageMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.image = m_densityImage;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;
    barrier.subresourceRange.levelCount = 1;

    int32_t mipWidth = m_params.gridDimensions.x;
    int32_t mipHeight = m_params.gridDimensions.y;
    int32_t mipDepth = m_params.gridDimensions.z;

    for (uint32_t i = 1; i < m_densityMipLevels; i++) {
        // Transition previous mip to transfer source
        barrier.subresourceRange.baseMipLevel = i - 1;
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        // Mip level 0 starts in SHADER_READ_ONLY_OPTIMAL from previous frame's volume render
        // Other mips start in UNDEFINED (never written before)
        barrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

        VkDependencyInfo dependencyInfo{};
        dependencyInfo.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dependencyInfo.imageMemoryBarrierCount = 1;
        dependencyInfo.pImageMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(cmd, &dependencyInfo);

        // Transition current mip to transfer destination
        barrier.subresourceRange.baseMipLevel = i;
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_NONE;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        vkCmdPipelineBarrier2(cmd, &dependencyInfo);

        // Blit from previous mip to current mip
        VkImageBlit blit{};
        blit.srcOffsets[0] = {0, 0, 0};
        blit.srcOffsets[1] = {mipWidth, mipHeight, mipDepth};
        blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.srcSubresource.mipLevel = i - 1;
        blit.srcSubresource.baseArrayLayer = 0;
        blit.srcSubresource.layerCount = 1;
        
        mipWidth = std::max(1, mipWidth / 2);
        mipHeight = std::max(1, mipHeight / 2);
        mipDepth = std::max(1, mipDepth / 2);

        blit.dstOffsets[0] = {0, 0, 0};
        blit.dstOffsets[1] = {mipWidth, mipHeight, mipDepth};
        blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.dstSubresource.mipLevel = i;
        blit.dstSubresource.baseArrayLayer = 0;
        blit.dstSubresource.layerCount = 1;

        vkCmdBlitImage(cmd, m_densityImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       m_densityImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       1, &blit, VK_FILTER_LINEAR);

        // Transition current mip to shader read
        barrier.subresourceRange.baseMipLevel = i;
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        vkCmdPipelineBarrier2(cmd, &dependencyInfo);

        // Transition previous source mip (i-1) back to shader read, except for mip 0 which is handled at the end
        if (i > 1) {
            barrier.subresourceRange.baseMipLevel = i - 1;
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
            barrier.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
            barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            vkCmdPipelineBarrier2(cmd, &dependencyInfo);
        }
    }

    // Transition mip level 0 to shader read
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    
    VkDependencyInfo finalDependencyInfo{};
    finalDependencyInfo.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    finalDependencyInfo.imageMemoryBarrierCount = 1;
    finalDependencyInfo.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &finalDependencyInfo);
}

bool VolumeRenderer::supportsRayTracing() const {
    // Check if the Vulkan context supports ray query and acceleration structures
    // For now, assume RT is supported if the extensions were loaded successfully
    return true; // TODO: Add proper RT support detection
}

void VolumeRenderer::createAccelerationStructures() {
    if (!supportsRayTracing()) {
        std::cout << "Ray tracing not supported, skipping acceleration structures" << std::endl;
        return;
    }

    VkDevice device = m_context->getDevice();

    // Stage 1: Create BLAS for shadow catcher disc (2 triangles forming a horizontal plane)
    // Simple disc vertices: 2 triangles forming a square below the volume
    float discVertices[] = {
        // Triangle 1
        -20.0f, -40.0f, -20.0f,  // Bottom-left
         20.0f, -40.0f, -20.0f,  // Bottom-right
        -20.0f, -40.0f,  20.0f,  // Top-left
        // Triangle 2
         20.0f, -40.0f, -20.0f,  // Bottom-right
         20.0f, -40.0f,  20.0f,  // Top-right
        -20.0f, -40.0f,  20.0f   // Top-left
    };

    // Create vertex buffer with RT usage flags
    VkBufferCreateInfo vertexBufferInfo{};
    vertexBufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    vertexBufferInfo.size = sizeof(discVertices);
    vertexBufferInfo.usage = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                             VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    vertexBufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateBuffer(device, &vertexBufferInfo, nullptr, &m_vertexBuffer) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create RT vertex buffer!");
    }

    // Allocate and bind vertex buffer memory
    VkMemoryRequirements vertexMemReqs;
    vkGetBufferMemoryRequirements(device, m_vertexBuffer, &vertexMemReqs);

    VkMemoryAllocateFlagsInfo vertexAllocFlags{};
    vertexAllocFlags.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
    vertexAllocFlags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;

    VkMemoryAllocateInfo vertexAllocInfo{};
    vertexAllocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    vertexAllocInfo.pNext = &vertexAllocFlags;
    vertexAllocInfo.allocationSize = vertexMemReqs.size;
    vertexAllocInfo.memoryTypeIndex = findMemoryType(vertexMemReqs.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    if (vkAllocateMemory(device, &vertexAllocInfo, nullptr, &m_vertexMemory) != VK_SUCCESS) {
        throw std::runtime_error("Failed to allocate RT vertex buffer memory!");
    }

    vkBindBufferMemory(device, m_vertexBuffer, m_vertexMemory, 0);

    // Upload vertex data
    void* vertexData;
    vkMapMemory(device, m_vertexMemory, 0, sizeof(discVertices), 0, &vertexData);
    memcpy(vertexData, discVertices, sizeof(discVertices));
    vkUnmapMemory(device, m_vertexMemory);

    // Get vertex buffer device address
    VkBufferDeviceAddressInfo vertexAddrInfo{};
    vertexAddrInfo.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    vertexAddrInfo.buffer = m_vertexBuffer;
    VkDeviceAddress vertexAddress = vkGetBufferDeviceAddress(device, &vertexAddrInfo);

    // Create BLAS geometry description
    VkAccelerationStructureGeometryKHR geometry{};
    geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
    geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
    geometry.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
    geometry.geometry.triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
    geometry.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
    geometry.geometry.triangles.vertexData.deviceAddress = vertexAddress;
    geometry.geometry.triangles.vertexStride = sizeof(float) * 3;
    geometry.geometry.triangles.indexType = VK_INDEX_TYPE_NONE_KHR;
    geometry.geometry.triangles.indexData.deviceAddress = 0;
    geometry.geometry.triangles.transformData.deviceAddress = 0;
    geometry.geometry.triangles.maxVertex = 5;  // 6 vertices - 1

    // Get BLAS build sizes
    VkAccelerationStructureBuildGeometryInfoKHR blasBuildInfo{};
    blasBuildInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
    blasBuildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    blasBuildInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    blasBuildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    blasBuildInfo.geometryCount = 1;
    blasBuildInfo.pGeometries = &geometry;

    uint32_t primitiveCount = 2;  // 2 triangles
    VkAccelerationStructureBuildSizesInfoKHR blasBuildSizes{};
    blasBuildSizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
    vkGetAccelerationStructureBuildSizesKHR(device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
        &blasBuildInfo, &primitiveCount, &blasBuildSizes);

    // Create BLAS buffer
    VkBufferCreateInfo blasBufferInfo{};
    blasBufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    blasBufferInfo.size = blasBuildSizes.accelerationStructureSize;
    blasBufferInfo.usage = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                           VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

    if (vkCreateBuffer(device, &blasBufferInfo, nullptr, &m_blasBuffer) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create BLAS buffer!");
    }

    VkMemoryRequirements blasMemReqs;
    vkGetBufferMemoryRequirements(device, m_blasBuffer, &blasMemReqs);

    VkMemoryAllocateFlagsInfo blasAllocFlags{};
    blasAllocFlags.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
    blasAllocFlags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;

    VkMemoryAllocateInfo blasAllocInfo{};
    blasAllocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    blasAllocInfo.pNext = &blasAllocFlags;
    blasAllocInfo.allocationSize = blasMemReqs.size;
    blasAllocInfo.memoryTypeIndex = findMemoryType(blasMemReqs.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    if (vkAllocateMemory(device, &blasAllocInfo, nullptr, &m_blasMemory) != VK_SUCCESS) {
        throw std::runtime_error("Failed to allocate BLAS memory!");
    }

    vkBindBufferMemory(device, m_blasBuffer, m_blasMemory, 0);

    // Create BLAS acceleration structure
    VkAccelerationStructureCreateInfoKHR blasCreateInfo{};
    blasCreateInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
    blasCreateInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    blasCreateInfo.buffer = m_blasBuffer;
    blasCreateInfo.size = blasBuildSizes.accelerationStructureSize;

    if (vkCreateAccelerationStructureKHR(device, &blasCreateInfo, nullptr, &m_bottomLevelAS) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create BLAS!");
    }

    std::cout << "BLAS created successfully - shadow catcher disc ready" << std::endl;
    m_rayTracingInitialized = true;
}

void VolumeRenderer::buildAccelerationStructures(VkCommandBuffer cmd) {
    if (!supportsRayTracing() || !m_rayTracingInitialized) {
        return;
    }

    // Check if acceleration structures are already built (prevent rebuilding every frame)
    static bool s_accelerationStructuresBuilt = false;
    if (s_accelerationStructuresBuilt) {
        return;
    }

    VkDevice device = m_context->getDevice();

    // Create TLAS instance referencing the BLAS
    VkAccelerationStructureDeviceAddressInfoKHR blasAddrInfo{};
    blasAddrInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
    blasAddrInfo.accelerationStructure = m_bottomLevelAS;
    VkDeviceAddress blasAddress = vkGetAccelerationStructureDeviceAddressKHR(device, &blasAddrInfo);

    VkAccelerationStructureInstanceKHR instance{};
    instance.transform = { 1,0,0,0,  0,1,0,0,  0,0,1,0 };  // Identity transform
    instance.instanceCustomIndex = 0;
    instance.mask = 0xFF;
    instance.instanceShaderBindingTableRecordOffset = 0;
    instance.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
    instance.accelerationStructureReference = blasAddress;

    // Create instance buffer
    VkBufferCreateInfo instanceBufferInfo{};
    instanceBufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    instanceBufferInfo.size = sizeof(VkAccelerationStructureInstanceKHR);
    instanceBufferInfo.usage = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                               VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

    if (vkCreateBuffer(device, &instanceBufferInfo, nullptr, &m_instanceBuffer) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create instance buffer!");
    }

    VkMemoryRequirements instanceMemReqs;
    vkGetBufferMemoryRequirements(device, m_instanceBuffer, &instanceMemReqs);

    VkMemoryAllocateFlagsInfo instanceAllocFlags{};
    instanceAllocFlags.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
    instanceAllocFlags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;

    VkMemoryAllocateInfo instanceAllocInfo{};
    instanceAllocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    instanceAllocInfo.pNext = &instanceAllocFlags;
    instanceAllocInfo.allocationSize = instanceMemReqs.size;
    instanceAllocInfo.memoryTypeIndex = findMemoryType(instanceMemReqs.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    if (vkAllocateMemory(device, &instanceAllocInfo, nullptr, &m_instanceMemory) != VK_SUCCESS) {
        throw std::runtime_error("Failed to allocate instance buffer memory!");
    }

    vkBindBufferMemory(device, m_instanceBuffer, m_instanceMemory, 0);

    // Upload instance data
    void* instanceData;
    vkMapMemory(device, m_instanceMemory, 0, sizeof(instance), 0, &instanceData);
    memcpy(instanceData, &instance, sizeof(instance));
    vkUnmapMemory(device, m_instanceMemory);

    // Get instance buffer device address
    VkBufferDeviceAddressInfo instanceAddrInfo{};
    instanceAddrInfo.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    instanceAddrInfo.buffer = m_instanceBuffer;
    VkDeviceAddress instanceAddress = vkGetBufferDeviceAddress(device, &instanceAddrInfo);

    // Create TLAS geometry
    VkAccelerationStructureGeometryKHR tlasGeometry{};
    tlasGeometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
    tlasGeometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    tlasGeometry.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    tlasGeometry.geometry.instances.data.deviceAddress = instanceAddress;

    // Get TLAS build sizes
    VkAccelerationStructureBuildGeometryInfoKHR tlasBuildInfo{};
    tlasBuildInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
    tlasBuildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    tlasBuildInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    tlasBuildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    tlasBuildInfo.geometryCount = 1;
    tlasBuildInfo.pGeometries = &tlasGeometry;

    uint32_t instanceCount = 1;
    VkAccelerationStructureBuildSizesInfoKHR tlasBuildSizes{};
    tlasBuildSizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
    vkGetAccelerationStructureBuildSizesKHR(device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
        &tlasBuildInfo, &instanceCount, &tlasBuildSizes);

    // Create TLAS buffer
    VkBufferCreateInfo tlasBufferInfo{};
    tlasBufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    tlasBufferInfo.size = tlasBuildSizes.accelerationStructureSize;
    tlasBufferInfo.usage = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                           VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

    if (vkCreateBuffer(device, &tlasBufferInfo, nullptr, &m_tlasBuffer) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create TLAS buffer!");
    }

    VkMemoryRequirements tlasMemReqs;
    vkGetBufferMemoryRequirements(device, m_tlasBuffer, &tlasMemReqs);

    VkMemoryAllocateFlagsInfo tlasAllocFlags{};
    tlasAllocFlags.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
    tlasAllocFlags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;

    VkMemoryAllocateInfo tlasAllocInfo{};
    tlasAllocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    tlasAllocInfo.pNext = &tlasAllocFlags;
    tlasAllocInfo.allocationSize = tlasMemReqs.size;
    tlasAllocInfo.memoryTypeIndex = findMemoryType(tlasMemReqs.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    if (vkAllocateMemory(device, &tlasAllocInfo, nullptr, &m_tlasMemory) != VK_SUCCESS) {
        throw std::runtime_error("Failed to allocate TLAS memory!");
    }

    vkBindBufferMemory(device, m_tlasBuffer, m_tlasMemory, 0);

    // Create TLAS acceleration structure
    VkAccelerationStructureCreateInfoKHR tlasCreateInfo{};
    tlasCreateInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
    tlasCreateInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    tlasCreateInfo.buffer = m_tlasBuffer;
    tlasCreateInfo.size = tlasBuildSizes.accelerationStructureSize;

    if (vkCreateAccelerationStructureKHR(device, &tlasCreateInfo, nullptr, &m_topLevelAS) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create TLAS!");
    }

    // Create scratch buffers for building
    // BLAS scratch buffer
    VkBufferCreateInfo blasScratchBufferInfo{};
    blasScratchBufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;

    // Get BLAS build sizes for scratch buffer
    VkAccelerationStructureGeometryKHR geometry{};
    geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
    geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
    geometry.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
    geometry.geometry.triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
    geometry.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;

    VkBufferDeviceAddressInfo vertexAddrInfo{};
    vertexAddrInfo.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    vertexAddrInfo.buffer = m_vertexBuffer;
    VkDeviceAddress vertexAddress = vkGetBufferDeviceAddress(device, &vertexAddrInfo);

    geometry.geometry.triangles.vertexData.deviceAddress = vertexAddress;
    geometry.geometry.triangles.vertexStride = sizeof(float) * 3;
    geometry.geometry.triangles.indexType = VK_INDEX_TYPE_NONE_KHR;
    geometry.geometry.triangles.maxVertex = 5;

    VkAccelerationStructureBuildGeometryInfoKHR blasBuildInfo{};
    blasBuildInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
    blasBuildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    blasBuildInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    blasBuildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    blasBuildInfo.geometryCount = 1;
    blasBuildInfo.pGeometries = &geometry;
    blasBuildInfo.dstAccelerationStructure = m_bottomLevelAS;

    uint32_t primitiveCount = 2;
    VkAccelerationStructureBuildSizesInfoKHR blasBuildSizes{};
    blasBuildSizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
    vkGetAccelerationStructureBuildSizesKHR(device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
        &blasBuildInfo, &primitiveCount, &blasBuildSizes);

    // Create BLAS scratch buffer (temporary for this function)
    VkBuffer blasScratchBuffer = VK_NULL_HANDLE;
    VkDeviceMemory blasScratchMemory = VK_NULL_HANDLE;

    blasScratchBufferInfo.size = blasBuildSizes.buildScratchSize;
    blasScratchBufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

    if (vkCreateBuffer(device, &blasScratchBufferInfo, nullptr, &blasScratchBuffer) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create BLAS scratch buffer!");
    }

    VkMemoryRequirements blasScratchMemReqs;
    vkGetBufferMemoryRequirements(device, blasScratchBuffer, &blasScratchMemReqs);

    VkMemoryAllocateFlagsInfo blasScratchAllocFlags{};
    blasScratchAllocFlags.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
    blasScratchAllocFlags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;

    VkMemoryAllocateInfo blasScratchAllocInfo{};
    blasScratchAllocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    blasScratchAllocInfo.pNext = &blasScratchAllocFlags;
    blasScratchAllocInfo.allocationSize = blasScratchMemReqs.size;
    blasScratchAllocInfo.memoryTypeIndex = findMemoryType(blasScratchMemReqs.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    if (vkAllocateMemory(device, &blasScratchAllocInfo, nullptr, &blasScratchMemory) != VK_SUCCESS) {
        throw std::runtime_error("Failed to allocate BLAS scratch memory!");
    }

    vkBindBufferMemory(device, blasScratchBuffer, blasScratchMemory, 0);

    VkBufferDeviceAddressInfo blasScratchAddrInfo{};
    blasScratchAddrInfo.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    blasScratchAddrInfo.buffer = blasScratchBuffer;
    VkDeviceAddress blasScratchAddress = vkGetBufferDeviceAddress(device, &blasScratchAddrInfo);

    blasBuildInfo.scratchData.deviceAddress = blasScratchAddress;

    // Build BLAS
    VkAccelerationStructureBuildRangeInfoKHR blasBuildRange{};
    blasBuildRange.primitiveCount = primitiveCount;
    const VkAccelerationStructureBuildRangeInfoKHR* blasBuildRanges[1] = { &blasBuildRange };

    vkCmdBuildAccelerationStructuresKHR(cmd, 1, &blasBuildInfo, blasBuildRanges);

    // Barrier between BLAS and TLAS build
    VkMemoryBarrier2 blasBarrier{};
    blasBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    blasBarrier.srcStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
    blasBarrier.srcAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    blasBarrier.dstStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
    blasBarrier.dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;

    VkDependencyInfo blasDep{};
    blasDep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    blasDep.memoryBarrierCount = 1;
    blasDep.pMemoryBarriers = &blasBarrier;

    vkCmdPipelineBarrier2(cmd, &blasDep);

    // Build TLAS with scratch buffer
    VkBuffer tlasScratchBuffer = VK_NULL_HANDLE;
    VkDeviceMemory tlasScratchMemory = VK_NULL_HANDLE;

    VkBufferCreateInfo tlasScratchBufferInfo{};
    tlasScratchBufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    tlasScratchBufferInfo.size = tlasBuildSizes.buildScratchSize;
    tlasScratchBufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

    if (vkCreateBuffer(device, &tlasScratchBufferInfo, nullptr, &tlasScratchBuffer) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create TLAS scratch buffer!");
    }

    VkMemoryRequirements tlasScratchMemReqs;
    vkGetBufferMemoryRequirements(device, tlasScratchBuffer, &tlasScratchMemReqs);

    VkMemoryAllocateFlagsInfo tlasScratchAllocFlags{};
    tlasScratchAllocFlags.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
    tlasScratchAllocFlags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;

    VkMemoryAllocateInfo tlasScratchAllocInfo{};
    tlasScratchAllocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    tlasScratchAllocInfo.pNext = &tlasScratchAllocFlags;
    tlasScratchAllocInfo.allocationSize = tlasScratchMemReqs.size;
    tlasScratchAllocInfo.memoryTypeIndex = findMemoryType(tlasScratchMemReqs.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    if (vkAllocateMemory(device, &tlasScratchAllocInfo, nullptr, &tlasScratchMemory) != VK_SUCCESS) {
        throw std::runtime_error("Failed to allocate TLAS scratch memory!");
    }

    vkBindBufferMemory(device, tlasScratchBuffer, tlasScratchMemory, 0);

    VkBufferDeviceAddressInfo tlasScratchAddrInfo{};
    tlasScratchAddrInfo.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    tlasScratchAddrInfo.buffer = tlasScratchBuffer;
    VkDeviceAddress tlasScratchAddress = vkGetBufferDeviceAddress(device, &tlasScratchAddrInfo);

    tlasBuildInfo.dstAccelerationStructure = m_topLevelAS;
    tlasBuildInfo.scratchData.deviceAddress = tlasScratchAddress;

    // Build TLAS
    VkAccelerationStructureBuildRangeInfoKHR tlasBuildRange{};
    tlasBuildRange.primitiveCount = instanceCount;
    const VkAccelerationStructureBuildRangeInfoKHR* tlasBuildRanges[1] = { &tlasBuildRange };

    vkCmdBuildAccelerationStructuresKHR(cmd, 1, &tlasBuildInfo, tlasBuildRanges);

    // Final barrier to make TLAS visible to fragment shader
    VkMemoryBarrier2 finalBarrier{};
    finalBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    finalBarrier.srcStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
    finalBarrier.srcAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    finalBarrier.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    finalBarrier.dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;

    VkDependencyInfo finalDep{};
    finalDep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    finalDep.memoryBarrierCount = 1;
    finalDep.pMemoryBarriers = &finalBarrier;

    vkCmdPipelineBarrier2(cmd, &finalDep);

    // Cleanup temporary scratch buffers
    vkDestroyBuffer(device, blasScratchBuffer, nullptr);
    vkFreeMemory(device, blasScratchMemory, nullptr);
    vkDestroyBuffer(device, tlasScratchBuffer, nullptr);
    vkFreeMemory(device, tlasScratchMemory, nullptr);

    std::cout << "BLAS and TLAS built successfully with proper synchronization!" << std::endl;
    s_accelerationStructuresBuilt = true;
}

} // namespace plasma
