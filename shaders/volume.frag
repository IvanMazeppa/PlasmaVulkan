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
    uint frameIndex;       // Frame counter for STBN layer selection
    vec2 cpOffset;         // Cranley-Patterson offset for temporal jitter
    float _padding2;
} push;

// 3D density texture
layout(binding = 0) uniform sampler3D densityTexture;
layout(binding = 1) uniform sampler2DArray stbnTexture;
layout(binding = 2) uniform sampler1D opticalDepthLUT;

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

// Improved continuous LOD sampling with dual-level blending for smoother transitions
float sampleDensityContinuousLOD(vec3 worldPos, float lod) {
    vec3 texCoord = worldToTexture(worldPos);
    
    // Check if we're inside the volume bounds
    if (any(lessThan(texCoord, vec3(0.0))) || any(greaterThan(texCoord, vec3(1.0)))) {
        return 0.0;
    }
    
    // Compute discrete LOD levels and blend factor
    float lodLower = floor(lod);
    float lodUpper = ceil(lod);
    float blendFactor = fract(lod);
    
    // Sample at both LOD levels
    float densityLower = textureLod(densityTexture, texCoord, lodLower).r;
    float densityUpper = textureLod(densityTexture, texCoord, lodUpper).r;
    
    // Blend between the two samples for smooth LOD transitions
    float blendedDensity = mix(densityLower, densityUpper, blendFactor);
    
    return blendedDensity * push.densityScale;
}

// Legacy sample for gradient computation (always use highest detail)
float sampleDensity(vec3 worldPos) {
    return sampleDensityLOD(worldPos, 0.0);
}

// Estimate gradient for simple shading (optimized with LOD sampling)
vec3 densityGradient(vec3 worldPos) {
    float h = push.voxelSize; // step equal to voxel size
    // Sample gradient at LOD 1 to reduce texture fetches and denoise
    // This trades some accuracy for significant performance improvement (6 fetches → cheaper fetches)
    const float gradientLOD = 1.0;
    float dx = sampleDensityLOD(worldPos + vec3(h,0,0), gradientLOD) - sampleDensityLOD(worldPos - vec3(h,0,0), gradientLOD);
    float dy = sampleDensityLOD(worldPos + vec3(0,h,0), gradientLOD) - sampleDensityLOD(worldPos - vec3(0,h,0), gradientLOD);
    float dz = sampleDensityLOD(worldPos + vec3(0,0,h), gradientLOD) - sampleDensityLOD(worldPos - vec3(0,0,h), gradientLOD);
    return vec3(dx, dy, dz) / (2.0 * h);
}

// Henyey-Greenstein phase function for single scattering
float henyeyGreenstein(float cosTheta, float g) {
    float g2 = g * g;
    float denominator = 1.0 + g2 - 2.0 * g * cosTheta;
    return (1.0 - g2) / (4.0 * 3.14159265 * pow(denominator, 1.5));
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

    // Sample STBN (Spatiotemporal Blue Noise) for temporal jitter
    // Convert screen coordinates to STBN texture coordinates with Cranley-Patterson offset
    vec2 stbnUV = (fragCoord * 512.0) / 128.0 + push.cpOffset; // Scale screen to tile repeatedly
    float stbnValue = texture(stbnTexture, vec3(fract(stbnUV), float(push.frameIndex))).r;
    float blueNoiseJitter = (stbnValue - 0.5); // Center around 0 for symmetric jitter
    
    // Apply blue noise jitter to ray start with configurable scale
    float jitterScale = 0.8; // Jitter scale parameter (0.5-1.0 recommended)
    float t = t0 + blueNoiseJitter * jitterScale * push.stepSize;

    // Beer-Lambert Transmittance + Emission Model with Optical-Depth Adaptive Stepping
    vec3 radiance = vec3(0.0);    // Accumulated light
    float transmittance = 1.0;    // How much light passes through
    
    // Runtime adjustable extinction coefficient (NUM2 key)
    float sigma_t = push.opacityScale;  // Controls opacity/absorption
    
    // Optical-depth adaptive stepping parameters (tuned for dense plasma rings)
    float tauTarget = 0.04;              // Smaller target for dense regions
    float stepMin = push.voxelSize * 0.8; // Larger minimum to prevent oversampling
    float stepMax = push.voxelSize * 6.0;  // Smaller maximum to maintain detail
    float lodBias = 0.2;                  // Lower bias for sharper detail
    float densNormalThreshold = 0.01;     // Higher threshold for sparse shading
    
    // Adaptive marching state
    float lastStep = push.stepSize;       // Warm start
    float prevLod = 0.0;                  // LOD smoothing
    uint shadeStride = 0;                 // Gradient computation gating

    for (uint i = 0u; i < push.maxSteps && t < t1 && transmittance > 0.02; ++i) {
        vec3 pos = ro + rd * t;
        
        // Estimate step size for LOD calculation (warm start)
        float tentativeStep = clamp(lastStep, stepMin, stepMax);
        float lod = clamp(log2(tentativeStep / push.voxelSize) + lodBias, 0.0, 5.0);
        lod = mix(prevLod, lod, 0.7); // Smooth LOD transitions to avoid flicker
        
        // Sample density with improved continuous LOD for smoother transitions
        float dens = sampleDensityContinuousLOD(pos, lod);
        
        // Optical-depth-driven step size
        float currentStep = clamp(tauTarget / max(sigma_t * dens, 1e-4), stepMin, stepMax);
        
        // Small stochastic dither to break resonance patterns (use efficient hash for per-step)
        float stepJitter = 0.9 + 0.2 * fract(sin(dot(vec2(i, stbnValue * 127.0), vec2(12.9898,78.233))) * 43758.5453);
        currentStep *= stepJitter;

        // Preintegrated segment using optical depth LUT for Beer-Lambert smoothing
        float optical_depth = sigma_t * dens * currentStep;
        
        // Sample preintegrated LUT: f(τ) = (1 - exp(-τ)) / τ
        // LUT maps [0, 8] optical depth range to [0, 1] texture coordinates
        float lutCoord = clamp(optical_depth / 8.0, 0.0, 1.0);
        float preintegratedSegment = texture(opticalDepthLUT, lutCoord).r;
        
        // Local transmittance (for early exit and shading decisions)
        float local_transmittance = exp(-optical_depth);
        
        if (dens > 0.001) {
            // Color from density (emission) - reduce base emission to prevent overexposure
            vec3 emission = temperatureToColor(dens) * 0.3; // Scale down base emission
            
            // Gate expensive shading: only when meaningful density and periodically
            bool doExpensiveShading = (dens > densNormalThreshold) && 
                                    (shadeStride % 2u == 0u || local_transmittance < 0.99);
            
            if (doExpensiveShading) {
                // Physically-based single scattering with Henyey-Greenstein phase function
                vec3 lightDir = normalize(vec3(-0.5, -0.8, -0.6)); // Light from upper-left-front
                vec3 lightColor = vec3(0.8, 0.7, 0.6); // Dimmer, warmer light to prevent overexposure
                
                // Compute scattering phase function
                float cosTheta = dot(-rd, lightDir); // Angle between ray and light
                float g = 0.75; // Forward scattering parameter (0.6-0.85 for plasma)
                float phase = henyeyGreenstein(cosTheta, g);
                
                // Add single scattering contribution with density-based modulation
                float scatteringStrength = 0.4 * (1.0 - clamp(dens * 2.0, 0.0, 0.8)); // Reduce in dense regions
                vec3 scatteredLight = lightColor * phase * scatteringStrength;
                emission += scatteredLight;
                
                // Edge enhancement with gated gradient computation (expensive: 6 texture fetches)
                vec3 N = normalize(densityGradient(pos) + 1e-5);
                float edgeEnhancement = clamp(dot(-rd, N) * 0.2 + 0.8, 0.6, 1.1); // Gentler enhancement
                emission *= edgeEnhancement;
            }
            
            emission *= push.emissionScale; // Runtime adjustable emission (NUM4 key)
            
            // Preintegrated emission using LUT for smooth Beer-Lambert integration
            vec3 integrated_emission = emission * dens * currentStep * preintegratedSegment;
            
            // Accumulate radiance attenuated by transmittance
            radiance += transmittance * integrated_emission;
        }
        
        // Update transmittance for next segment
        transmittance *= local_transmittance;

        t += currentStep;
        
        // Update adaptive marching state
        shadeStride++;
        prevLod = lod;
        lastStep = currentStep;
        
        // Subgroup-coherent early exit optimization
        // If all invocations in this subgroup have reached the opacity threshold, exit early
        // This reduces warp divergence and improves performance on GPUs
        if (i > 8u && subgroupAll(transmittance <= 0.02)) {
            break;  // Entire subgroup is opaque, early termination
        }
    }
    
    // Final opacity from transmittance
    float alpha = 1.0 - transmittance;
    
    fragColor = vec4(radiance, alpha);
}
