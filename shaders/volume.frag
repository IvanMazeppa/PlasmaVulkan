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

// Dramatically cooler color temperature mapping for stable visuals
vec3 blackbodyColor(float temperature) {
    // MUCH more conservative temperature scaling
    float t = clamp(temperature * 5.0, 0.0, 1.0); // Amplify the weak signal
    
    vec3 color;
    
    // Extended color range with emphasis on cooler colors
    if (t < 0.2) {
        // Very cold - nearly black to dark red
        color = vec3(0.05, 0.0, 0.0) * t / 0.2;
    } else if (t < 0.35) {
        // Cold - dark red to red
        float factor = (t - 0.2) / 0.15;
        color = mix(vec3(0.05, 0.0, 0.0), vec3(0.4, 0.05, 0.0), factor);
    } else if (t < 0.5) {
        // Cool - red to red-orange
        float factor = (t - 0.35) / 0.15;
        color = mix(vec3(0.4, 0.05, 0.0), vec3(0.7, 0.15, 0.0), factor);
    } else if (t < 0.65) {
        // Moderate - red-orange to orange
        float factor = (t - 0.5) / 0.15;
        color = mix(vec3(0.7, 0.15, 0.0), vec3(0.9, 0.3, 0.0), factor);
    } else if (t < 0.75) {
        // Warm - orange to yellow-orange
        float factor = (t - 0.65) / 0.1;
        color = mix(vec3(0.9, 0.3, 0.0), vec3(1.0, 0.5, 0.1), factor);
    } else if (t < 0.85) {
        // Hot - yellow-orange to yellow
        float factor = (t - 0.75) / 0.1;
        color = mix(vec3(1.0, 0.5, 0.1), vec3(1.0, 0.8, 0.2), factor);
    } else if (t < 0.95) {
        // Very hot - yellow to yellow-white
        float factor = (t - 0.85) / 0.1;
        color = mix(vec3(1.0, 0.8, 0.2), vec3(1.0, 0.95, 0.6), factor);
    } else {
        // Extremely hot - yellow-white (rarely reached)
        color = vec3(1.0, 0.95, 0.6);
    }
    
    // Much gentler intensity with a dim base
    float intensity = 0.1 + t * 0.5; // Dimmer overall
    
    return color * intensity;
}

// Radial temperature model for volume rendering
vec3 temperatureToColor(float density) {
    if (density < 0.01) return vec3(0.0);
    
    // Use density as temperature proxy but with radial enhancement
    // Scale density to match particle radial model
    float enhancedDensity = density * 2.0; // Boost for visibility
    
    return blackbodyColor(enhancedDensity);
}

// Alternative: Direct radial temperature (if we can pass world position)
vec3 radialTemperatureColor(vec3 worldPos, float density) {
    if (density < 0.01) return vec3(0.0);
    
    float distanceFromCenter = length(worldPos);
    float radialTemp = 10.0 / pow(max(distanceFromCenter, 1.0), 0.75);
    float normalizedTemp = clamp(radialTemp * 0.3, 0.0, 1.0);
    
    // Same color scheme as particle shader
    vec3 veryOuterColor = vec3(0.05, 0.0, 0.0);
    vec3 outerColor = vec3(0.6, 0.0, 0.0);
    vec3 midOuterColor = vec3(0.9, 0.1, 0.0);
    vec3 midColor = vec3(1.0, 0.3, 0.0);
    vec3 midInnerColor = vec3(1.0, 0.6, 0.0);
    vec3 innerColor = vec3(1.0, 0.9, 0.1);
    vec3 veryInnerColor = vec3(1.0, 1.0, 0.8);
    
    vec3 color;
    if (normalizedTemp < 0.1) {
        color = mix(veryOuterColor, outerColor, normalizedTemp * 10.0);
    } else if (normalizedTemp < 0.25) {
        color = mix(outerColor, midOuterColor, (normalizedTemp - 0.1) * 6.67);
    } else if (normalizedTemp < 0.4) {
        color = mix(midOuterColor, midColor, (normalizedTemp - 0.25) * 6.67);
    } else if (normalizedTemp < 0.55) {
        color = mix(midColor, midInnerColor, (normalizedTemp - 0.4) * 6.67);
    } else if (normalizedTemp < 0.7) {
        color = mix(midInnerColor, innerColor, (normalizedTemp - 0.55) * 6.67);
    } else if (normalizedTemp < 0.85) {
        color = mix(innerColor, veryInnerColor, (normalizedTemp - 0.7) * 6.67);
    } else {
        color = veryInnerColor;
    }
    
    float intensity = 0.1 + normalizedTemp * 1.2;
    return color * intensity * density; // Modulate by density
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