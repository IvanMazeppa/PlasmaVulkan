# Shader compilation
find_program(GLSLC glslc HINTS 
    $ENV{VULKAN_SDK}/Bin 
    $ENV{VULKAN_SDK}/Bin32
    ${Vulkan_GLSLC_EXECUTABLE}
)

if(NOT GLSLC)
    message(FATAL_ERROR "glslc not found! Please install Vulkan SDK.")
endif()

# List of shaders to compile
set(SHADER_SOURCES
    particle.comp
    particle.vert
    particle.frag
    sph_simple.comp
    density_splat.comp
    density_splat_scatter.comp
    volume.vert
    volume.frag
    # TAA (Temporal Anti-Aliasing) shaders for noise reduction
    taa.vert
    taa.frag
    # Bloom post-processing shaders
    fullscreen.vert
    bloom_bright.frag
    bloom_blur.frag
    bloom_combine.frag
    # sph.comp           # Complex SPH - TODO later
    # spatial_hash.comp  # Spatial hashing - TODO later
)

# Compile each shader
foreach(SHADER ${SHADER_SOURCES})
    get_filename_component(SHADER_NAME ${SHADER} NAME_WE)
    get_filename_component(SHADER_EXT ${SHADER} EXT)
    
    # Determine shader stage
    if(${SHADER_EXT} STREQUAL ".vert")
        set(SHADER_STAGE "vertex")
        set(EXTRA_ARGS "")
    elseif(${SHADER_EXT} STREQUAL ".frag")
        set(SHADER_STAGE "fragment")
        # Use SPIR-V 1.3 and Vulkan 1.1 for volume.frag which uses subgroup operations
        if(${SHADER_NAME} STREQUAL "volume")
            set(EXTRA_ARGS "--target-spv=spv1.3" "--target-env=vulkan1.1")
        else()
            set(EXTRA_ARGS "")
        endif()
    elseif(${SHADER_EXT} STREQUAL ".comp")
        set(SHADER_STAGE "compute")
        set(EXTRA_ARGS "")
    elseif(${SHADER_EXT} STREQUAL ".geom")
        set(SHADER_STAGE "geometry")
        set(EXTRA_ARGS "")
    elseif(${SHADER_EXT} STREQUAL ".mesh")
        set(SHADER_STAGE "mesh")
        set(EXTRA_ARGS "--target-spv=spv1.4" "-fshader-stage=mesh")
    else()
        message(FATAL_ERROR "Unknown shader extension: ${SHADER_EXT}")
    endif()
    
    set(SHADER_OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${SHADER}.spv)
    set(SHADER_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/${SHADER})
    
    # Add compilation command
    add_custom_command(
        OUTPUT ${SHADER_OUTPUT}
        COMMAND ${GLSLC} 
            ${EXTRA_ARGS}
            -O 
            ${SHADER_SOURCE} 
            -o ${SHADER_OUTPUT}
        DEPENDS ${SHADER_SOURCE}
        COMMENT "Compiling ${SHADER}"
    )
    
    list(APPEND SHADER_OUTPUTS ${SHADER_OUTPUT})
endforeach()

# Create target for all shaders (do not force into ALL; let the app depend on it)
add_custom_target(shaders DEPENDS ${SHADER_OUTPUTS})

# Copy compiled shaders to output directory
add_custom_command(TARGET shaders POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:${PROJECT_NAME}>/shaders
    COMMAND ${CMAKE_COMMAND} -E copy ${SHADER_OUTPUTS} $<TARGET_FILE_DIR:${PROJECT_NAME}>/shaders
)
