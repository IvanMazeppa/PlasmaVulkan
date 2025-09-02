#version 450

// Input from vertex shader
layout(location = 0) in vec3 fragColor;
layout(location = 1) in float fragIntensity;

// Depth buffer for soft particles (optional - can be disabled)
// layout(binding = 0) uniform sampler2D depthTexture;

// Push constants for soft particle parameters
layout(push_constant) uniform PushConstants {
    mat4 viewProj;
    float particleSize;
    float softParticleFactor; // Controls how soft the blending is
    vec2 screenSize;          // For depth texture coordinate calculation
} push;

// Output
layout(location = 0) out vec4 outColor;

void main() {
    // Calculate distance from center of point sprite
    vec2 coord = gl_PointCoord - vec2(0.5);
    float dist = length(coord);
    
    // Soft circular falloff
    if (dist > 0.5) {
        discard;
    }
    
    // VOLUMETRIC ENHANCEMENT (soft particles disabled for now)
    // Future: implement depth buffer sampling for true soft particles
    float softFade = 1.0; // No soft fade for now
    
    // Enhanced Gaussian falloff for volumetric appearance
    float gaussianFalloff = exp(-dist * dist * 3.0);
    
    // ADVANCED MULTI-LAYER VOLUMETRIC BLENDING
    // Create multiple overlapping layers for smooth, cloud-like appearance
    float outerHalo = exp(-dist * dist * 1.0);   // Very soft outer glow
    float midLayer = exp(-dist * dist * 4.0);    // Medium density
    float innerCore = exp(-dist * dist * 12.0);  // Dense inner region
    float brightCore = exp(-dist * dist * 30.0); // Bright center
    
    // Layer weights for smooth blending - emphasis on outer layers for volume
    float volumetricAlpha = (outerHalo * 0.15 +     // Subtle outer volume
                           midLayer * 0.35 +        // Main volume contribution
                           innerCore * 0.4 +        // Dense regions
                           brightCore * 0.1) *      // Bright highlights
                          fragIntensity;
    
    // Apply soft particle fade
    volumetricAlpha *= softFade;
    
    // PYRO-STYLE BRIGHTNESS ACCUMULATION
    // White hot cores get extreme brightness boost
    float temperatureIntensity = fragIntensity * fragIntensity; // Quadratic boost for high temps
    float whiteHotBoost = smoothstep(0.8, 1.0, fragIntensity) * 2.0; // Extra boost for white regions
    
    // Churning texture effect with temperature-based variation
    float textureNoise = fract(sin(dot(gl_FragCoord.xy, vec2(12.9898, 78.233))) * 43758.5453);
    float churnVariation = 0.8 + 0.4 * textureNoise * (1.0 + temperatureIntensity);
    
    // DENSITY ACCUMULATION COLOR BOOST
    // High-temperature particles get massive brightness increase
    vec3 volumetricColor = fragColor;
    
    // CONSERVATIVE BRIGHTNESS - rare white hot cores only
    if (fragIntensity > 0.9) {
        volumetricColor *= (2.5 + whiteHotBoost); // Extreme brightness for rare white cores
    } else if (fragIntensity > 0.8) {
        volumetricColor *= (2.0 + temperatureIntensity * 0.8); // Yellow-white regions
    } else if (fragIntensity > 0.65) {
        volumetricColor *= (1.6 + temperatureIntensity * 0.6); // Hot yellow regions
    } else {
        volumetricColor *= (1.4 + fragIntensity * 0.4); // Normal regions - more conservative
    }
    
    // Apply churning variation
    volumetricColor *= churnVariation;
    
    // Multi-layer brightness accumulation (pyro technique)
    float coreDensity = brightCore + innerCore * 0.5;
    float layerBrightness = 1.0 + innerCore * 0.4 + brightCore * 1.2 + coreDensity * whiteHotBoost;
    vec3 finalColor = volumetricColor * layerBrightness;
    
    // Output with enhanced volumetric appearance
    outColor = vec4(finalColor * volumetricAlpha * 1.8, volumetricAlpha * 0.8);
}
