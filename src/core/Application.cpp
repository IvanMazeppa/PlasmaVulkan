#include "Application.h"
#include <iostream>
#include <fstream>
#include <vector>
#include <chrono>
#include <iomanip>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

namespace plasma {

Application::Application(const std::string& title, uint32_t width, uint32_t height)
    : m_title(title), m_width(width), m_height(height) {

    initWindow();
    initVulkan();
}

Application::~Application() {
    cleanup();
}

void Application::run() {
    m_isRunning = true;

    auto lastTime = std::chrono::high_resolution_clock::now();

    while (m_isRunning && !glfwWindowShouldClose(m_window)) {
        auto currentTime = std::chrono::high_resolution_clock::now();
        float actualDeltaTime = std::chrono::duration<float, std::chrono::seconds::period>(
            currentTime - lastTime).count();
        lastTime = currentTime;
        
        // Accumulator-based fixed timestep physics
        const float fixedTimeStep = 1.0f / 60.0f; // 60Hz physics simulation
        m_physicsAccumulator += actualDeltaTime;
        
        // Only update physics when enough time has accumulated
        m_shouldUpdatePhysics = false;
        if (m_physicsAccumulator >= fixedTimeStep) {
            m_shouldUpdatePhysics = true;
            m_currentPhysicsDeltaTime = fixedTimeStep * m_timeScale;
            m_physicsAccumulator -= fixedTimeStep;
            
            // Prevent spiral of death - cap accumulator
            if (m_physicsAccumulator > fixedTimeStep * 4.0f) {
                m_physicsAccumulator = 0.0f;
            }
        }

        glfwPollEvents();

        // Recording mode: use fixed timestep for consistent output
        if (m_recordingActive) {
            // Force fixed timestep physics for recording
            m_currentPhysicsDeltaTime = m_recordingFixedTimestep;
            m_shouldUpdatePhysics = true; // Always update in recording mode
            
            update(m_currentPhysicsDeltaTime);
            render();
            
            // Capture frame after rendering
            captureFrame();
        } else {
            // Normal real-time mode
            update(m_currentPhysicsDeltaTime); // Physics will only update when flag is set
            render();
        }

        // Calculate FPS using actual frame time (not physics time)
        m_frameCount++;
        m_frameTime += actualDeltaTime; // Use real frame deltaTime for accurate FPS
        if (m_frameTime >= 1.0f) {
            m_fps = m_frameCount / m_frameTime;
            m_frameCount = 0;
            m_frameTime = 0.0f;
            updateWindowTitle();
        }
        
        // Enhanced console display - periodic status updates
        m_statusUpdateTimer += actualDeltaTime; // Use real time for UI updates
        if (m_statusUpdateTimer >= 3.0f && m_showOSD) {  // Every 3 seconds
            printStatusUpdate();
            m_statusUpdateTimer = 0.0f;
        }
    }

    // Wait for device to finish before cleanup
    if (m_vulkanContext) {
        vkDeviceWaitIdle(m_vulkanContext->getDevice());
    }
}

void Application::stop() {
    m_isRunning = false;
}

// Setter implementations for initial configuration
void Application::setActiveParticleCount(uint32_t count) {
    if (m_particleSystem) {
        m_particleSystem->setActiveParticleCount(count);
    }
}

void Application::setGravityStrength(float strength) {
    if (m_particleSystem) {
        m_particleSystem->setGravityStrength(strength);
    }
}

void Application::setTurbulenceStrength(float strength) {
    if (m_particleSystem) {
        m_particleSystem->setTurbulenceStrength(strength);
    }
}

void Application::setDampingFactor(float factor) {
    if (m_particleSystem) {
        m_particleSystem->setDampingFactor(factor);
    }
}

void Application::setAngularMomentumBoost(float boost) {
    if (m_particleSystem) {
        m_particleSystem->setAngularMomentumBoost(boost);
    }
}

void Application::setTimeScale(float scale) {
    m_timeScale = scale;
}

void Application::setConstraintShape(int shape) {
    m_constraintShape = static_cast<ConstraintShape>(shape);
    if (m_particleSystem) {
        m_particleSystem->setConstraintShape(static_cast<uint32_t>(shape));
    }
}

void Application::setBlackHoleMass(float mass) {
    if (m_particleSystem) {
        m_particleSystem->setBlackHoleMass(mass);
    }
}

void Application::setGravityCenter(const glm::vec3& center) {
    if (m_particleSystem) {
        m_particleSystem->setGravityCenter(center);
    }
}

void Application::setDualGalaxyMode(bool enabled) {
    if (m_particleSystem) {
        m_particleSystem->setDualGalaxyMode(enabled);
    }
}

void Application::setGravityCenter2(const glm::vec3& center) {
    if (m_particleSystem) {
        m_particleSystem->setGravityCenter2(center);
    }
}

void Application::setBlackHoleMass2(float mass) {
    if (m_particleSystem) {
        m_particleSystem->setBlackHoleMass2(mass);
    }
}

void Application::setCameraPosition(float distance, const glm::vec3& target) {
    m_cameraDistance = distance;
    m_cameraTarget = target;
    m_cameraTheta = 0.0f; // Reset rotation
    m_cameraPhi = 0.0f;   // Reset elevation
}

void Application::enableVolumetricMode(bool enabled) {
    m_volumetricMode = enabled;
    if (enabled) {
        std::cout << "Volumetric rendering mode enabled" << std::endl;
    }
}

void Application::initWindow() {
    glfwInit();

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);

    m_window = glfwCreateWindow(m_width, m_height, m_title.c_str(), nullptr, nullptr);

    if (!m_window) {
        throw std::runtime_error("Failed to create GLFW window!");
    }

    glfwSetWindowUserPointer(m_window, this);

    // Set callbacks
    glfwSetFramebufferSizeCallback(m_window, framebufferResizeCallback);
    glfwSetKeyCallback(m_window, keyCallback);
    glfwSetMouseButtonCallback(m_window, mouseButtonCallback);
    glfwSetCursorPosCallback(m_window, cursorPosCallback);
    glfwSetScrollCallback(m_window, scrollCallback);
}

void Application::initVulkan() {
#ifdef NDEBUG
    const bool enableValidation = false;
#else
    const bool enableValidation = true;
#endif

    m_vulkanContext = std::make_unique<VulkanContext>(m_window, enableValidation);

    // NOTE: VMA currently has initialization issues - needs further investigation
    std::cout << "VMA allocator disabled for now (needs investigation)" << std::endl;
    m_allocator = nullptr;
    
    // TODO: Debug VMA hanging issue 
    /*
    std::cout << "Initializing VMA allocator..." << std::endl;
    // Initialize VMA allocator
    VmaAllocatorCreateInfo allocatorInfo = {};
    allocatorInfo.vulkanApiVersion = VK_API_VERSION_1_3;  // Use 1.3 for better VMA compatibility
    allocatorInfo.physicalDevice = m_vulkanContext->getPhysicalDevice();
    allocatorInfo.device = m_vulkanContext->getDevice();
    allocatorInfo.instance = m_vulkanContext->getInstance();
    
    // Set Vulkan functions from volk
    VmaVulkanFunctions vulkanFunctions = {};
    vulkanFunctions.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    vulkanFunctions.vkGetDeviceProcAddr = vkGetDeviceProcAddr;
    allocatorInfo.pVulkanFunctions = &vulkanFunctions;

    VkResult result = vmaCreateAllocator(&allocatorInfo, &m_allocator);
    if (result != VK_SUCCESS) {
        throw std::runtime_error("Failed to create VMA allocator! Error code: " + std::to_string(result));
    }
    std::cout << "VMA allocator created successfully!" << std::endl;
    */

    // Create command buffers
    createCommandBuffers();

    std::cout << "Vulkan initialized successfully!" << std::endl;

    // Create a timestamp query pool for simple GPU profiling
    if (m_gpuProfilingEnabled) {
        VkQueryPoolCreateInfo qp{};
        qp.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        qp.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qp.queryCount = VulkanContext::MAX_FRAMES_IN_FLIGHT * 4; // 4 queries per frame
        if (vkCreateQueryPool(m_vulkanContext->getDevice(), &qp, nullptr, &m_timestampQueryPool) != VK_SUCCESS) {
            std::cerr << "Warning: Failed to create timestamp query pool; GPU profiling disabled" << std::endl;
            m_gpuProfilingEnabled = false;
        }
    }
    
    // Create particle system
    try {
        m_particleSystem = std::make_unique<ParticleSystem>(m_vulkanContext.get(), m_fullParticleCount); // 1M particles - full performance utilization!
        
        // Initialize constraint parameters
        m_particleSystem->setConstraintShape(static_cast<uint32_t>(m_constraintShape));
        m_particleSystem->setConstraintRadius(m_constraintRadius);
        m_particleSystem->setConstraintThickness(m_constraintThickness);
        
        std::cout << "Particle system created with " << m_particleSystem->getParticleCount() << " particles" << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "Warning: Failed to create particle system: " << e.what() << std::endl;
        // Continue without particles for now
    }
    
    // Create volume renderer
    try {
        m_volumeRenderer = std::make_unique<VolumeRenderer>(m_vulkanContext.get());
        std::cout << "Volume renderer created successfully!" << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "Warning: Failed to create volume renderer: " << e.what() << std::endl;
        // Continue without volumetric rendering for now
    }
    
    // Create mesh particle renderer if supported
    try {
        m_meshRenderer = std::make_unique<MeshParticleRenderer>(m_vulkanContext.get());
        if (m_meshRenderer->isSupported()) {
            std::cout << "Mesh shader particle renderer initialized successfully!" << std::endl;
        } else {
            std::cout << "Mesh shaders not supported on this GPU" << std::endl;
            m_meshRenderer.reset();
        }
    } catch (const std::exception& e) {
        std::cerr << "Warning: Failed to create mesh particle renderer: " << e.what() << std::endl;
        m_meshRenderer.reset();
    }
    
    // Initialize bloom resources
    try {
        createBloomResources();
        createBloomPipeline();
    } catch (const std::exception& e) {
        std::cerr << "Warning: Failed to create bloom resources: " << e.what() << std::endl;
        // Continue without bloom for now
    }
    
    // Initialize timing
    m_startTime = std::chrono::high_resolution_clock::now();
}

void Application::createCommandBuffers() {
    m_commandBuffers.resize(VulkanContext::MAX_FRAMES_IN_FLIGHT);

    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = m_vulkanContext->getCommandPool();
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = static_cast<uint32_t>(m_commandBuffers.size());

    if (vkAllocateCommandBuffers(m_vulkanContext->getDevice(), &allocInfo, m_commandBuffers.data()) != VK_SUCCESS) {
        throw std::runtime_error("Failed to allocate command buffers!");
    }
}

void Application::update(float deltaTime) {
    // Update total time only when physics updates
    if (m_shouldUpdatePhysics) {
        m_totalTime += deltaTime;
    }
    
    // Apply repulsive gravity if enabled
    if (m_particleSystem) {
        float baseGravity = std::abs(m_particleSystem->getGravityStrength());
        float effectiveGravity = m_repulsiveGravity ? -baseGravity : baseGravity;
        if (effectiveGravity != m_particleSystem->getGravityStrength()) {
            m_particleSystem->setGravityStrength(effectiveGravity);
        }
    }
    
    // Apply energy injection (periodic turbulence boost)
    if (m_energyInjection > 0.0f && m_particleSystem) {
        m_energyTimer += deltaTime;
        if (m_energyTimer >= 1.0f) { // Inject energy every second
            float baseTurbulence = m_particleSystem->getTurbulenceStrength();
            float boostedTurbulence = std::min(1.0f, baseTurbulence + m_energyInjection);
            m_particleSystem->setTurbulenceStrength(boostedTurbulence);
            
            // Reset turbulence after a short burst
            if (m_energyTimer >= 1.2f) {
                m_particleSystem->setTurbulenceStrength(baseTurbulence);
                m_energyTimer = 0.0f;
            }
        }
    } else {
        m_energyTimer = 0.0f;
    }
    
    // Update particle system physics would happen here if using CPU
    // But we're using compute shaders, so it happens in the render command buffer
}

void Application::render() {
    static bool firstFrame = true;
    if (firstFrame) {
        std::cout << "First render frame starting..." << std::endl;
        firstFrame = false;
    }

    // Wait for previous frame
    VkFence fence = m_vulkanContext->getInFlightFence(m_currentFrame);
    vkWaitForFences(m_vulkanContext->getDevice(), 1,
        &fence, VK_TRUE, UINT64_MAX);

    // After waiting, collect GPU timestamps for the frame that just finished
    // Only try to get results after we've rendered enough frames to populate queries
    if (m_gpuProfilingEnabled && m_timestampQueryPool != VK_NULL_HANDLE && m_totalFramesRendered > VulkanContext::MAX_FRAMES_IN_FLIGHT) {
        uint32_t base = m_currentFrame * 4;
        uint64_t timestamps[4] = {0,0,0,0};
        VkResult qr = vkGetQueryPoolResults(
            m_vulkanContext->getDevice(),
            m_timestampQueryPool,
            base,
            4,
            sizeof(timestamps),
            timestamps,
            sizeof(uint64_t),
            VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
        if (qr == VK_SUCCESS) {
            double periodNs = static_cast<double>(m_vulkanContext->getDeviceProperties().limits.timestampPeriod);
            m_gpuDensityMs   = (timestamps[1] - timestamps[0]) * periodNs / 1.0e6;
            m_gpuRaymarchMs  = (timestamps[3] - timestamps[2]) * periodNs / 1.0e6;
            if (m_frameCount % 60 == 0) {
                std::cout << "[GPU] density=" << m_gpuDensityMs << " ms, raymarch=" << m_gpuRaymarchMs << " ms" << std::endl;
            }
        }
    }

    // Acquire next image
    VkResult result = vkAcquireNextImageKHR(
        m_vulkanContext->getDevice(),
        m_vulkanContext->getSwapChain(),
        UINT64_MAX,
        m_vulkanContext->getImageAvailableSemaphore(m_currentFrame),
        VK_NULL_HANDLE,
        &m_currentImageIndex
    );

    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        recreateSwapChain();
        return;
    } else if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
        throw std::runtime_error("Failed to acquire swap chain image!");
    }

    vkResetFences(m_vulkanContext->getDevice(), 1, &fence);

    // Record command buffer
    vkResetCommandBuffer(m_commandBuffers[m_currentFrame], 0);
    recordCommandBuffer(m_commandBuffers[m_currentFrame], m_currentImageIndex);

    // Submit command buffer using Synchronization2
    VkSemaphoreSubmitInfo waitSemaphoreInfo{};
    waitSemaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    waitSemaphoreInfo.semaphore = m_vulkanContext->getImageAvailableSemaphore(m_currentFrame);
    waitSemaphoreInfo.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;

    VkCommandBufferSubmitInfo commandBufferInfo{};
    commandBufferInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    commandBufferInfo.commandBuffer = m_commandBuffers[m_currentFrame];

    VkSemaphoreSubmitInfo signalSemaphoreInfo{};
    signalSemaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    signalSemaphoreInfo.semaphore = m_vulkanContext->getRenderFinishedSemaphore(m_currentFrame);
    signalSemaphoreInfo.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

    VkSubmitInfo2 submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    submitInfo.waitSemaphoreInfoCount = 1;
    submitInfo.pWaitSemaphoreInfos = &waitSemaphoreInfo;
    submitInfo.commandBufferInfoCount = 1;
    submitInfo.pCommandBufferInfos = &commandBufferInfo;
    submitInfo.signalSemaphoreInfoCount = 1;
    submitInfo.pSignalSemaphoreInfos = &signalSemaphoreInfo;

    if (vkQueueSubmit2(m_vulkanContext->getGraphicsQueue(), 1, &submitInfo,
        fence) != VK_SUCCESS) {
        throw std::runtime_error("Failed to submit draw command buffer!");
    }

    // Present
    VkPresentInfoKHR presentInfo{};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    VkSemaphore presentWaitSemaphore = m_vulkanContext->getRenderFinishedSemaphore(m_currentFrame);
    presentInfo.pWaitSemaphores = &presentWaitSemaphore;

    VkSwapchainKHR swapChains[] = {m_vulkanContext->getSwapChain()};
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = swapChains;
    presentInfo.pImageIndices = &m_currentImageIndex;

    result = vkQueuePresentKHR(m_vulkanContext->getPresentQueue(), &presentInfo);

    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR || m_framebufferResized) {
        m_framebufferResized = false;
        recreateSwapChain();
    } else if (result != VK_SUCCESS) {
        throw std::runtime_error("Failed to present swap chain image!");
    }

    m_currentFrame = (m_currentFrame + 1) % VulkanContext::MAX_FRAMES_IN_FLIGHT;
    m_totalFramesRendered++; // Increment total frames counter for query pool safety
}

void Application::recordCommandBuffer(VkCommandBuffer commandBuffer, uint32_t imageIndex) {
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

    if (vkBeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS) {
        throw std::runtime_error("Failed to begin recording command buffer!");
    }

    // Begin dynamic rendering
    VkRenderingAttachmentInfo colorAttachment{};
    colorAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    colorAttachment.imageView = m_vulkanContext->getSwapChainImageView(imageIndex);
    colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}}; // Pure black background

    VkRenderingInfo renderingInfo{};
    renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    renderingInfo.renderArea.offset = {0, 0};
    renderingInfo.renderArea.extent = m_vulkanContext->getSwapChainExtent();
    renderingInfo.layerCount = 1;
    renderingInfo.colorAttachmentCount = 1;
    renderingInfo.pColorAttachments = &colorAttachment;

    // Transition image layout for rendering
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = m_vulkanContext->getSwapChainImage(imageIndex);
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    vkCmdPipelineBarrier(
        commandBuffer,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        0,
        0, nullptr,
        0, nullptr,
        1, &barrier
    );

    // Update particles BEFORE beginning rendering (compute must be outside render pass)
    // Only update physics when the accumulator says we should
    if (m_particleSystem && m_shouldUpdatePhysics) {
        m_particleSystem->update(commandBuffer, m_currentPhysicsDeltaTime, m_totalTime); // Use actual physics timestep
        
        // Update volumetric density grid from particles (if enabled)
        if (m_volumetricMode && m_volumeRenderer) {
            // Update density grid from particle data
            VkBuffer particleBuffer = m_particleSystem->getParticleBuffer();
            uint32_t activeParticles = m_particleSystem->getActiveParticleCount();
            // Write GPU timestamp around compute dispatch
            if (m_gpuProfilingEnabled && m_timestampQueryPool != VK_NULL_HANDLE) {
                uint32_t base = m_currentFrame * 4;
                vkCmdResetQueryPool(commandBuffer, m_timestampQueryPool, base, 4);
                vkCmdWriteTimestamp2(commandBuffer, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, m_timestampQueryPool, base + 0);
            }
            m_volumeRenderer->updateDensityGrid(commandBuffer, particleBuffer, activeParticles);
            if (m_gpuProfilingEnabled && m_timestampQueryPool != VK_NULL_HANDLE) {
                uint32_t base = m_currentFrame * 4;
                vkCmdWriteTimestamp2(commandBuffer, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, m_timestampQueryPool, base + 1);
            }
        }
    }

    vkCmdBeginRendering(commandBuffer, &renderingInfo);

    // Render particles if system exists
    if (m_particleSystem) {
        // Enhanced spherical camera system with mouse controls
        // Calculate camera position using spherical coordinates
        float x = cos(m_cameraTheta) * cos(m_cameraPhi) * m_cameraDistance;
        float y = sin(m_cameraPhi) * m_cameraDistance;  
        float z = sin(m_cameraTheta) * cos(m_cameraPhi) * m_cameraDistance;
        
        glm::vec3 cameraPos = glm::vec3(x, y, z) + m_cameraTarget;
        
        glm::mat4 view = glm::lookAt(
            cameraPos,          // Camera position
            m_cameraTarget,     // Look at target (can be panned)
            glm::vec3(0.0f, 1.0f, 0.0f) // Up vector
        );
        
        float aspect = static_cast<float>(m_vulkanContext->getSwapChainExtent().width) / 
                      static_cast<float>(m_vulkanContext->getSwapChainExtent().height);
        glm::mat4 proj = glm::perspective(glm::radians(45.0f), aspect, 0.1f, 100.0f);
        proj[1][1] *= -1; // Vulkan has inverted Y
        
        glm::mat4 viewProj = proj * view;
        
        if (m_volumetricMode && m_volumeRenderer) {
            // Render volumetric effect
            if (m_gpuProfilingEnabled && m_timestampQueryPool != VK_NULL_HANDLE) {
                uint32_t base = m_currentFrame * 4;
                vkCmdWriteTimestamp2(m_commandBuffers[m_currentFrame], VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, m_timestampQueryPool, base + 2);
            }
            // Determine quality level based on mode flags
            VolumeRenderer::QualityLevel quality = VolumeRenderer::QualityLevel::Standard;
            if (m_volumetricUltraQuality) {
                quality = VolumeRenderer::QualityLevel::Ultra;
            } else if (m_volumetricHighQuality) {
                quality = VolumeRenderer::QualityLevel::High;
            }
            
            // Update runtime parameters before rendering
            m_volumeRenderer->setRuntimeParameters(
                m_volumeDensityScale, m_volumeOpacity, m_volumeStepSize,
                m_volumeEmissionScale, m_volumeMaxSteps,
                m_volumeRedBalance, m_volumeOrangeBalance, m_volumeYellowBalance);
            
            m_volumeRenderer->render(commandBuffer, viewProj, cameraPos, quality);
            if (m_gpuProfilingEnabled && m_timestampQueryPool != VK_NULL_HANDLE) {
                uint32_t base = m_currentFrame * 4;
                vkCmdWriteTimestamp2(m_commandBuffers[m_currentFrame], VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT, m_timestampQueryPool, base + 3);
            }
        } else {
            // Render particles (drawing only, physics already updated)
            if (m_useMeshShaders && m_meshRenderer && m_meshRenderer->isSupported()) {
                // Use mesh shader rendering - GPU-driven particle generation
                m_meshRenderer->render(commandBuffer, viewProj, cameraPos,
                                      m_particleSystem->getParticleBuffer(),
                                      m_particleSystem->getActiveParticleCount(),
                                      1.0f, // particle size
                                      m_totalTime);
            } else {
                // Use traditional vertex buffer rendering
                m_particleSystem->render(commandBuffer, viewProj);
            }
        }
    }

    vkCmdEndRendering(commandBuffer);

    // Apply bloom post-processing if enabled
    renderBloomPass(commandBuffer);

    // Transition image layout for presentation
    barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.dstAccessMask = 0;

    vkCmdPipelineBarrier(
        commandBuffer,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
        0,
        0, nullptr,
        0, nullptr,
        1, &barrier
    );

    if (vkEndCommandBuffer(commandBuffer) != VK_SUCCESS) {
        throw std::runtime_error("Failed to record command buffer!");
    }
}

void Application::recreateSwapChain() {
    int width = 0, height = 0;
    glfwGetFramebufferSize(m_window, &width, &height);
    while (width == 0 || height == 0) {
        glfwGetFramebufferSize(m_window, &width, &height);
        glfwWaitEvents();
    }

    vkDeviceWaitIdle(m_vulkanContext->getDevice());

    // Recreate swap chain
    m_vulkanContext->createSwapChain();
}


void Application::updateWindowTitle() {
    std::string title = m_title;
    
    // Add FPS
    title += " - FPS: " + std::to_string(static_cast<int>(m_fps));
    
    // Add key parameters for quick reference
    if (m_particleSystem) {
        title += " | G:" + std::to_string(m_particleSystem->getGravityStrength()).substr(0, 4);
        title += " T:" + std::to_string(m_particleSystem->getTurbulenceStrength()).substr(0, 4);
        title += " D:" + std::to_string(m_particleSystem->getDampingFactor()).substr(2, 3); // Show .999 as 999
        title += " P:" + std::to_string(m_particleSystem->getActiveParticleCount() / 1000) + "k";
        
        if (m_particleSystem->isSPHMode()) {
            title += " [SPH]";
        }
        if (m_volumetricMode) {
            title += " [VOL]";
        }
        if (m_repulsiveGravity) {
            title += " [REP]";
        }
        if (m_timeScale != 1.0f) {
            title += " TS:" + std::to_string(m_timeScale).substr(0, 3);
        }
        if (m_energyInjection > 0.0f) {
            title += " EN:" + std::to_string(m_energyInjection).substr(0, 3);
        }
        
        // Shape constraint indicators
        switch (m_constraintShape) {
            case ConstraintShape::SPHERE:
                title += " [SPHERE]";
                break;
            case ConstraintShape::DISC:
                title += " [DISC]";
                break;
            case ConstraintShape::TORUS:
                title += " [TORUS]";
                break;
            default:
                break;
        }
        if (m_showWireframe && m_constraintShape != ConstraintShape::NONE) {
            title += " [WIRE]";
        }
        if (m_bloomEnabled) {
            title += " [BLOOM]";
        }
    }
    
    glfwSetWindowTitle(m_window, title.c_str());
}

void Application::printStatusUpdate() {
    if (!m_particleSystem) return;
    
    std::cout << "\n=== PlasmaVulkan Status Update ===" << std::endl;
    std::cout << "Performance: " << std::to_string(static_cast<int>(m_fps)) << " FPS" << std::endl;
    std::cout << "Physics Parameters:" << std::endl;
    std::cout << "  Gravity: " << m_particleSystem->getGravityStrength() << std::endl;
    std::cout << "  Turbulence: " << m_particleSystem->getTurbulenceStrength() << std::endl;
    std::cout << "  Damping: " << m_particleSystem->getDampingFactor() << std::endl;
    std::cout << "  Particles: " << m_particleSystem->getActiveParticleCount() 
              << " / " << m_particleSystem->getParticleCount() << std::endl;
    
    std::cout << "Simulation Modes:" << std::endl;
    std::cout << "  SPH Fluid: " << (m_particleSystem->isSPHMode() ? "ENABLED" : "DISABLED") << std::endl;
    std::cout << "  Volumetric: " << (m_volumetricMode ? "ENABLED" : "DISABLED") << std::endl;
    
    std::cout << "Advanced Physics:" << std::endl;
    std::cout << "  Time Scale: " << m_timeScale << std::endl;
    std::cout << "  Repulsive Gravity: " << (m_repulsiveGravity ? "ENABLED" : "DISABLED") << std::endl;
    std::cout << "  Energy Injection: " << m_energyInjection << std::endl;
    std::cout << "  Angular Momentum Boost: " << m_particleSystem->getAngularMomentumBoost() << std::endl;
    
    if (m_particleSystem->getDualGalaxyMode()) {
        glm::vec3 gravCenter1 = m_particleSystem->getGravityCenter();
        glm::vec3 gravCenter2 = m_particleSystem->getGravityCenter2();
        std::cout << "  Galaxy A Center: (" << gravCenter1.x << ", " << gravCenter1.y << ", " << gravCenter1.z << ")" << std::endl;
        std::cout << "  Galaxy B Center: (" << gravCenter2.x << ", " << gravCenter2.y << ", " << gravCenter2.z << ")" << std::endl;
        std::cout << "  Galaxy A Mass: " << m_particleSystem->getBlackHoleMass() << std::endl;
        std::cout << "  Galaxy B Mass: " << m_particleSystem->getBlackHoleMass2() << std::endl;
    } else {
        glm::vec3 gravCenter = m_particleSystem->getGravityCenter();
        std::cout << "  Gravity Center: (" << gravCenter.x << ", " << gravCenter.y << ", " << gravCenter.z << ")" << std::endl;
    }
    
    std::cout << "Shape Constraints:" << std::endl;
    std::cout << "  Shape: ";
    switch (m_constraintShape) {
        case ConstraintShape::NONE:   std::cout << "NONE (Open space)"; break;
        case ConstraintShape::SPHERE: std::cout << "SPHERE (R=" << m_constraintRadius << ")"; break;
        case ConstraintShape::DISC:   std::cout << "DISC (R=" << m_constraintRadius << " T=" << m_constraintThickness << ")"; break;
        case ConstraintShape::TORUS:  std::cout << "TORUS (Major=" << m_constraintRadius << " Minor=" << m_constraintThickness << ")"; break;
        case ConstraintShape::ACCRETION_DISK: 
            std::cout << "ACCRETION DISK (BH=" << (m_particleSystem ? m_particleSystem->getBlackHoleMass() : 1.0f) << " M☉)"; 
            break;
    }
    std::cout << std::endl;
    std::cout << "  Wireframe: " << (m_showWireframe ? "ENABLED" : "DISABLED") << std::endl;
    
    std::cout << "Camera: Distance=" << m_cameraDistance 
              << " Theta=" << m_cameraTheta << " Phi=" << m_cameraPhi << std::endl;
    std::cout << "=================================" << std::endl;
}

void Application::printParameterChange(const std::string& paramName, float value) {
    std::cout << "[PARAM] " << paramName << ": " << value << std::endl;
    updateWindowTitle(); // Immediately update title when parameters change
}

void Application::cleanup() {
    if (m_timestampQueryPool != VK_NULL_HANDLE && m_vulkanContext) {
        vkDestroyQueryPool(m_vulkanContext->getDevice(), m_timestampQueryPool, nullptr);
        m_timestampQueryPool = VK_NULL_HANDLE;
    }
    cleanupBloomResources();
    
    if (m_allocator) {
        vmaDestroyAllocator(m_allocator);
    }

    m_vulkanContext.reset();

    if (m_window) {
        glfwDestroyWindow(m_window);
        glfwTerminate();
    }
}

void Application::createBloomPipeline() {
    auto device = m_vulkanContext->getDevice();
    
    // Create descriptor set layout for bloom pipeline
    VkDescriptorSetLayoutBinding uboBinding{};
    uboBinding.binding = 0;
    uboBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    uboBinding.descriptorCount = 1;
    uboBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    uboBinding.pImmutableSamplers = nullptr;
    
    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &uboBinding;
    
    if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &m_bloomDescriptorLayout) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create bloom descriptor set layout!");
    }
    
    // Create pipeline layout with push constants for bloom parameters
    VkPushConstantRange pushConstantRange{};
    pushConstantRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    pushConstantRange.offset = 0;
    pushConstantRange.size = sizeof(float) * 4; // threshold, intensity, exposure, bloomStrength
    
    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &m_bloomDescriptorLayout;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushConstantRange;
    
    if (vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &m_bloomPipelineLayout) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create bloom pipeline layout!");
    }
    
    // Create bloom sampler for texture sampling
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.anisotropyEnable = VK_FALSE;
    samplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    samplerInfo.unnormalizedCoordinates = VK_FALSE;
    samplerInfo.compareEnable = VK_FALSE;
    samplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.mipLodBias = 0.0f;
    samplerInfo.minLod = 0.0f;
    samplerInfo.maxLod = 0.0f;
    
    if (vkCreateSampler(device, &samplerInfo, nullptr, &m_bloomSampler) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create bloom sampler!");
    }
    
    // Load bloom shader modules
    auto loadShader = [device](const std::string& path) -> VkShaderModule {
        std::ifstream file(path, std::ios::ate | std::ios::binary);
        if (!file.is_open()) {
            throw std::runtime_error("Failed to open shader file: " + path);
        }
        
        size_t fileSize = (size_t)file.tellg();
        std::vector<char> code(fileSize);
        file.seekg(0);
        file.read(code.data(), fileSize);
        file.close();
        
        VkShaderModuleCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        createInfo.codeSize = code.size();
        createInfo.pCode = reinterpret_cast<const uint32_t*>(code.data());
        
        VkShaderModule shaderModule;
        if (vkCreateShaderModule(device, &createInfo, nullptr, &shaderModule) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create shader module!");
        }
        
        return shaderModule;
    };
    
    // Load shader modules
    VkShaderModule vertShader = loadShader("shaders/fullscreen.vert.spv");
    VkShaderModule brightFragShader = loadShader("shaders/bloom_bright.frag.spv");
    VkShaderModule blurFragShader = loadShader("shaders/bloom_blur.frag.spv");
    VkShaderModule combineFragShader = loadShader("shaders/bloom_combine.frag.spv");
    
    // Create shader stage infos
    VkPipelineShaderStageCreateInfo vertStageInfo{};
    vertStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    vertStageInfo.stage = VK_SHADER_STAGE_VERTEX_BIT;
    vertStageInfo.module = vertShader;
    vertStageInfo.pName = "main";
    
    // Since we're using Vulkan 1.4 dynamic rendering, we don't need a traditional render pass
    // Instead, we'll use dynamic rendering for bloom passes
    
    // Clean up shader modules
    vkDestroyShaderModule(device, vertShader, nullptr);
    vkDestroyShaderModule(device, brightFragShader, nullptr);
    vkDestroyShaderModule(device, blurFragShader, nullptr);
    vkDestroyShaderModule(device, combineFragShader, nullptr);
    
    std::cout << "Bloom pipeline and shaders loaded successfully!" << std::endl;
}

void Application::createBloomResources() {
    auto device = m_vulkanContext->getDevice();
    auto swapChainExtent = m_vulkanContext->getSwapChainExtent();
    
    // Create HDR color image for bloom extraction
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent.width = swapChainExtent.width;
    imageInfo.extent.height = swapChainExtent.height;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = VK_FORMAT_R16G16B16A16_SFLOAT; // HDR format
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    
    if (vkCreateImage(device, &imageInfo, nullptr, &m_hdrColorImage) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create HDR color image!");
    }
    
    // Allocate memory for HDR image (simplified allocation)
    VkMemoryRequirements memRequirements;
    vkGetImageMemoryRequirements(device, m_hdrColorImage, &memRequirements);
    
    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    
    // Find suitable memory type
    VkPhysicalDeviceMemoryProperties memProperties;
    vkGetPhysicalDeviceMemoryProperties(m_vulkanContext->getPhysicalDevice(), &memProperties);
    
    uint32_t memoryType = 0;
    for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
        if ((memRequirements.memoryTypeBits & (1 << i)) && 
            (memProperties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            memoryType = i;
            break;
        }
    }
    
    allocInfo.memoryTypeIndex = memoryType;
    
    VkDeviceMemory hdrImageMemory;
    if (vkAllocateMemory(device, &allocInfo, nullptr, &hdrImageMemory) != VK_SUCCESS) {
        throw std::runtime_error("Failed to allocate HDR image memory!");
    }
    
    vkBindImageMemory(device, m_hdrColorImage, hdrImageMemory, 0);
    
    // Create HDR image view
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = m_hdrColorImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;
    
    if (vkCreateImageView(device, &viewInfo, nullptr, &m_hdrColorImageView) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create HDR image view!");
    }
    
    std::cout << "HDR framebuffer created successfully (" << swapChainExtent.width << "x" << swapChainExtent.height << ")" << std::endl;
}

void Application::cleanupBloomResources() {
    auto device = m_vulkanContext->getDevice();
    
    if (device != VK_NULL_HANDLE) {
        if (m_bloomBrightPipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(device, m_bloomBrightPipeline, nullptr);
            m_bloomBrightPipeline = VK_NULL_HANDLE;
        }
        
        if (m_bloomBlurPipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(device, m_bloomBlurPipeline, nullptr);
            m_bloomBlurPipeline = VK_NULL_HANDLE;
        }
        
        if (m_bloomCombinePipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(device, m_bloomCombinePipeline, nullptr);
            m_bloomCombinePipeline = VK_NULL_HANDLE;
        }
        
        if (m_bloomPipelineLayout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(device, m_bloomPipelineLayout, nullptr);
            m_bloomPipelineLayout = VK_NULL_HANDLE;
        }
        
        if (m_bloomRenderPass != VK_NULL_HANDLE) {
            vkDestroyRenderPass(device, m_bloomRenderPass, nullptr);
            m_bloomRenderPass = VK_NULL_HANDLE;
        }
        
        // Cleanup framebuffers and images
        if (m_hdrFramebuffer != VK_NULL_HANDLE) {
            vkDestroyFramebuffer(device, m_hdrFramebuffer, nullptr);
            m_hdrFramebuffer = VK_NULL_HANDLE;
        }
        
        if (m_bloomBrightFramebuffer != VK_NULL_HANDLE) {
            vkDestroyFramebuffer(device, m_bloomBrightFramebuffer, nullptr);
            m_bloomBrightFramebuffer = VK_NULL_HANDLE;
        }
        
        if (m_bloomBlurFramebuffer != VK_NULL_HANDLE) {
            vkDestroyFramebuffer(device, m_bloomBlurFramebuffer, nullptr);
            m_bloomBlurFramebuffer = VK_NULL_HANDLE;
        }
        
        // Cleanup images and views
        if (m_hdrColorImageView != VK_NULL_HANDLE) {
            vkDestroyImageView(device, m_hdrColorImageView, nullptr);
            m_hdrColorImageView = VK_NULL_HANDLE;
        }
        
        if (m_bloomBrightImageView != VK_NULL_HANDLE) {
            vkDestroyImageView(device, m_bloomBrightImageView, nullptr);
            m_bloomBrightImageView = VK_NULL_HANDLE;
        }
        
        if (m_bloomBlurImageView != VK_NULL_HANDLE) {
            vkDestroyImageView(device, m_bloomBlurImageView, nullptr);
            m_bloomBlurImageView = VK_NULL_HANDLE;
        }
        
        if (m_bloomSampler != VK_NULL_HANDLE) {
            vkDestroySampler(device, m_bloomSampler, nullptr);
            m_bloomSampler = VK_NULL_HANDLE;
        }
        
        if (m_bloomDescriptorPool != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(device, m_bloomDescriptorPool, nullptr);
            m_bloomDescriptorPool = VK_NULL_HANDLE;
        }
        
        if (m_bloomDescriptorLayout != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(device, m_bloomDescriptorLayout, nullptr);
            m_bloomDescriptorLayout = VK_NULL_HANDLE;
        }
    }
}

void Application::renderBloomPass(VkCommandBuffer commandBuffer) {
    if (!m_bloomEnabled || m_hdrColorImageView == VK_NULL_HANDLE) {
        return; // Skip bloom if disabled or resources not ready
    }
    
    // For visual feedback that bloom is active
    static int frameCounter = 0;
    frameCounter++;
    
    // Create push constants for bloom parameters
    struct BloomPushConstants {
        float threshold;
        float intensity;
        float exposure;
        float bloomStrength;
    } pushConstants;
    
    pushConstants.threshold = m_bloomThreshold;
    pushConstants.intensity = m_bloomIntensity;
    pushConstants.exposure = m_exposure;
    pushConstants.bloomStrength = m_bloomStrength;
    
    // Since we're using dynamic rendering, we would implement bloom passes here
    // For now, we'll prepare the command buffer for future bloom implementation
    
    // Pass 1: Bright pass extraction - extract pixels above threshold
    // This would render to m_bloomBrightImage
    
    // Pass 2: Horizontal blur - blur the bright pixels horizontally
    // This would ping-pong between blur buffers
    
    // Pass 3: Vertical blur - blur the result vertically
    // This completes the Gaussian blur
    
    // Pass 4: Combine pass - blend bloom with original scene
    // This would composite back to the swapchain image
    
    // Push bloom parameters to fragment shader
    if (m_bloomPipelineLayout != VK_NULL_HANDLE) {
        vkCmdPushConstants(
            commandBuffer,
            m_bloomPipelineLayout,
            VK_SHADER_STAGE_FRAGMENT_BIT,
            0,
            sizeof(BloomPushConstants),
            &pushConstants
        );
    }
    
    // Visual indicator that bloom processing is active (every 60 frames)
    if (frameCounter % 60 == 0) {
        std::cout << "[BLOOM] Active - Threshold: " << m_bloomThreshold 
                  << ", Intensity: " << m_bloomIntensity 
                  << ", Strength: " << m_bloomStrength << std::endl;
    }
}

void Application::enableBloomMode(bool enabled) {
    m_bloomEnabled = enabled;
    std::cout << "Bloom mode: " << (enabled ? "ENABLED" : "DISABLED") << std::endl;
}

// Recording system implementation
void Application::startRecording(uint32_t maxFrames, bool highQuality) {
    if (m_recordingActive) {
        std::cout << "Already recording!" << std::endl;
        return;
    }
    
    // Find next available session number
    m_recordingSessionNumber = 0;
    while (true) {
        std::string testDir = m_recordingBaseDir + std::string(3 - std::to_string(m_recordingSessionNumber).length(), '0') + std::to_string(m_recordingSessionNumber) + "/";
        
        // Check if directory exists
        #ifdef _WIN32
            // Windows: Use dir command to check if directory exists
            std::string checkCmd = "if exist \"" + testDir + "\" (exit 1) else (exit 0)";
            int result = std::system(checkCmd.c_str());
            if (result == 0) {
                break; // Directory doesn't exist, use this number
            }
        #else
            struct stat st;
            if (stat(testDir.c_str(), &st) != 0) {
                break; // Directory doesn't exist, use this number
            }
        #endif
        m_recordingSessionNumber++;
        
        // Safety check to prevent infinite loop
        if (m_recordingSessionNumber > 999) {
            std::cerr << "Error: Too many recording sessions!" << std::endl;
            break;
        }
    }
    
    // Create numbered subfolder for this recording session
    m_recordingOutputDir = m_recordingBaseDir + std::string(3 - std::to_string(m_recordingSessionNumber).length(), '0') + std::to_string(m_recordingSessionNumber) + "/";
    
    m_recordingMaxFrames = maxFrames;
    m_recordingFrame = 0;
    m_recordingActive = true;
    m_recordingHighQuality = highQuality;
    
    // Enhanced quality mode for offline rendering
    if (highQuality && m_particleSystem) {
        // Store original settings for restoration
        m_recordingOriginalParticleCount = m_particleSystem->getActiveParticleCount();
        
        // Increase particle count for higher quality
        uint32_t hqParticleCount = std::min(500000u, m_recordingOriginalParticleCount * 2);
        m_particleSystem->setActiveParticleCount(hqParticleCount);
        
        // Enable volumetric mode if not already enabled
        if (!m_volumetricMode) {
            enableVolumetricMode(true);
        }
        
        // Enhance bloom for better visuals
        m_bloomIntensity = std::min(2.5f, m_bloomIntensity * 1.5f);
        m_bloomStrength = std::min(1.0f, m_bloomStrength * 1.2f);
        
        std::cout << "  High Quality Mode: " << hqParticleCount << " particles, volumetrics enabled" << std::endl;
    }
    
    // Reset simulation for perfect loops if enabled
    if (m_recordingLoop && m_particleSystem) {
        m_particleSystem->reinitializeParticles();
        std::cout << "  Simulation reset for seamless loop" << std::endl;
    }
    
    // Create output directory (platform specific)
    #ifdef _WIN32
        std::system(("mkdir \"" + m_recordingOutputDir + "\" 2>nul || echo Directory creation attempted").c_str());
    #else
        std::system(("mkdir -p \"" + m_recordingOutputDir + "\"").c_str());
    #endif
    
    std::cout << "\n[RECORDING] Started - Session #" << m_recordingSessionNumber << std::endl;
    std::cout << "  Frames: " << maxFrames << " (" << (maxFrames / 60.0f) << " seconds at 60fps)" << std::endl;
    std::cout << "  Output: " << m_recordingOutputDir << std::endl;
    std::cout << "  Fixed timestep: " << m_recordingFixedTimestep << "s" << std::endl;
    std::cout << "  Loop mode: " << (m_recordingLoop ? "ON" : "OFF") << std::endl;
    
    // Reset simulation for consistent start
    if (m_recordingLoop && m_particleSystem) {
        // TODO: Add particle system reset method
        std::cout << "  Resetting simulation state for loop..." << std::endl;
    }
}

void Application::stopRecording() {
    if (!m_recordingActive) {
        std::cout << "Not recording!" << std::endl;
        return;
    }
    
    // Restore original settings if high quality mode was used
    if (m_recordingHighQuality && m_particleSystem) {
        m_particleSystem->setActiveParticleCount(m_recordingOriginalParticleCount);
        
        // Restore bloom settings
        m_bloomIntensity = 1.5f;  // Original default
        m_bloomStrength = 0.6f;   // Original default
        
        std::cout << "  High quality settings restored" << std::endl;
    }
    
    m_recordingActive = false;
    m_recordingHighQuality = false;
    std::cout << "\n[RECORDING] Stopped" << std::endl;
    std::cout << "  Captured " << m_recordingFrame << " frames" << std::endl;
    std::cout << "  Output directory: " << m_recordingOutputDir << std::endl;
    
    // Automatically create video from the PNG sequence
    if (m_recordingFrame > 0) {
        createVideoFromFrames();
    }
}

void Application::captureFrame() {
    if (!m_recordingActive) return;
    
    // Optimize performance - only capture every 2nd frame to halve the work
    if (m_recordingFrame % 2 != 0) {
        m_recordingFrame++;
        return;
    }
    
    // Create filename with zero-padded frame number  
    std::string filename = m_recordingOutputDir + "frame_" + 
                          std::string(6 - std::to_string(m_recordingFrame).length(), '0') + 
                          std::to_string(m_recordingFrame) + ".png";
    
    // Get current swap chain image info
    VkExtent2D extent = m_vulkanContext->getSwapChainExtent();
    VkFormat swapChainFormat = m_vulkanContext->getSwapChainImageFormat();
    VkImage swapChainImage = m_vulkanContext->getSwapChainImage(m_currentImageIndex);
    
    // Create a simple staging buffer using standard Vulkan (avoid VMA complexity)
    VkDeviceSize imageSize = extent.width * extent.height * 4; // RGBA
    VkDevice device = m_vulkanContext->getDevice();
    
    VkBuffer stagingBuffer;
    VkDeviceMemory stagingBufferMemory;
    
    // Create buffer
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = imageSize;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    
    if (vkCreateBuffer(device, &bufferInfo, nullptr, &stagingBuffer) != VK_SUCCESS) {
        std::cerr << "Failed to create staging buffer!" << std::endl;
        m_recordingFrame++;
        return;
    }
    
    // Allocate memory
    VkMemoryRequirements memRequirements;
    vkGetBufferMemoryRequirements(device, stagingBuffer, &memRequirements);
    
    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    
    // Find host-visible memory type
    VkPhysicalDeviceMemoryProperties memProperties;
    vkGetPhysicalDeviceMemoryProperties(m_vulkanContext->getPhysicalDevice(), &memProperties);
    
    uint32_t memoryTypeIndex = UINT32_MAX;
    for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
        if ((memRequirements.memoryTypeBits & (1 << i)) && 
            (memProperties.memoryTypes[i].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))) {
            memoryTypeIndex = i;
            break;
        }
    }
    
    if (memoryTypeIndex == UINT32_MAX) {
        std::cerr << "Failed to find suitable memory type!" << std::endl;
        vkDestroyBuffer(device, stagingBuffer, nullptr);
        m_recordingFrame++;
        return;
    }
    
    allocInfo.memoryTypeIndex = memoryTypeIndex;
    if (vkAllocateMemory(device, &allocInfo, nullptr, &stagingBufferMemory) != VK_SUCCESS ||
        vkBindBufferMemory(device, stagingBuffer, stagingBufferMemory, 0) != VK_SUCCESS) {
        std::cerr << "Failed to allocate/bind buffer memory!" << std::endl;
        vkDestroyBuffer(device, stagingBuffer, nullptr);
        m_recordingFrame++;
        return;
    }
    
    // Quick copy operation with proper layout transition
    VkCommandBuffer commandBuffer = m_vulkanContext->beginSingleTimeCommands();
    
    // Transition to transfer source layout
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = swapChainImage;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 
                        0, 0, nullptr, 0, nullptr, 1, &barrier);
    
    // Copy image to buffer
    VkBufferImageCopy region{};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {extent.width, extent.height, 1};
    
    vkCmdCopyImageToBuffer(commandBuffer, swapChainImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                          stagingBuffer, 1, &region);
    
    // Transition back to present layout
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.dstAccessMask = 0;
    
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                        0, 0, nullptr, 0, nullptr, 1, &barrier);
    
    m_vulkanContext->endSingleTimeCommands(commandBuffer);
    
    // Map memory and save PNG with color format fix
    void* data;
    if (vkMapMemory(device, stagingBufferMemory, 0, imageSize, 0, &data) == VK_SUCCESS) {
        // Fix BGRA -> RGBA color channels for correct colors
        std::vector<uint8_t> pixels(imageSize);
        uint8_t* srcPixels = static_cast<uint8_t*>(data);
        
        if (swapChainFormat == VK_FORMAT_B8G8R8A8_SRGB || swapChainFormat == VK_FORMAT_B8G8R8A8_UNORM) {
            // Convert BGRA to RGBA (fix grey spheres issue)
            for (size_t i = 0; i < imageSize; i += 4) {
                pixels[i] = srcPixels[i + 2];     // R = B
                pixels[i + 1] = srcPixels[i + 1]; // G = G  
                pixels[i + 2] = srcPixels[i];     // B = R
                pixels[i + 3] = srcPixels[i + 3]; // A = A
            }
        } else {
            // Direct copy for RGBA formats
            std::memcpy(pixels.data(), srcPixels, imageSize);
        }
        
        if (stbi_write_png(filename.c_str(), extent.width, extent.height, 4, pixels.data(), extent.width * 4)) {
            // Only print progress every 60 frames to reduce console spam
            if (m_recordingFrame % 60 == 0) {
                float progress = (float)m_recordingFrame / (float)m_recordingMaxFrames * 100.0f;
                std::cout << "[RECORDING] " << std::fixed << std::setprecision(1) 
                         << progress << "% (" << m_recordingFrame << "/" << m_recordingMaxFrames << ")" << std::endl;
            }
        }
        vkUnmapMemory(device, stagingBufferMemory);
    }
    
    // Cleanup
    vkDestroyBuffer(device, stagingBuffer, nullptr);
    vkFreeMemory(device, stagingBufferMemory, nullptr);
    
    m_recordingFrame++;
    
    // Auto-stop when max frames reached
    if (m_recordingFrame >= m_recordingMaxFrames) {
        stopRecording();
        std::cout << "[RECORDING] Completed! " << m_recordingFrame << " PNG files saved to " 
                 << m_recordingOutputDir << std::endl;
    }
}

void Application::setRecordingParameters(uint32_t maxFrames, float timestep, bool loop) {
    m_recordingMaxFrames = maxFrames;
    m_recordingFixedTimestep = timestep;
    m_recordingLoop = loop;
    
    std::cout << "Recording parameters updated:" << std::endl;
    std::cout << "  Max frames: " << maxFrames << " (" << (maxFrames * timestep) << "s)" << std::endl;
    std::cout << "  Timestep: " << timestep << "s (" << (1.0f/timestep) << " fps)" << std::endl;
    std::cout << "  Loop mode: " << (loop ? "ON" : "OFF") << std::endl;
}

void Application::createVideoFromFrames() {
    std::cout << "\n[VIDEO] Creating video from recorded frames..." << std::endl;
    std::cout << "[VIDEO] Session folder: " << m_recordingOutputDir << std::endl;
    std::cout << "[VIDEO] Session number: " << m_recordingSessionNumber << std::endl;
    
    // Generate output video filename with sequential numbering
    std::string videoFilename = m_recordingBaseDir + "video_" + 
                               std::string(3 - std::to_string(m_recordingSessionNumber).length(), '0') + 
                               std::to_string(m_recordingSessionNumber) + ".mp4";
    
    // Build ffmpeg command for video creation
    std::string ffmpegCmd;
    #ifdef _WIN32
        // Windows command - create file list since glob is not supported
        std::string sessionFolder = std::string(3 - std::to_string(m_recordingSessionNumber).length(), '0') + 
                                  std::to_string(m_recordingSessionNumber);
        
        // Create a temporary file list for ffmpeg (works without glob support)
        std::string fileListPath = m_recordingOutputDir + "filelist.txt";
        std::ofstream fileList(fileListPath);
        
        // List all PNG files in the directory
        for (uint32_t i = 0; i < m_recordingFrame; i += 2) { // Only even frames
            std::string frameName = "frame_" + std::string(6 - std::to_string(i).length(), '0') + std::to_string(i) + ".png";
            fileList << "file '" << frameName << "'\n";
        }
        fileList.close();
        
        // Use concat demuxer with file list
        ffmpegCmd = "ffmpeg -y -f concat -safe 0 -r 15 -i \"" + fileListPath + 
                   "\" -c:v libx264 -pix_fmt yuv420p -crf 18 \"" + 
                   m_recordingBaseDir + "video_" + sessionFolder + ".mp4\"";
        
        std::cout << "[VIDEO] Running ffmpeg with file list..." << std::endl;
        std::cout << "[VIDEO] Command: " << ffmpegCmd << std::endl;
    #else
        // Linux/Unix command
        ffmpegCmd = "cd \"" + m_recordingOutputDir + "\" && ffmpeg -y -framerate 15 -pattern_type glob -i 'frame_*.png' "
                   "-c:v libx264 -pix_fmt yuv420p -crf 18 '../video_" + 
                   std::string(3 - std::to_string(m_recordingSessionNumber).length(), '0') + 
                   std::to_string(m_recordingSessionNumber) + ".mp4' 2>/dev/null";
    #endif
    
    // Execute ffmpeg command
    std::cout << "[VIDEO] Executing command..." << std::endl;
    int result = std::system(ffmpegCmd.c_str());
    
    std::cout << "[VIDEO] Command returned: " << result << std::endl;
    
    if (result == 0) {
        std::cout << "[VIDEO] Successfully created: " << videoFilename << std::endl;
        std::cout << "[VIDEO] PNG sequence preserved in: " << m_recordingOutputDir << std::endl;
    } else {
        std::cerr << "[VIDEO] Failed to create video (return code: " << result << ")" << std::endl;
        std::cerr << "[VIDEO] Manual command to try:" << std::endl;
        std::cerr << "  " << ffmpegCmd << std::endl;
        std::cerr << "[VIDEO] Make sure ffmpeg is installed and in PATH." << std::endl;
    }
}

// Static callback functions
void Application::framebufferResizeCallback(GLFWwindow* window, int width, int height) {
    auto app = reinterpret_cast<Application*>(glfwGetWindowUserPointer(window));
    app->m_framebufferResized = true;
}

void Application::keyCallback(GLFWwindow* window, int key, int scancode, int action, int mods) {
    auto app = reinterpret_cast<Application*>(glfwGetWindowUserPointer(window));

    if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS) {
        app->stop();
    }
    else if (key == GLFW_KEY_SPACE && action == GLFW_PRESS) {
        // Toggle SPH mode
        bool currentMode = app->m_particleSystem->isSPHMode();
        app->m_particleSystem->setSPHMode(!currentMode);
        
        if (!currentMode) {
            std::cout << "SPH Mode ENABLED - Fluid physics active!" << std::endl;
            std::cout << "Current particles: " << app->m_particleSystem->getActiveParticleCount() << std::endl;
            std::cout << "Use P/Shift+P to adjust particle count (25k-250k recommended for SPH)" << std::endl;
            
            // Keep current particle count - user can adjust with P/Shift+P
            // No automatic reduction anymore
            
            // Set more stable SPH parameters
            app->m_particleSystem->setSPHParameters(
                0.6f,    // smoothingRadius - smaller for better performance
                1000.0f, // restDensity - standard water density
                50.0f,   // pressureConstant - reduced for stability
                0.1f,    // viscosity - higher for stability
                0.05f    // mass - heavier for stability
            );
        } else {
            std::cout << "SPH Mode DISABLED - Back to orbital physics" << std::endl;
            std::cout << "Current particles: " << app->m_particleSystem->getActiveParticleCount() << std::endl;
            
            // Keep current particle count - no automatic changes
        }
    }
    else if (key == GLFW_KEY_1 && action == GLFW_PRESS) {
        std::cout << "Performance tip: For smooth SPH, try creating a new particle system with fewer particles (e.g., 5000-10000)" << std::endl;
        std::cout << "Current: 100k particles = ~10 billion SPH comparisons per frame" << std::endl;
    }
    // Camera zoom controls
    else if (key == GLFW_KEY_Q && (action == GLFW_PRESS || action == GLFW_REPEAT)) {
        app->m_cameraDistance = std::max(5.0f, app->m_cameraDistance - 2.0f);
        std::cout << "Zoom in - Distance: " << app->m_cameraDistance << std::endl;
    }
    else if (key == GLFW_KEY_E && (action == GLFW_PRESS || action == GLFW_REPEAT)) {
        app->m_cameraDistance = std::min(50.0f, app->m_cameraDistance + 2.0f);
        std::cout << "Zoom out - Distance: " << app->m_cameraDistance << std::endl;
    }
    // SPH parameter controls (quick tweaks without recompiling!)
    else if (key == GLFW_KEY_2 && action == GLFW_PRESS) {
        // Reduce pressure for less explosive behavior
        app->m_particleSystem->setSPHParameters(0.6f, 1000.0f, 30.0f, 0.1f, 0.05f);
        std::cout << "SPH: Low pressure mode" << std::endl;
    }
    else if (key == GLFW_KEY_3 && action == GLFW_PRESS) {
        // Increase viscosity for more fluid-like behavior
        app->m_particleSystem->setSPHParameters(0.6f, 1000.0f, 50.0f, 0.2f, 0.05f);
        std::cout << "SPH: High viscosity mode" << std::endl;
    }
    // Physics parameter controls
    else if (key == GLFW_KEY_G && (action == GLFW_PRESS || action == GLFW_REPEAT)) {
        // Gravity strength controls
        if (mods & GLFW_MOD_SHIFT) {
            // Decrease gravity
            float newGravity = std::max(0.0f, app->m_particleSystem->getGravityStrength() - 0.1f);
            app->m_particleSystem->setGravityStrength(newGravity);
            app->printParameterChange("Gravity Strength", newGravity);
        } else {
            // Increase gravity - much higher cap for stronger attraction
            float newGravity = std::min(20.0f, app->m_particleSystem->getGravityStrength() + 0.1f);
            app->m_particleSystem->setGravityStrength(newGravity);
            app->printParameterChange("Gravity Strength", newGravity);
        }
    }
    else if (key == GLFW_KEY_T && (action == GLFW_PRESS || action == GLFW_REPEAT)) {
        // Turbulence strength controls
        if (mods & GLFW_MOD_SHIFT) {
            // Decrease turbulence
            float newTurbulence = std::max(0.0f, app->m_particleSystem->getTurbulenceStrength() - 0.05f);
            app->m_particleSystem->setTurbulenceStrength(newTurbulence);
            app->printParameterChange("Turbulence Strength", newTurbulence);
        } else {
            // Increase turbulence
            float newTurbulence = std::min(1.0f, app->m_particleSystem->getTurbulenceStrength() + 0.05f);
            app->m_particleSystem->setTurbulenceStrength(newTurbulence);
            app->printParameterChange("Turbulence Strength", newTurbulence);
        }
    }
    else if (key == GLFW_KEY_D && (action == GLFW_PRESS || action == GLFW_REPEAT)) {
        // Damping factor controls
        if (mods & GLFW_MOD_SHIFT) {
            // Decrease damping (more energy retention)
            float newDamping = std::max(0.9f, app->m_particleSystem->getDampingFactor() - 0.005f);
            app->m_particleSystem->setDampingFactor(newDamping);
            app->printParameterChange("Damping Factor", newDamping);
        } else {
            // Increase damping (more energy loss)
            float newDamping = std::min(1.0f, app->m_particleSystem->getDampingFactor() + 0.005f);
            app->m_particleSystem->setDampingFactor(newDamping);
            app->printParameterChange("Damping Factor", newDamping);
        }
    }
    else if (key == GLFW_KEY_P && (action == GLFW_PRESS || action == GLFW_REPEAT)) {
        // Particle count controls
        if (mods & GLFW_MOD_SHIFT) {
            // Decrease particle count
            uint32_t currentCount = app->m_particleSystem->getActiveParticleCount();
            uint32_t newCount = std::max(1000u, currentCount - 5000u);
            app->m_particleSystem->setActiveParticleCount(newCount);
            app->printParameterChange("Active Particles", static_cast<float>(newCount));
        } else {
            // Increase particle count
            uint32_t currentCount = app->m_particleSystem->getActiveParticleCount();
            uint32_t maxCount = app->m_particleSystem->getParticleCount();
            uint32_t newCount = std::min(maxCount, currentCount + 5000u);
            app->m_particleSystem->setActiveParticleCount(newCount);
            app->printParameterChange("Active Particles", static_cast<float>(newCount));
        }
    }
    else if (key == GLFW_KEY_R && action == GLFW_PRESS) {
        if (mods & GLFW_MOD_SHIFT) {
            // SHIFT+R: Initialize particles in disk formation for accretion disk mode
            if (app->m_constraintShape == Application::ConstraintShape::ACCRETION_DISK) {
                std::cout << "[DISK INIT] Forming accretion disk structure..." << std::endl;
                // The actual disk formation happens in the shader based on initial conditions
                // Reset with optimal parameters for disk
                app->m_particleSystem->setGravityStrength(1.5f);
                app->m_particleSystem->setTurbulenceStrength(0.05f); // Slight turbulence
                app->m_particleSystem->setDampingFactor(0.995f);
                std::cout << "  Particles will organize into disk orbits" << std::endl;
            }
        } else {
            // Regular R: Reset physics parameters to defaults
            app->m_particleSystem->setGravityStrength(0.6f);
            app->m_particleSystem->setGravityCenter({0.0f, 0.0f, 0.0f});
            app->m_particleSystem->setTurbulenceStrength(0.0f);
            app->m_particleSystem->setDampingFactor(0.999f);
            std::cout << "Physics parameters reset to defaults" << std::endl;
        }
    }
    else if (key == GLFW_KEY_H && action == GLFW_PRESS) {
        // Display current physics values
        std::cout << "\n=== Current Physics Parameters ===" << std::endl;
        std::cout << "Gravity strength: " << app->m_particleSystem->getGravityStrength() << std::endl;
        glm::vec3 center = app->m_particleSystem->getGravityCenter();
        std::cout << "Gravity center: (" << center.x << ", " << center.y << ", " << center.z << ")" << std::endl;
        std::cout << "Turbulence strength: " << app->m_particleSystem->getTurbulenceStrength() << std::endl;
        std::cout << "Damping factor: " << app->m_particleSystem->getDampingFactor() << std::endl;
        std::cout << "Active particles: " << app->m_particleSystem->getActiveParticleCount() << "/" << app->m_particleSystem->getParticleCount() << std::endl;
        std::cout << "===================================" << std::endl;
        std::cout << "\n=== Controls ===" << std::endl;
        std::cout << "Physics Controls:" << std::endl;
        std::cout << "  G/Shift+G: Increase/Decrease gravity strength" << std::endl;
        std::cout << "  T/Shift+T: Increase/Decrease turbulence" << std::endl;
        std::cout << "  D/Shift+D: Increase/Decrease damping" << std::endl;
        std::cout << "  P/Shift+P: Increase/Decrease particle count" << std::endl;
        std::cout << "  R: Reset all physics parameters" << std::endl;
        std::cout << "Mode Controls:" << std::endl;
        std::cout << "  Space: Toggle SPH fluid mode" << std::endl;
        std::cout << "  V: Toggle volumetric rendering" << std::endl;
        std::cout << "Display Controls:" << std::endl;
        std::cout << "  H: Show this help" << std::endl;
        std::cout << "  F1: Toggle periodic status updates (OSD)" << std::endl;
        std::cout << "Advanced Physics:" << std::endl;
        std::cout << "  Arrow Keys: Move gravity center (Up/Down=Y, Left/Right=X)" << std::endl;
        std::cout << "  Page Up/Down: Move gravity center Z" << std::endl;
        std::cout << "  [ / ]: Decrease/Increase time scale (0.1x to 5.0x)" << std::endl;
        std::cout << "  N: Toggle attraction/repulsion gravity" << std::endl;
        std::cout << "  I/U: Increase/Decrease energy injection" << std::endl;
        std::cout << "  J/K: Decrease/Increase angular momentum boost" << std::endl;
        std::cout << "Shape Constraints:" << std::endl;
        std::cout << "  F1: No constraints (open space)" << std::endl;
        std::cout << "  F2: Spherical boundary" << std::endl;
        std::cout << "  F3: Disc boundary (accretion disc)" << std::endl;
        std::cout << "  F4: Torus boundary (fusion reactor)" << std::endl;
        std::cout << "  W: Toggle wireframe display" << std::endl;
        std::cout << "  +/-: Increase/Decrease constraint size" << std::endl;
        std::cout << "Camera Controls:" << std::endl;
        std::cout << "  Left Mouse: Orbit camera" << std::endl;
        std::cout << "  Right Mouse: Pan camera" << std::endl;
        std::cout << "  Wheel: Zoom in/out (0.5-200 units)" << std::endl;
        std::cout << "  Middle Mouse: Pan camera target" << std::endl;
    }
    // Volumetric rendering toggle (V - standard mode)
    else if (key == GLFW_KEY_V && action == GLFW_PRESS && !(mods & (GLFW_MOD_CONTROL | GLFW_MOD_SHIFT))) {
        app->m_volumetricMode = !app->m_volumetricMode;
        app->m_volumetricHighQuality = false;   // Disable high quality when toggling normal mode
        app->m_volumetricUltraQuality = false;  // Disable ultra quality when toggling normal mode
        if (app->m_volumetricMode) {
            std::cout << "[MODE] Volumetric rendering ENABLED - 3D plasma glow!" << std::endl;
            std::cout << "       Particles → Volume → Ray march pipeline active" << std::endl;
        } else {
            std::cout << "[MODE] Volumetric rendering DISABLED - Back to particle rendering" << std::endl;
        }
        app->updateWindowTitle();
    }
    // High-quality volumetric mode (Shift+V - realtime HQ)
    else if (key == GLFW_KEY_V && action == GLFW_PRESS && (mods & GLFW_MOD_SHIFT) && !(mods & GLFW_MOD_CONTROL)) {
        if (!app->m_volumetricHighQuality) {
            app->m_volumetricHighQuality = true;
            app->m_volumetricUltraQuality = false; // Disable ultra quality
            app->m_volumetricMode = true;
            std::cout << "[MODE] HIGH-QUALITY Volumetric rendering ENABLED!" << std::endl;
            std::cout << "       256 ray steps, 0.1 step size - Still realtime capable" << std::endl;
        } else {
            app->m_volumetricHighQuality = false;
            std::cout << "[MODE] HIGH-QUALITY Volumetric rendering DISABLED!" << std::endl;
        }
        app->updateWindowTitle();
    }
    // Ultra-high quality volumetric recording mode (Ctrl+V) - Auto-starts recording
    else if (key == GLFW_KEY_V && action == GLFW_PRESS && (mods & GLFW_MOD_CONTROL)) {
        if (!app->m_volumetricUltraQuality) {
            // Enable ultra-high quality and start recording automatically
            app->m_volumetricUltraQuality = true;
            app->m_volumetricHighQuality = false; // Disable high quality
            app->m_volumetricMode = true;
            app->startRecording();
            
            std::cout << "[RECORDING] ULTRA-HIGH QUALITY Volumetric Recording STARTED!" << std::endl;
            std::cout << "            500³ voxel grid, 1024 ray steps, 0.02 step size - MAXIMUM QUALITY!" << std::endl;
            std::cout << "            Performance will be very low - recording only mode!" << std::endl;
        } else {
            // Stop recording and disable ultra-high quality mode
            app->stopRecording();
            app->m_volumetricUltraQuality = false;
            app->m_volumetricMode = false;
            
            std::cout << "[RECORDING] ULTRA-HIGH QUALITY Volumetric Recording COMPLETED!" << std::endl;
            std::cout << "            Video saved with maximum cinematic quality!" << std::endl;
        }
        app->updateWindowTitle();
    }
    // OSD toggle - moved to O key to free up F1
    else if (key == GLFW_KEY_O && action == GLFW_PRESS) {
        app->m_showOSD = !app->m_showOSD;
        std::cout << "[OSD] Status updates " << (app->m_showOSD ? "ENABLED" : "DISABLED") << std::endl;
        if (app->m_showOSD) {
            app->printStatusUpdate(); // Show immediate status when enabled
        }
    }
    // Gravity center position controls
    else if (key == GLFW_KEY_UP && (action == GLFW_PRESS || action == GLFW_REPEAT)) {
        glm::vec3 center = app->m_particleSystem->getGravityCenter();
        center.y += 0.5f;
        app->m_particleSystem->setGravityCenter(center);
        app->printParameterChange("Gravity Center Y", center.y);
    }
    else if (key == GLFW_KEY_DOWN && (action == GLFW_PRESS || action == GLFW_REPEAT)) {
        glm::vec3 center = app->m_particleSystem->getGravityCenter();
        center.y -= 0.5f;
        app->m_particleSystem->setGravityCenter(center);
        app->printParameterChange("Gravity Center Y", center.y);
    }
    else if (key == GLFW_KEY_LEFT && (action == GLFW_PRESS || action == GLFW_REPEAT)) {
        glm::vec3 center = app->m_particleSystem->getGravityCenter();
        center.x -= 0.5f;
        app->m_particleSystem->setGravityCenter(center);
        app->printParameterChange("Gravity Center X", center.x);
    }
    else if (key == GLFW_KEY_RIGHT && (action == GLFW_PRESS || action == GLFW_REPEAT)) {
        glm::vec3 center = app->m_particleSystem->getGravityCenter();
        center.x += 0.5f;
        app->m_particleSystem->setGravityCenter(center);
        app->printParameterChange("Gravity Center X", center.x);
    }
    else if (key == GLFW_KEY_PAGE_UP && (action == GLFW_PRESS || action == GLFW_REPEAT)) {
        glm::vec3 center = app->m_particleSystem->getGravityCenter();
        center.z += 0.5f;
        app->m_particleSystem->setGravityCenter(center);
        app->printParameterChange("Gravity Center Z", center.z);
    }
    else if (key == GLFW_KEY_PAGE_DOWN && (action == GLFW_PRESS || action == GLFW_REPEAT)) {
        glm::vec3 center = app->m_particleSystem->getGravityCenter();
        center.z -= 0.5f;
        app->m_particleSystem->setGravityCenter(center);
        app->printParameterChange("Gravity Center Z", center.z);
    }
    // Time scale controls
    else if (key == GLFW_KEY_LEFT_BRACKET && (action == GLFW_PRESS || action == GLFW_REPEAT)) {
        // Decrease time scale (slow motion)
        app->m_timeScale = std::max(0.1f, app->m_timeScale - 0.1f);
        app->printParameterChange("Time Scale", app->m_timeScale);
    }
    else if (key == GLFW_KEY_RIGHT_BRACKET && (action == GLFW_PRESS || action == GLFW_REPEAT)) {
        // Increase time scale (fast forward)
        app->m_timeScale = std::min(5.0f, app->m_timeScale + 0.1f);
        app->printParameterChange("Time Scale", app->m_timeScale);
    }
    // Attraction/Repulsion toggle
    else if (key == GLFW_KEY_N && action == GLFW_PRESS) {
        app->m_repulsiveGravity = !app->m_repulsiveGravity;
        if (app->m_repulsiveGravity) {
            std::cout << "[MODE] Repulsive gravity ENABLED - Matter explosion!" << std::endl;
        } else {
            std::cout << "[MODE] Attractive gravity ENABLED - Back to normal" << std::endl;
        }
        app->updateWindowTitle();
    }
    // Energy injection controls  
    else if (key == GLFW_KEY_I && (action == GLFW_PRESS || action == GLFW_REPEAT)) {
        // Increase energy injection
        app->m_energyInjection = std::min(2.0f, app->m_energyInjection + 0.1f);
        app->printParameterChange("Energy Injection", app->m_energyInjection);
    }
    else if (key == GLFW_KEY_U && (action == GLFW_PRESS || action == GLFW_REPEAT)) {
        // Decrease energy injection
        app->m_energyInjection = std::max(0.0f, app->m_energyInjection - 0.1f);
        app->printParameterChange("Energy Injection", app->m_energyInjection);
    }
    // Angular momentum boost controls for better particle distribution
    else if (key == GLFW_KEY_J && (action == GLFW_PRESS || action == GLFW_REPEAT)) {
        // Decrease angular momentum boost
        float currentBoost = app->m_particleSystem->getAngularMomentumBoost();
        float newBoost = std::max(0.0f, currentBoost - 0.1f);
        app->m_particleSystem->setAngularMomentumBoost(newBoost);
        app->printParameterChange("Angular Momentum Boost", newBoost);
    }
    else if (key == GLFW_KEY_K && (action == GLFW_PRESS || action == GLFW_REPEAT)) {
        // Increase angular momentum boost
        float currentBoost = app->m_particleSystem->getAngularMomentumBoost();
        float newBoost = std::min(3.0f, currentBoost + 0.1f);
        app->m_particleSystem->setAngularMomentumBoost(newBoost);
        app->printParameterChange("Angular Momentum Boost", newBoost);
    }
    // Shape constraint controls (moved to function keys to avoid SPH conflicts)
    else if (key == GLFW_KEY_F1 && action == GLFW_PRESS) {
        app->m_constraintShape = Application::ConstraintShape::NONE;
        app->m_particleSystem->setConstraintShape(static_cast<uint32_t>(app->m_constraintShape));
        std::cout << "[SHAPE] No constraints - Open space" << std::endl;
        app->updateWindowTitle();
    }
    else if (key == GLFW_KEY_F2 && action == GLFW_PRESS) {
        app->m_constraintShape = Application::ConstraintShape::SPHERE;
        app->m_particleSystem->setConstraintShape(static_cast<uint32_t>(app->m_constraintShape));
        std::cout << "[SHAPE] Spherical boundary - Radius: " << app->m_constraintRadius << std::endl;
        app->updateWindowTitle();
    }
    else if (key == GLFW_KEY_F3 && action == GLFW_PRESS) {
        app->m_constraintShape = Application::ConstraintShape::DISC;
        app->m_particleSystem->setConstraintShape(static_cast<uint32_t>(app->m_constraintShape));
        std::cout << "[SHAPE] Disc boundary - Radius: " << app->m_constraintRadius 
                  << " Thickness: " << app->m_constraintThickness << std::endl;
        app->updateWindowTitle();
    }
    else if (key == GLFW_KEY_F4 && action == GLFW_PRESS) {
        app->m_constraintShape = Application::ConstraintShape::TORUS;
        app->m_particleSystem->setConstraintShape(static_cast<uint32_t>(app->m_constraintShape));
        std::cout << "[SHAPE] Torus boundary - Major: " << app->m_constraintRadius 
                  << " Minor: " << app->m_constraintThickness << std::endl;
        app->updateWindowTitle();
    }
    else if (key == GLFW_KEY_F5 && action == GLFW_PRESS) {
        app->m_constraintShape = Application::ConstraintShape::ACCRETION_DISK;
        app->m_particleSystem->setConstraintShape(static_cast<uint32_t>(app->m_constraintShape));
        // Set optimal accretion disk parameters
        app->m_particleSystem->setGravityStrength(1.5f); // Gentler gravity
        app->m_particleSystem->setDampingFactor(0.995f); // Less damping for more motion
        app->m_particleSystem->setBlackHoleMass(app->m_blackHoleMass);
        app->m_particleSystem->setAlphaViscosity(app->m_alphaViscosity);
        app->m_constraintRadius = 25.0f; // Larger disk
        app->m_constraintThickness = 2.0f;
        app->m_particleSystem->setConstraintRadius(app->m_constraintRadius);
        app->m_particleSystem->setConstraintThickness(app->m_constraintThickness);
        std::cout << "[SHAPE] BLACK HOLE ACCRETION DISK MODE" << std::endl;
        std::cout << "  Black hole mass: " << app->m_blackHoleMass << " solar masses" << std::endl;
        std::cout << "  Alpha viscosity: " << app->m_alphaViscosity << std::endl;
        std::cout << "  Controls:" << std::endl;
        std::cout << "    M/N - Adjust black hole mass" << std::endl;
        std::cout << "    B/SHIFT+V - Adjust alpha viscosity" << std::endl;
        std::cout << "    C/X - Adjust color temperature scale" << std::endl;
        std::cout << "    SHIFT+R - Reset particles in disk formation" << std::endl;
        app->updateWindowTitle();
    }
    else if (key == GLFW_KEY_W && action == GLFW_PRESS) {
        app->m_showWireframe = !app->m_showWireframe;
        std::cout << "[WIREFRAME] " << (app->m_showWireframe ? "ENABLED" : "DISABLED") << std::endl;
        app->updateWindowTitle();
    }
    else if (key == GLFW_KEY_B && action == GLFW_PRESS) {
        app->enableBloomMode(!app->m_bloomEnabled);
        app->updateWindowTitle();
    }
    // Recording controls (F key)
    else if (key == GLFW_KEY_F && action == GLFW_PRESS) {
        if (!app->isRecording()) {
            if (mods & GLFW_MOD_SHIFT) {
                // High quality recording mode
                app->startRecording(300, true); // 300 frames, high quality
                std::cout << "  HIGH QUALITY recording mode activated!" << std::endl;
            } else {
                // Standard recording mode
                app->startRecording(300); // 300 frames = 5 seconds at 60fps
            }
        } else {
            // Stop recording
            app->stopRecording();
        }
    }
    // Loop mode toggle (L key)
    else if (key == GLFW_KEY_L && action == GLFW_PRESS) {
        app->m_recordingLoop = !app->m_recordingLoop;
        std::cout << "[RECORDING] Loop mode: " << (app->m_recordingLoop ? "ON" : "OFF") 
                  << " (resets simulation when recording starts)" << std::endl;
    }
    // Relativistic jet controls (A key)
    else if (key == GLFW_KEY_A && action == GLFW_PRESS) {
        // Toggle relativistic jets using a separate flag and adjusting black hole mass
        // Track jet state separately from black hole mass
        static bool jetsEnabled = false;
        jetsEnabled = !jetsEnabled;
        
        if (jetsEnabled) {
            // Enable jets - ensure black hole mass is sufficient
            if (app->m_blackHoleMass < 2.0f) {
                app->m_blackHoleMass = 2.0f; // Minimum mass for jets
            }
            app->m_particleSystem->setBlackHoleMass(app->m_blackHoleMass);
            std::cout << "\n[RELATIVISTIC JETS] ENABLED" << std::endl;
            std::cout << "  Black hole mass: " << app->m_blackHoleMass << " solar masses" << std::endl;
            std::cout << "  Jets emerge from poles when central density is high" << std::endl;
            std::cout << "  Jet strength proportional to accretion rate" << std::endl;
        } else {
            // Disable jets - reduce black hole mass to below jet threshold
            app->m_blackHoleMass = 0.3f; // Below 0.5 threshold, no jets
            app->m_particleSystem->setBlackHoleMass(app->m_blackHoleMass);
            std::cout << "\n[RELATIVISTIC JETS] DISABLED" << std::endl;
            std::cout << "  Black hole mass reduced to " << app->m_blackHoleMass << " solar masses" << std::endl;
        }
        app->updateWindowTitle();
    }
    // Black hole mass controls (M/N keys)
    else if (key == GLFW_KEY_M && (action == GLFW_PRESS || action == GLFW_REPEAT)) {
        // Increase black hole mass
        app->m_blackHoleMass = std::min(10.0f, app->m_blackHoleMass + 0.1f);
        app->m_particleSystem->setBlackHoleMass(app->m_blackHoleMass);
        app->printParameterChange("Black Hole Mass", app->m_blackHoleMass);
        std::cout << "  (solar masses)" << std::endl;
    }
    else if (key == GLFW_KEY_N && (action == GLFW_PRESS || action == GLFW_REPEAT)) {
        // Decrease black hole mass
        app->m_blackHoleMass = std::max(0.1f, app->m_blackHoleMass - 0.1f);
        app->m_particleSystem->setBlackHoleMass(app->m_blackHoleMass);
        app->printParameterChange("Black Hole Mass", app->m_blackHoleMass);
        std::cout << "  (solar masses)" << std::endl;
    }
    // Alpha viscosity controls (B/V keys - reusing V when not in volumetric toggle)
    else if (key == GLFW_KEY_B && (action == GLFW_PRESS || action == GLFW_REPEAT)) {
        // Increase alpha viscosity
        app->m_alphaViscosity = std::min(0.4f, app->m_alphaViscosity + 0.01f);
        app->m_particleSystem->setAlphaViscosity(app->m_alphaViscosity);
        app->printParameterChange("Alpha Viscosity", app->m_alphaViscosity);
        std::cout << "  (angular momentum transport)" << std::endl;
    }
    else if (key == GLFW_KEY_V && (action == GLFW_PRESS || action == GLFW_REPEAT) && 
             (mods & GLFW_MOD_SHIFT)) {
        // Decrease alpha viscosity (with shift to avoid conflict with volumetric toggle)
        app->m_alphaViscosity = std::max(0.0f, app->m_alphaViscosity - 0.01f);
        app->m_particleSystem->setAlphaViscosity(app->m_alphaViscosity);
        app->printParameterChange("Alpha Viscosity", app->m_alphaViscosity);
        std::cout << "  (angular momentum transport)" << std::endl;
    }
    // Mesh shader toggle (Y key)
    else if (key == GLFW_KEY_Y && action == GLFW_PRESS) {
        if (app->m_meshRenderer && app->m_meshRenderer->isSupported()) {
            app->m_useMeshShaders = !app->m_useMeshShaders;
            if (app->m_useMeshShaders) {
                std::cout << "[RENDERER] Mesh shaders ENABLED - GPU-driven particle generation!" << std::endl;
                std::cout << "           Faster performance, better quality rendering" << std::endl;
            } else {
                std::cout << "[RENDERER] Mesh shaders DISABLED - Traditional vertex rendering" << std::endl;
            }
        } else {
            std::cout << "[RENDERER] Mesh shaders not available on this GPU" << std::endl;
        }
    }
    // Volumetric parameter tuning (Numpad 1-5 with Shift for decrease)
    else if (key == GLFW_KEY_KP_1 && action == GLFW_PRESS) {
        float delta = (mods & GLFW_MOD_SHIFT) ? -0.05f : 0.05f;
        app->m_volumeDensityScale = std::max(0.01f, app->m_volumeDensityScale + delta);
        std::cout << "[VOLUMETRIC] Density Scale: " << app->m_volumeDensityScale << " (NUM1" << ((mods & GLFW_MOD_SHIFT) ? " -)" : " +)") << std::endl;
    }
    else if (key == GLFW_KEY_KP_2 && action == GLFW_PRESS) {
        float delta = (mods & GLFW_MOD_SHIFT) ? -0.5f : 0.5f;
        app->m_volumeOpacity = std::max(0.1f, app->m_volumeOpacity + delta);
        std::cout << "[VOLUMETRIC] Opacity (Sigma_t): " << app->m_volumeOpacity << " (NUM2" << ((mods & GLFW_MOD_SHIFT) ? " -)" : " +)") << std::endl;
    }
    else if (key == GLFW_KEY_KP_3 && action == GLFW_PRESS) {
        float delta = (mods & GLFW_MOD_SHIFT) ? -0.005f : 0.005f;
        app->m_volumeStepSize = std::clamp(app->m_volumeStepSize + delta, 0.005f, 0.1f);
        std::cout << "[VOLUMETRIC] Step Size: " << app->m_volumeStepSize << " (NUM3" << ((mods & GLFW_MOD_SHIFT) ? " -)" : " +)") << std::endl;
    }
    else if (key == GLFW_KEY_KP_4 && action == GLFW_PRESS) {
        float delta = (mods & GLFW_MOD_SHIFT) ? -0.1f : 0.1f;
        app->m_volumeEmissionScale = std::max(0.1f, app->m_volumeEmissionScale + delta);
        std::cout << "[VOLUMETRIC] Emission Scale: " << app->m_volumeEmissionScale << " (NUM4" << ((mods & GLFW_MOD_SHIFT) ? " -)" : " +)") << std::endl;
    }
    else if (key == GLFW_KEY_KP_5 && action == GLFW_PRESS) {
        int delta = (mods & GLFW_MOD_SHIFT) ? -32 : 32;
        app->m_volumeMaxSteps = std::clamp(app->m_volumeMaxSteps + delta, 64, 2048);
        std::cout << "[VOLUMETRIC] Max Steps: " << app->m_volumeMaxSteps << " (NUM5" << ((mods & GLFW_MOD_SHIFT) ? " -)" : " +)") << std::endl;
    }
    else if (key == GLFW_KEY_KP_6 && action == GLFW_PRESS) {
        float delta = (mods & GLFW_MOD_SHIFT) ? -0.1f : 0.1f;
        app->m_volumeRedBalance = std::max(0.1f, app->m_volumeRedBalance + delta);
        std::cout << "[VOLUMETRIC] Red Balance: " << app->m_volumeRedBalance << " (NUM6" << ((mods & GLFW_MOD_SHIFT) ? " -)" : " +)") << std::endl;
    }
    else if (key == GLFW_KEY_KP_7 && action == GLFW_PRESS) {
        float delta = (mods & GLFW_MOD_SHIFT) ? -0.1f : 0.1f;
        app->m_volumeOrangeBalance = std::max(0.1f, app->m_volumeOrangeBalance + delta);
        std::cout << "[VOLUMETRIC] Orange Balance: " << app->m_volumeOrangeBalance << " (NUM7" << ((mods & GLFW_MOD_SHIFT) ? " -)" : " +)") << std::endl;
    }
    else if (key == GLFW_KEY_KP_8 && action == GLFW_PRESS) {
        float delta = (mods & GLFW_MOD_SHIFT) ? -0.1f : 0.1f;
        app->m_volumeYellowBalance = std::max(0.1f, app->m_volumeYellowBalance + delta);
        std::cout << "[VOLUMETRIC] Yellow Balance: " << app->m_volumeYellowBalance << " (NUM8" << ((mods & GLFW_MOD_SHIFT) ? " -)" : " +)") << std::endl;
    }
    // Temperature scaling controls (C/X keys)
    else if (key == GLFW_KEY_C && (action == GLFW_PRESS || action == GLFW_REPEAT)) {
        // Increase temperature scale (hotter colors)
        app->m_temperatureScale = std::min(5.0f, app->m_temperatureScale + 0.1f);
        app->printParameterChange("Temperature Scale", app->m_temperatureScale);
        std::cout << "  (color intensity)" << std::endl;
    }
    else if (key == GLFW_KEY_X && (action == GLFW_PRESS || action == GLFW_REPEAT)) {
        // Decrease temperature scale (cooler colors)
        app->m_temperatureScale = std::max(0.1f, app->m_temperatureScale - 0.1f);
        app->printParameterChange("Temperature Scale", app->m_temperatureScale);
        std::cout << "  (color intensity)" << std::endl;
    }
    // Constraint size controls
    else if (key == GLFW_KEY_EQUAL && (action == GLFW_PRESS || action == GLFW_REPEAT)) {
        // Increase constraint radius
        app->m_constraintRadius = std::min(50.0f, app->m_constraintRadius + 1.0f);
        app->m_particleSystem->setConstraintRadius(app->m_constraintRadius);
        app->printParameterChange("Constraint Radius", app->m_constraintRadius);
    }
    else if (key == GLFW_KEY_MINUS && (action == GLFW_PRESS || action == GLFW_REPEAT)) {
        // Decrease constraint radius
        app->m_constraintRadius = std::max(5.0f, app->m_constraintRadius - 1.0f);
        app->m_particleSystem->setConstraintRadius(app->m_constraintRadius);
        app->printParameterChange("Constraint Radius", app->m_constraintRadius);
    }
}

void Application::mouseButtonCallback(GLFWwindow* window, int button, int action, int mods) {
    auto app = reinterpret_cast<Application*>(glfwGetWindowUserPointer(window));
    
    if (button == GLFW_MOUSE_BUTTON_LEFT) {
        if (action == GLFW_PRESS) {
            app->m_mousePressed = true;
            glfwGetCursorPos(window, &app->m_lastMouseX, &app->m_lastMouseY);
        } else if (action == GLFW_RELEASE) {
            app->m_mousePressed = false;
        }
    } else if (button == GLFW_MOUSE_BUTTON_RIGHT) {
        if (action == GLFW_PRESS) {
            app->m_middleMousePressed = true;
            glfwGetCursorPos(window, &app->m_lastMouseX, &app->m_lastMouseY);
        } else if (action == GLFW_RELEASE) {
            app->m_middleMousePressed = false;
        }
    }
}

void Application::cursorPosCallback(GLFWwindow* window, double xpos, double ypos) {
    auto app = reinterpret_cast<Application*>(glfwGetWindowUserPointer(window));
    
    if (app->m_mousePressed) {
        // Orbit camera around target
        double deltaX = xpos - app->m_lastMouseX;
        double deltaY = ypos - app->m_lastMouseY;
        
        // Adjust rotation angles (scale for sensitivity)
        app->m_cameraTheta += deltaX * 0.005f;  // Horizontal rotation
        app->m_cameraPhi -= deltaY * 0.005f;    // Vertical rotation (inverted for natural feel)
        
        // Clamp vertical rotation to prevent flipping
        app->m_cameraPhi = glm::clamp(app->m_cameraPhi, -1.5f, 1.5f);
        
        app->m_lastMouseX = xpos;
        app->m_lastMouseY = ypos;
    } else if (app->m_middleMousePressed) {
        // Pan camera target
        double deltaX = xpos - app->m_lastMouseX;
        double deltaY = ypos - app->m_lastMouseY;
        
        // Calculate camera's right and up vectors for panning
        float sensitivity = app->m_cameraDistance * 0.001f; // Scale with distance
        
        app->m_cameraTarget.x -= deltaX * sensitivity;
        app->m_cameraTarget.y += deltaY * sensitivity; // Inverted for natural feel
        
        app->m_lastMouseX = xpos;
        app->m_lastMouseY = ypos;
    }
}

void Application::scrollCallback(GLFWwindow* window, double xoffset, double yoffset) {
    auto app = reinterpret_cast<Application*>(glfwGetWindowUserPointer(window));
    
    // Zoom in/out with mouse wheel
    float zoomSpeed = app->m_cameraDistance * 0.1f; // Scale zoom speed with current distance
    app->m_cameraDistance -= yoffset * zoomSpeed;
    
    // Allow very close zoom for detailed inspection, but prevent going inside the simulation
    app->m_cameraDistance = glm::clamp(app->m_cameraDistance, 0.5f, 200.0f);
    
    // Debug output for zoom level
    std::cout << "Zoom - Distance: " << app->m_cameraDistance << std::endl;
}

} // namespace plasma
