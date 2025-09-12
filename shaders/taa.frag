#version 450

// TAA (Temporal Anti-Aliasing) fragment shader for volumetric noise reduction
// Implements reprojection with neighborhood clamping to prevent ghosting

layout(location = 0) in vec2 fragCoord;
layout(location = 0) out vec4 fragColor;

// Push constants for TAA parameters
layout(push_constant) uniform TAAConstants {
    mat4 currentToHistory;    // Reprojection matrix: previous_viewProj * inverse(current_viewProj)
    vec2 screenSize;          // Screen dimensions for texel size calculations
    float blendFactor;        // Temporal blend factor (typically 0.1 for volumetrics)
    uint firstFrame;          // 1 if first frame (skip history), 0 otherwise
} taa;

// Input textures
layout(binding = 0) uniform sampler2D currentFrame;  // Current volumetric render result
layout(binding = 1) uniform sampler2D historyFrame;  // Previous frame history

// 3x3 neighborhood clamping to prevent ghosting artifacts
vec3 neighborhoodClamp(vec3 historyColor, vec2 uv, vec2 texelSize) {
    // Sample 3x3 neighborhood around current pixel
    vec3 colorSum = vec3(0.0);
    vec3 colorMin = vec3(1e10);
    vec3 colorMax = vec3(-1e10);
    
    // Sample 3x3 neighborhood
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            vec2 offset = vec2(float(x), float(y)) * texelSize;
            vec3 neighborColor = texture(currentFrame, uv + offset).rgb;
            
            colorSum += neighborColor;
            colorMin = min(colorMin, neighborColor);
            colorMax = max(colorMax, neighborColor);
        }
    }
    
    // Clamp history color to neighborhood bounds
    return clamp(historyColor, colorMin, colorMax);
}

void main() {
    vec2 texelSize = 1.0 / taa.screenSize;
    
    // Sample current frame
    vec4 currentColor = texture(currentFrame, fragCoord);
    
    // Handle first frame - no history available
    if (taa.firstFrame != 0u) {
        fragColor = currentColor;
        return;
    }
    
    // Reproject current pixel to history position
    vec4 ndcPos = vec4(fragCoord * 2.0 - 1.0, 0.0, 1.0);
    vec4 historyNDC = taa.currentToHistory * ndcPos;
    vec2 historyUV = (historyNDC.xy / historyNDC.w) * 0.5 + 0.5;
    
    // Check if reprojected position is within screen bounds
    if (historyUV.x < 0.0 || historyUV.x > 1.0 || historyUV.y < 0.0 || historyUV.y > 1.0) {
        // Outside screen bounds - use current frame only
        fragColor = currentColor;
        return;
    }
    
    // Sample history at reprojected position
    vec3 historyColor = texture(historyFrame, historyUV).rgb;
    
    // Apply neighborhood clamping to prevent ghosting
    vec3 clampedHistory = neighborhoodClamp(historyColor, fragCoord, texelSize);
    
    // Temporal blend with volumetric-optimized blend factor
    vec3 blendedColor = mix(clampedHistory, currentColor.rgb, taa.blendFactor);
    
    // Output with current frame alpha (preserve transparency)
    fragColor = vec4(blendedColor, currentColor.a);
}