#version 450

// Volumetric ray marching fragment shader
// Renders 3D density grid as glowing plasma

layout(location = 0) in vec2 fragCoord; // Screen coordinates [0,1]
layout(location = 0) out vec4 fragColor;

// Push constants for ray marching parameters
layout(push_constant) uniform PushConstants {
    mat4 viewProjInv;      // Inverse view-projection matrix for ray generation
    vec3 cameraPos;        // Camera position in world space
    float _padding1;
    vec3 gridOrigin;       // Volume grid origin
    float voxelSize;       // Size of each voxel
    uvec3 gridDimensions;  // Grid resolution
    uint maxSteps;         // Maximum ray marching steps
    float stepSize;        // Ray marching step size
    float densityScale;    // Scale factor for density visualization
    float _padding2;
    float _padding3;
} push;

// 3D density texture
layout(binding = 0) uniform sampler3D densityTexture;

// Generate world space ray from screen coordinate
vec3 getRayDirection(vec2 screenPos) {
    // Convert screen coordinates to NDC (-1 to 1)
    vec4 ndc = vec4(screenPos * 2.0 - 1.0, 1.0, 1.0);
    
    // Transform to world space
    vec4 worldPos = push.viewProjInv * ndc;
    worldPos /= worldPos.w;
    
    return normalize(worldPos.xyz - push.cameraPos);
}

// Convert world position to texture coordinates [0,1]
vec3 worldToTexture(vec3 worldPos) {
    vec3 localPos = worldPos - push.gridOrigin;
    vec3 gridPos = localPos / (push.voxelSize * vec3(push.gridDimensions));
    return clamp(gridPos, 0.0, 1.0);
}

// Sample density at world position
float sampleDensity(vec3 worldPos) {
    vec3 texCoord = worldToTexture(worldPos);
    
    // Check if we're inside the volume bounds
    if (any(lessThan(texCoord, vec3(0.0))) || any(greaterThan(texCoord, vec3(1.0)))) {
        return 0.0;
    }
    
    return texture(densityTexture, texCoord).r * push.densityScale;
}

// Simple temperature-based color mapping
vec3 temperatureToColor(float density) {
    if (density < 0.01) return vec3(0.0);
    
    // Plasma color palette based on density/temperature
    float t = clamp(density, 0.0, 1.0);
    
    // Cool to hot: dark blue → blue → cyan → yellow → orange → white
    if (t < 0.2) {
        return mix(vec3(0.0, 0.0, 0.1), vec3(0.0, 0.2, 0.8), t * 5.0);
    } else if (t < 0.4) {
        return mix(vec3(0.0, 0.2, 0.8), vec3(0.0, 0.8, 1.0), (t - 0.2) * 5.0);
    } else if (t < 0.6) {
        return mix(vec3(0.0, 0.8, 1.0), vec3(0.8, 1.0, 0.2), (t - 0.4) * 5.0);
    } else if (t < 0.8) {
        return mix(vec3(0.8, 1.0, 0.2), vec3(1.0, 0.6, 0.0), (t - 0.6) * 5.0);
    } else {
        return mix(vec3(1.0, 0.6, 0.0), vec3(1.0, 1.0, 1.0), (t - 0.8) * 5.0);
    }
}

void main() {
    // Generate ray from camera through this pixel
    vec3 rayDir = getRayDirection(fragCoord);
    vec3 rayPos = push.cameraPos;
    
    // Ray marching accumulation
    vec3 color = vec3(0.0);
    float alpha = 0.0;
    
    // March through the volume
    for (uint step = 0; step < push.maxSteps; step++) {
        // Sample density at current position
        float density = sampleDensity(rayPos);
        
        if (density > 0.005) {
            // Calculate color and opacity contribution
            vec3 sampleColor = temperatureToColor(density);
            float sampleAlpha = density * 0.05; // Balanced opacity for plasma effect
            
            // Alpha blending (front-to-back)
            color += sampleColor * sampleAlpha * (1.0 - alpha);
            alpha += sampleAlpha * (1.0 - alpha);
            
            // Early termination if sufficiently opaque
            if (alpha > 0.8) break;
        }
        
        // Step forward along the ray
        rayPos += rayDir * push.stepSize;
    }
    
    // Output final color
    fragColor = vec4(color, alpha);
}