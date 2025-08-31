cmake_minimum_required(VERSION 3.25)
project(PlasmaVulkan VERSION 1.0.0 LANGUAGES CXX C)

# Modern C++ standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Export compile commands for IDEs
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Options
option(ENABLE_VALIDATION "Enable Vulkan validation layers" ON)
option(ENABLE_PROFILING "Enable Tracy profiling" OFF)
option(USE_MESH_SHADERS "Use mesh shaders if available" ON)

# Find packages via vcpkg
find_package(Vulkan REQUIRED)
find_package(glfw3 CONFIG REQUIRED)
find_package(glm CONFIG REQUIRED)
find_package(volk CONFIG REQUIRED)
find_package(VulkanMemoryAllocator CONFIG REQUIRED)
find_package(vk-bootstrap CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)

if(ENABLE_PROFILING)
    find_package(Tracy CONFIG REQUIRED)
endif()

# Create main executable
file(GLOB_RECURSE SOURCES 
    "src/*.cpp"
    "src/*.h"
)

add_executable(${PROJECT_NAME} ${SOURCES})

# Include directories
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${Vulkan_INCLUDE_DIRS}
)

# Compile definitions
target_compile_definitions(${PROJECT_NAME} PRIVATE
    GLM_FORCE_RADIANS
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLM_ENABLE_EXPERIMENTAL
    VULKAN_HPP_NO_CONSTRUCTORS
    $<$<CONFIG:Debug>:DEBUG_BUILD>
    $<$<BOOL:${ENABLE_VALIDATION}>:ENABLE_VALIDATION>
    $<$<BOOL:${USE_MESH_SHADERS}>:USE_MESH_SHADERS>
)

# Platform-specific settings
if(WIN32)
    target_compile_definitions(${PROJECT_NAME} PRIVATE
        VK_USE_PLATFORM_WIN32_KHR
        NOMINMAX
        WIN32_LEAN_AND_MEAN
    )
elseif(UNIX AND NOT APPLE)
    target_compile_definitions(${PROJECT_NAME} PRIVATE
        VK_USE_PLATFORM_XCB_KHR
    )
endif()

# Link libraries
target_link_libraries(${PROJECT_NAME} PRIVATE
    Vulkan::Vulkan
    glfw
    glm::glm
    volk::volk_headers
    GPUOpen::VulkanMemoryAllocator
    vk-bootstrap::vk-bootstrap
    spdlog::spdlog
    $<$<BOOL:${ENABLE_PROFILING}>:Tracy::TracyClient>
)

# Compiler warnings
if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE 
        /W4 
        /permissive- 
        /Zc:preprocessor
        $<$<CONFIG:Release>:/O2>
    )
else()
    target_compile_options(${PROJECT_NAME} PRIVATE 
        -Wall 
        -Wextra 
        -Wpedantic
        -Wno-missing-field-initializers
        $<$<CONFIG:Release>:-O3>
    )
endif()

# Shader compilation
add_subdirectory(shaders)

# Ensure shaders are built before the executable is linked/run
add_dependencies(${PROJECT_NAME} shaders)

# Copy assets to build directory (only if present)
if(EXISTS "${CMAKE_SOURCE_DIR}/assets")
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/assets"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/assets"
    )
endif()

# Install rules
install(TARGETS ${PROJECT_NAME} DESTINATION bin)
if(EXISTS "${CMAKE_SOURCE_DIR}/assets")
    install(DIRECTORY assets/ DESTINATION bin/assets)
endif()
install(DIRECTORY shaders/ DESTINATION bin/shaders FILES_MATCHING PATTERN "*.spv")
