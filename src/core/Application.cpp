#include "Application.h"
#include <iostream>
#include <chrono>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace plasma {

Application::Application(const std::string& title, uint32_t width, uint32_t height)
    : m_title(title), m_width(width), m_height(height) {

    initWindow();
    initVulkan();
    initImGui();
}

Application::~Application() {
    cleanup();
}

void Application::run() {
    m_isRunning = true;

    auto lastTime = std::chrono::high_resolution_clock::now();

    while (m_isRunning && !glfwWindowShouldClose(m_window)) {
        auto currentTime = std::chrono::high_resolution_clock::now();
        float deltaTime = std::chrono::duration<float, std::chrono::seconds::period>(
            currentTime - lastTime).count();
        lastTime = currentTime;
        
        // Cap deltaTime to prevent physics instability
        // This ensures consistent behavior regardless of framerate
        const float maxDeltaTime = 1.0f / 30.0f; // 30 FPS minimum
        deltaTime = std::min(deltaTime, maxDeltaTime);

        glfwPollEvents();

        update(deltaTime);
        render();

        // Calculate FPS
        m_frameCount++;
        m_frameTime += deltaTime;
        if (m_frameTime >= 1.0f) {
            m_fps = m_frameCount / m_frameTime;
            m_frameCount = 0;
            m_frameTime = 0.0f;

            // Update window title with FPS
            std::string newTitle = m_title + " - FPS: " + std::to_string(static_cast<int>(m_fps));
            glfwSetWindowTitle(m_window, newTitle.c_str());
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

    // Skip VMA for now to debug the rest of the application
    std::cout << "Skipping VMA allocator for debugging..." << std::endl;
    m_allocator = nullptr;
    
    // TODO: Re-enable VMA after debugging
    /*
    std::cout << "Initializing VMA allocator..." << std::endl;
    // Initialize VMA allocator
    VmaAllocatorCreateInfo allocatorInfo = {};
    allocatorInfo.vulkanApiVersion = VK_API_VERSION_1_3;  // Use 1.3 for better compatibility
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
    
    // Create particle system
    try {
        m_particleSystem = std::make_unique<ParticleSystem>(m_vulkanContext.get(), 250000); // 250k particles - utilizing performance headroom
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
    // Update total time
    m_totalTime += deltaTime;
    
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

    // Acquire next image
    uint32_t imageIndex;
    VkResult result = vkAcquireNextImageKHR(
        m_vulkanContext->getDevice(),
        m_vulkanContext->getSwapChain(),
        UINT64_MAX,
        m_vulkanContext->getImageAvailableSemaphore(m_currentFrame),
        VK_NULL_HANDLE,
        &imageIndex
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
    recordCommandBuffer(m_commandBuffers[m_currentFrame], imageIndex);

    // Submit command buffer
    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;

    VkSemaphore waitSemaphores[] = {m_vulkanContext->getImageAvailableSemaphore(m_currentFrame)};
    VkPipelineStageFlags waitStages[] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores = waitSemaphores;
    submitInfo.pWaitDstStageMask = waitStages;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &m_commandBuffers[m_currentFrame];

    VkSemaphore signalSemaphores[] = {m_vulkanContext->getRenderFinishedSemaphore(m_currentFrame)};
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = signalSemaphores;

    if (vkQueueSubmit(m_vulkanContext->getGraphicsQueue(), 1, &submitInfo,
        fence) != VK_SUCCESS) {
        throw std::runtime_error("Failed to submit draw command buffer!");
    }

    // Present
    VkPresentInfoKHR presentInfo{};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = signalSemaphores;

    VkSwapchainKHR swapChains[] = {m_vulkanContext->getSwapChain()};
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = swapChains;
    presentInfo.pImageIndices = &imageIndex;

    result = vkQueuePresentKHR(m_vulkanContext->getPresentQueue(), &presentInfo);

    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR || m_framebufferResized) {
        m_framebufferResized = false;
        recreateSwapChain();
    } else if (result != VK_SUCCESS) {
        throw std::runtime_error("Failed to present swap chain image!");
    }

    m_currentFrame = (m_currentFrame + 1) % VulkanContext::MAX_FRAMES_IN_FLIGHT;
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
    if (m_particleSystem) {
        m_particleSystem->update(commandBuffer, 0.016f, m_totalTime); // Fixed timestep for now
        
        // Update volumetric density grid from particles (if enabled)
        if (m_volumetricMode && m_volumeRenderer) {
            // Update density grid from particle data
            VkBuffer particleBuffer = m_particleSystem->getParticleBuffer();
            uint32_t activeParticles = m_particleSystem->getActiveParticleCount();
            m_volumeRenderer->updateDensityGrid(commandBuffer, particleBuffer, activeParticles);
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
            m_volumeRenderer->render(commandBuffer, viewProj, cameraPos);
        } else {
            // Render particles (drawing only, physics already updated)
            m_particleSystem->render(commandBuffer, viewProj);
        }

        // Render ImGui on top
        renderImGui(commandBuffer);
    }

    vkCmdEndRendering(commandBuffer);

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

void Application::initImGui() {
    // Create descriptor pool for ImGui
    VkDescriptorPoolSize poolSizes[] = {
        {VK_DESCRIPTOR_TYPE_SAMPLER, 1000},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1000},
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1000},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1000},
        {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1000},
        {VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 1000},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1000},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1000},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1000},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1000},
        {VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 1000}
    };

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolInfo.maxSets = 1000;
    poolInfo.poolSizeCount = std::size(poolSizes);
    poolInfo.pPoolSizes = poolSizes;

    if (vkCreateDescriptorPool(m_vulkanContext->getDevice(), &poolInfo, nullptr, &m_imguiDescriptorPool) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create ImGui descriptor pool!");
    }

    // Setup Dear ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    // Setup Dear ImGui style
    ImGui::StyleColorsDark();

    // Initialize ImGui for GLFW and Vulkan
    ImGui_ImplGlfw_InitForVulkan(m_window, true);

    ImGui_ImplVulkan_InitInfo initInfo{};
    initInfo.Instance = m_vulkanContext->getInstance();
    initInfo.PhysicalDevice = m_vulkanContext->getPhysicalDevice();
    initInfo.Device = m_vulkanContext->getDevice();
    initInfo.QueueFamily = m_vulkanContext->getQueueFamilyIndices().graphicsFamily.value();
    initInfo.Queue = m_vulkanContext->getGraphicsQueue();
    initInfo.PipelineCache = VK_NULL_HANDLE;
    initInfo.DescriptorPool = m_imguiDescriptorPool;
    initInfo.RenderPass = VK_NULL_HANDLE; // Using dynamic rendering
    initInfo.Subpass = 0;
    initInfo.MinImageCount = VulkanContext::MAX_FRAMES_IN_FLIGHT;
    initInfo.ImageCount = VulkanContext::MAX_FRAMES_IN_FLIGHT;
    initInfo.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    initInfo.UseDynamicRendering = true;

    ImGui_ImplVulkan_Init(&initInfo);

    // Upload fonts
    VkCommandBuffer commandBuffer = m_vulkanContext->beginSingleTimeCommands();
    ImGui_ImplVulkan_CreateFontsTexture();
    m_vulkanContext->endSingleTimeCommands(commandBuffer);
    ImGui_ImplVulkan_DestroyFontsTexture();
}

void Application::renderImGui(VkCommandBuffer commandBuffer) {
    if (!m_showGUI) return;

    // Start the Dear ImGui frame
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    // Create main control panel
    ImGui::Begin("PlasmaVulkan Control Panel", &m_showGUI);

    // Physics parameters
    if (ImGui::CollapsingHeader("Physics Parameters", ImGuiTreeNodeFlags_DefaultOpen)) {
        float gravity = m_particleSystem->getGravityStrength();
        if (ImGui::SliderFloat("Gravity Strength", &gravity, 0.0f, 5.0f, "%.2f")) {
            m_particleSystem->setGravityStrength(gravity);
        }

        float turbulence = m_particleSystem->getTurbulenceStrength();
        if (ImGui::SliderFloat("Turbulence", &turbulence, 0.0f, 1.0f, "%.3f")) {
            m_particleSystem->setTurbulenceStrength(turbulence);
        }

        float damping = m_particleSystem->getDampingFactor();
        if (ImGui::SliderFloat("Damping", &damping, 0.9f, 1.0f, "%.4f")) {
            m_particleSystem->setDampingFactor(damping);
        }

        int particleCount = static_cast<int>(m_particleSystem->getActiveParticleCount());
        int maxParticles = static_cast<int>(m_particleSystem->getParticleCount());
        if (ImGui::SliderInt("Active Particles", &particleCount, 1000, maxParticles, "%d")) {
            m_particleSystem->setActiveParticleCount(static_cast<uint32_t>(particleCount));
        }

        // Reset button
        if (ImGui::Button("Reset Physics")) {
            m_particleSystem->setGravityStrength(0.6f);
            m_particleSystem->setTurbulenceStrength(0.0f);
            m_particleSystem->setDampingFactor(0.999f);
        }
    }

    // Simulation modes
    if (ImGui::CollapsingHeader("Simulation Modes", ImGuiTreeNodeFlags_DefaultOpen)) {
        bool sphMode = m_particleSystem->isSPHMode();
        if (ImGui::Checkbox("SPH Fluid Mode", &sphMode)) {
            m_particleSystem->setSPHMode(sphMode);
            if (sphMode) {
                m_particleSystem->setActiveParticleCount(m_sphParticleCount);
            } else {
                m_particleSystem->setActiveParticleCount(m_fullParticleCount);
            }
        }

        ImGui::Checkbox("Volumetric Rendering", &m_volumetricMode);
    }

    // Camera controls
    if (ImGui::CollapsingHeader("Camera Controls")) {
        ImGui::SliderFloat("Distance", &m_cameraDistance, 0.5f, 200.0f, "%.1f");
        ImGui::SliderFloat("Theta", &m_cameraTheta, -3.14f, 3.14f, "%.2f");
        ImGui::SliderFloat("Phi", &m_cameraPhi, -1.5f, 1.5f, "%.2f");
        
        if (ImGui::Button("Reset Camera")) {
            m_cameraDistance = 20.0f;
            m_cameraTheta = 0.0f;
            m_cameraPhi = 0.0f;
            m_cameraTarget = {0.0f, 0.0f, 0.0f};
        }
    }

    // Performance info
    if (ImGui::CollapsingHeader("Performance")) {
        ImGui::Text("FPS: %.1f", m_fps);
        ImGui::Text("Frame Time: %.3f ms", m_frameTime * 1000.0f);
        ImGui::Text("Total Time: %.2f s", m_totalTime);
        ImGui::Text("Particles: %u / %u", 
                   m_particleSystem->getActiveParticleCount(),
                   m_particleSystem->getParticleCount());
    }

    // Controls help
    if (ImGui::CollapsingHeader("Keyboard Controls")) {
        ImGui::Text("G/Shift+G: Adjust gravity strength");
        ImGui::Text("T/Shift+T: Adjust turbulence");
        ImGui::Text("D/Shift+D: Adjust damping");
        ImGui::Text("P/Shift+P: Adjust particle count");
        ImGui::Text("R: Reset physics parameters");
        ImGui::Text("H: Print help to console");
        ImGui::Text("Space: Toggle SPH fluid mode");
        ImGui::Text("V: Toggle volumetric rendering");
        ImGui::Text("Mouse: Orbit camera");
        ImGui::Text("Wheel: Zoom in/out");
        ImGui::Text("Middle Mouse: Pan camera");
    }

    ImGui::End();

    // Render ImGui
    ImGui::Render();
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), commandBuffer);
}

void Application::cleanup() {
    // Cleanup ImGui
    if (m_imguiDescriptorPool != VK_NULL_HANDLE) {
        ImGui_ImplVulkan_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();

        vkDestroyDescriptorPool(m_vulkanContext->getDevice(), m_imguiDescriptorPool, nullptr);
        m_imguiDescriptorPool = VK_NULL_HANDLE;
    }

    if (m_allocator) {
        vmaDestroyAllocator(m_allocator);
    }

    m_vulkanContext.reset();

    if (m_window) {
        glfwDestroyWindow(m_window);
        glfwTerminate();
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
            std::cout << "Reducing particles to " << app->m_sphParticleCount << " for performance..." << std::endl;
            
            // Switch to SPH particle count for performance
            app->m_particleSystem->setActiveParticleCount(app->m_sphParticleCount);
            
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
            std::cout << "Restoring full particle count..." << std::endl;
            
            // Switch back to full particle count for orbital physics
            app->m_particleSystem->setActiveParticleCount(app->m_fullParticleCount);
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
            std::cout << "Gravity strength: " << newGravity << std::endl;
        } else {
            // Increase gravity
            float newGravity = std::min(5.0f, app->m_particleSystem->getGravityStrength() + 0.1f);
            app->m_particleSystem->setGravityStrength(newGravity);
            std::cout << "Gravity strength: " << newGravity << std::endl;
        }
    }
    else if (key == GLFW_KEY_T && (action == GLFW_PRESS || action == GLFW_REPEAT)) {
        // Turbulence strength controls
        if (mods & GLFW_MOD_SHIFT) {
            // Decrease turbulence
            float newTurbulence = std::max(0.0f, app->m_particleSystem->getTurbulenceStrength() - 0.05f);
            app->m_particleSystem->setTurbulenceStrength(newTurbulence);
            std::cout << "Turbulence strength: " << newTurbulence << std::endl;
        } else {
            // Increase turbulence
            float newTurbulence = std::min(1.0f, app->m_particleSystem->getTurbulenceStrength() + 0.05f);
            app->m_particleSystem->setTurbulenceStrength(newTurbulence);
            std::cout << "Turbulence strength: " << newTurbulence << std::endl;
        }
    }
    else if (key == GLFW_KEY_D && (action == GLFW_PRESS || action == GLFW_REPEAT)) {
        // Damping factor controls
        if (mods & GLFW_MOD_SHIFT) {
            // Decrease damping (more energy retention)
            float newDamping = std::max(0.9f, app->m_particleSystem->getDampingFactor() - 0.005f);
            app->m_particleSystem->setDampingFactor(newDamping);
            std::cout << "Damping factor: " << newDamping << std::endl;
        } else {
            // Increase damping (more energy loss)
            float newDamping = std::min(1.0f, app->m_particleSystem->getDampingFactor() + 0.005f);
            app->m_particleSystem->setDampingFactor(newDamping);
            std::cout << "Damping factor: " << newDamping << std::endl;
        }
    }
    else if (key == GLFW_KEY_P && (action == GLFW_PRESS || action == GLFW_REPEAT)) {
        // Particle count controls
        if (mods & GLFW_MOD_SHIFT) {
            // Decrease particle count
            uint32_t currentCount = app->m_particleSystem->getActiveParticleCount();
            uint32_t newCount = std::max(1000u, currentCount - 5000u);
            app->m_particleSystem->setActiveParticleCount(newCount);
            std::cout << "Active particles: " << newCount << std::endl;
        } else {
            // Increase particle count
            uint32_t currentCount = app->m_particleSystem->getActiveParticleCount();
            uint32_t maxCount = app->m_particleSystem->getParticleCount();
            uint32_t newCount = std::min(maxCount, currentCount + 5000u);
            app->m_particleSystem->setActiveParticleCount(newCount);
            std::cout << "Active particles: " << newCount << std::endl;
        }
    }
    else if (key == GLFW_KEY_R && action == GLFW_PRESS) {
        // Reset physics parameters to defaults
        app->m_particleSystem->setGravityStrength(0.6f);
        app->m_particleSystem->setGravityCenter({0.0f, 0.0f, 0.0f});
        app->m_particleSystem->setTurbulenceStrength(0.0f);
        app->m_particleSystem->setDampingFactor(0.999f);
        std::cout << "Physics parameters reset to defaults" << std::endl;
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
        std::cout << "\nControls:" << std::endl;
        std::cout << "G/Shift+G: Increase/Decrease gravity strength" << std::endl;
        std::cout << "T/Shift+T: Increase/Decrease turbulence" << std::endl;
        std::cout << "D/Shift+D: Increase/Decrease damping" << std::endl;
        std::cout << "P/Shift+P: Increase/Decrease particle count" << std::endl;
        std::cout << "R: Reset all physics parameters" << std::endl;
        std::cout << "H: Show this help" << std::endl;
    }
    // Volumetric rendering toggle
    else if (key == GLFW_KEY_V && action == GLFW_PRESS) {
        app->m_volumetricMode = !app->m_volumetricMode;
        if (app->m_volumetricMode) {
            std::cout << "Volumetric rendering ENABLED - 3D plasma glow!" << std::endl;
            std::cout << "Note: Particles → Volume → Ray march pipeline active" << std::endl;
        } else {
            std::cout << "Volumetric rendering DISABLED - Back to particle rendering" << std::endl;
        }
    }
    // GUI toggle
    else if (key == GLFW_KEY_F1 && action == GLFW_PRESS) {
        app->m_showGUI = !app->m_showGUI;
        std::cout << "GUI " << (app->m_showGUI ? "ENABLED" : "DISABLED") << std::endl;
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
    } else if (button == GLFW_MOUSE_BUTTON_MIDDLE) {
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
