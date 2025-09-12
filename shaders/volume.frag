#version 450
#extension GL_KHR_shader_subgroup_ballot : enable
#extension GL_KHR_shader_subgroup_vote : enable

// Volumetric ray marching fragment shader with subgroup-coherent early exit
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
    float opacityScale;    // Sigma_t scaling for opacity/absorption
    float emissionScale;   // Emission intensity scaling
    float tempOffset;      // Temperature offset for color shift
    float tempRange;       // Temperature range compression/expansion
    float saturation;      // Color saturation control
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

// Compute AABB intersection with the volume grid
bool intersectVolume(vec3 ro, vec3 rd, out float tmin, out float tmax) {
    vec3 boxMin = push.gridOrigin;
    vec3 boxMax = push.gridOrigin + vec3(push.voxelSize) * vec3(push.gridDimensions);
    vec3 invD = 1.0 / rd;
    vec3 t0 = (boxMin - ro) * invD;
    vec3 t1 = (boxMax - ro) * invD;
    vec3 tsmaller = min(t0, t1);
    vec3 tbigger  = max(t0, t1);
    tmin = max(0.0, max(max(tsmaller.x, tsmaller.y), tsmaller.z));
    tmax = min(tbigger.x, min(tbigger.y, tbigger.z));
    return tmax > tmin;
}

// Convert world position to texture coordinates [0,1]
vec3 worldToTexture(vec3 worldPos) {
    vec3 localPos = worldPos - push.gridOrigin;
    vec3 gridPos = localPos / (push.voxelSize * vec3(push.gridDimensions));
    return clamp(gridPos, 0.0, 1.0);
}

// Sample density at world position
// Sample density texture with LOD for cone-stepped raymarch
float sampleDensityLOD(vec3 worldPos, float lod) {
    vec3 texCoord = worldToTexture(worldPos);
    
    // Check if we're inside the volume bounds
    if (any(lessThan(texCoord, vec3(0.0))) || any(greaterThan(texCoord, vec3(1.0)))) {
        return 0.0;
    }
    
    // Use textureLod for manual mip level selection (cone-stepped optimization)
    return textureLod(densityTexture, texCoord, lod).r * push.densityScale;
}

// Legacy sample for gradient computation (always use highest detail)
float sampleDensity(vec3 worldPos) {
    return sampleDensityLOD(worldPos, 0.0);
}

// Estimate gradient for simple shading
vec3 densityGradient(vec3 worldPos) {
    float h = push.voxelSize; // step equal to voxel size
    float dx = sampleDensity(worldPos + vec3(h,0,0)) - sampleDensity(worldPos - vec3(h,0,0));
    float dy = sampleDensity(worldPos + vec3(0,h,0)) - sampleDensity(worldPos - vec3(0,h,0));
    float dz = sampleDensity(worldPos + vec3(0,0,h)) - sampleDensity(worldPos - vec3(0,0,h));
    return vec3(dx, dy, dz) / (2.0 * h);
}

// Improved plasma color with temperature remapping controls
vec3 plasmaColor(float temperature) {
    // Apply temperature remapping controls (NUM6-7)
    // tempOffset shifts the whole gradient (-0.5 = more red, +0.5 = more yellow)
    // tempRange compresses/expands the gradient (0.5 = compressed, 2.0 = expanded)
    float t = clamp((temperature + push.tempOffset) * push.tempRange, 0.0, 1.0);
    
    vec3 color;
    
    // Pure red-orange-yellow-white plasma progression
    if (t < 0.2) {
        // Deep red to bright red
        float factor = t / 0.2;
        color = mix(vec3(0.4, 0.0, 0.0), vec3(0.8, 0.0, 0.0), factor);
    } else if (t < 0.35) {
        // Bright red to red-orange
        float factor = (t - 0.2) / 0.15;
        color = mix(vec3(0.8, 0.0, 0.0), vec3(1.0, 0.2, 0.0), factor);
    } else if (t < 0.5) {
        // Red-orange to pure orange
        float factor = (t - 0.35) / 0.15;
        color = mix(vec3(1.0, 0.2, 0.0), vec3(1.0, 0.5, 0.0), factor);
    } else if (t < 0.65) {
        // Orange to yellow-orange
        float factor = (t - 0.5) / 0.15;
        color = mix(vec3(1.0, 0.5, 0.0), vec3(1.0, 0.7, 0.0), factor);
    } else if (t < 0.8) {
        // Yellow-orange to bright yellow
        float factor = (t - 0.65) / 0.15;
        color = mix(vec3(1.0, 0.7, 0.0), vec3(1.0, 0.95, 0.0), factor);
    } else if (t < 0.98) {
        // Yellow to warm white
        float factor = (t - 0.8) / 0.18;
        color = mix(vec3(1.0, 0.95, 0.0), vec3(1.0, 1.0, 0.85), factor);
    } else {
        // White-hot core
        color = vec3(1.0, 1.0, 0.9);
    }
    
    // Apply saturation control (NUM8)
    // Desaturate by mixing with luminance
    float lum = dot(color, vec3(0.299, 0.587, 0.114)); // Standard luminance weights
    color = mix(vec3(lum), color, push.saturation);
    
    // Moderate intensity for volumetric rendering
    float intensity = 0.5 + t * 0.8;
    
    return color * intensity;
}

// Enhanced plasma temperature model for volumetric rendering
vec3 temperatureToColor(float density) {
    if (density < 0.001) return vec3(0.0);
    
    // Runtime adjustable density scaling (NUM1 key)
    float enhancedDensity = density * push.densityScale;
    
    return plasmaColor(enhancedDensity);
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
    // Generate ray
    vec3 rd = getRayDirection(fragCoord);
    vec3 ro = push.cameraPos;

    // Intersect with volume bounds
    float t0, t1;
    if (!intersectVolume(ro, rd, t0, t1)) {
        fragColor = vec4(0.0);
        return;
    }

    // Jitter start to reduce banding (hash on pixel)
    float hash = fract(sin(dot(fragCoord, vec2(12.9898,78.233))) * 43758.5453);
    float t = t0 + hash * push.stepSize;

    // Beer-Lambert Transmittance + Emission Model
    // Physically correct opacity accumulation that prevents overbright saturation
    vec3 radiance = vec3(0.0);    // Accumulated light
    float transmittance = 1.0;    // How much light passes through
    
    // Runtime adjustable extinction coefficient (NUM2 key)
    float sigma_t = push.opacityScale;  // Controls opacity/absorption
    
    // Adaptive step parameters
    float baseStep = push.stepSize;

    for (uint i = 0u; i < push.maxSteps && t < t1 && transmittance > 0.01; ++i) {
        vec3 pos = ro + rd * t;
        float dens = sampleDensity(pos);

        // Empty-space skipping: take larger steps when empty
        float stepMul = (dens < 0.002) ? 4.0 : ((dens < 0.01) ? 2.0 : 1.0);
        float currentStep = baseStep * stepMul;

        if (dens > 0.001) {
            // Color from density (emission)
            vec3 emission = temperatureToColor(dens);
            
            // Simple edge lighting using gradient and view dir
            vec3 N = normalize(densityGradient(pos) + 1e-5);
            float viewDot = clamp(dot(-rd, N) * 0.5 + 0.5, 0.0, 1.0);
            emission *= mix(0.8, 1.3, viewDot); // emphasize thin features
            emission *= push.emissionScale; // Runtime adjustable emission (NUM4 key)
            
            // Beer-Lambert transmittance update
            float optical_depth = sigma_t * dens * currentStep;
            float local_transmittance = exp(-optical_depth);
            
            // Integrate emission along the ray segment
            // This accounts for absorption within the segment itself
            vec3 segment_radiance = emission * dens * (1.0 - local_transmittance) / max(sigma_t * dens, 0.001);
            
            // Accumulate radiance attenuated by transmittance
            radiance += transmittance * segment_radiance;
            
            // Update transmittance for next segment
            transmittance *= local_transmittance;
        }

        t += currentStep;
        
        // Subgroup-coherent early exit optimization
        // If all invocations in this subgroup have reached the opacity threshold, exit early
        // This reduces warp divergence and improves performance on GPUs
        if (i > 8u && subgroupAll(transmittance <= 0.01)) {
            break;  // Entire subgroup is opaque, early termination
        }
    }
    
    // Final opacity from transmittance
    float alpha = 1.0 - transmittance;
    
    fragColor = vec4(radiance, alpha);
}
