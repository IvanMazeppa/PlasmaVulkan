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

# If a Vulkan SDK path is provided via environment, hint CMake before find_package
if(DEFINED ENV{VULKAN_SDK} AND EXISTS "$ENV{VULKAN_SDK}")
    list(PREPEND CMAKE_PREFIX_PATH "$ENV{VULKAN_SDK}")
    message(STATUS "VULKAN_SDK detected at: $ENV{VULKAN_SDK}")
endif()

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

# Also add Vulkan SDK include directory if environment variable is set
if(DEFINED ENV{VULKAN_SDK})
    if(EXISTS "$ENV{VULKAN_SDK}/Include")
        target_include_directories(${PROJECT_NAME} PRIVATE "$ENV{VULKAN_SDK}/Include")
    elseif(EXISTS "$ENV{VULKAN_SDK}/include")
        target_include_directories(${PROJECT_NAME} PRIVATE "$ENV{VULKAN_SDK}/include")
    endif()
endif()

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

# Optionally add Vulkan SDK lib directory to the link search path
if(DEFINED ENV{VULKAN_SDK})
    if(WIN32)
        if(EXISTS "$ENV{VULKAN_SDK}/Lib")
            target_link_directories(${PROJECT_NAME} PRIVATE "$ENV{VULKAN_SDK}/Lib")
        endif()
    else()
        if(EXISTS "$ENV{VULKAN_SDK}/lib")
            target_link_directories(${PROJECT_NAME} PRIVATE "$ENV{VULKAN_SDK}/lib")
        elseif(EXISTS "$ENV{VULKAN_SDK}/Lib")
            target_link_directories(${PROJECT_NAME} PRIVATE "$ENV{VULKAN_SDK}/Lib")
        endif()
    endif()
endif()

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
