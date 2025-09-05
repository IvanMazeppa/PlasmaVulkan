#version 450

// Input from vertex buffer
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inVelocity;
layout(location = 2) in float inTemperature;

// Output to fragment shader
layout(location = 0) out vec3 fragColor;
layout(location = 1) out float fragIntensity;

// Push constants
layout(push_constant) uniform PushConstants {
    mat4 viewProj;
    float particleSize;
    float softParticleFactor;
    vec2 screenSize;
} push;

void main() {
    // Transform position
    gl_Position = push.viewProj * vec4(inPosition, 1.0);
    
    // Set point size based on distance and particle size - enhanced for volumetric effect
    float distance = length(inPosition);
    gl_PointSize = push.particleSize * (25.0 / (distance + 1.0)); // Much larger for volume
    gl_PointSize = clamp(gl_PointSize, 4.0, 40.0); // Bigger range for smooth overlap
    
    // ORBITAL VELOCITY-BASED TEMPERATURE MODEL
    // Use persistent temperature from compute shader instead of distance-based calculation
    float normalizedTemp = clamp(inTemperature, 0.0, 1.0);
    
    // Enhanced blackbody radiation color mapping - from cool to hot
    if (normalizedTemp < 0.15) {
        // Very cool: Deep blue (new for galaxy collision visualization)
        fragColor = vec3(0.1, 0.3, 0.9);
    } else if (normalizedTemp < 0.3) {
        // Cool: Cyan-blue
        fragColor = vec3(0.2, 0.6, 1.0);
    } else if (normalizedTemp < 0.45) {
        // Moderate cool: Cyan
        fragColor = vec3(0.3, 0.8, 0.9);
    } else if (normalizedTemp < 0.6) {
        // Moderate: White-yellow
        fragColor = vec3(0.9, 0.9, 0.6);
    } else if (normalizedTemp < 0.75) {
        // Moderate hot: Yellow-orange
        fragColor = vec3(1.0, 0.7, 0.1);
    } else if (normalizedTemp < 0.88) {
        // Hot: Orange
        fragColor = vec3(1.0, 0.5, 0.05);
    } else if (normalizedTemp < 0.96) {
        // Very hot: Red-orange
        fragColor = vec3(1.0, 0.25, 0.0);
    } else {
        // WHITE HOT: Pure red (EXTREME conditions only)
        fragColor = vec3(1.0, 0.1, 0.0);
    }
    
    // Stable intensity based on temperature
    fragIntensity = 0.3 + normalizedTemp * 0.6;
}
