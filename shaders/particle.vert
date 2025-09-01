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
    float _padding[3];
} push;

void main() {
    // Transform position
    gl_Position = push.viewProj * vec4(inPosition, 1.0);
    
    // Set point size based on distance and particle size
    float distance = length(inPosition);
    gl_PointSize = push.particleSize * (20.0 / (distance + 1.0)); // Larger base size
    gl_PointSize = clamp(gl_PointSize, 2.0, 20.0); // Bigger range
    
    // ORBITAL VELOCITY-BASED TEMPERATURE MODEL
    // Use persistent temperature from compute shader instead of distance-based calculation
    float normalizedTemp = clamp(inTemperature, 0.0, 1.0);
    
    // Blackbody radiation color mapping - proper physics-based spectrum
    // Based on 2025 research: stable temperature tracking prevents white-out
    if (normalizedTemp < 0.1) {
        // Very cool: Deep red-brown (outer disk regions)
        fragColor = vec3(0.4, 0.05, 0.0);
    } else if (normalizedTemp < 0.2) {
        // Cool: Red (stable orbital regions)
        fragColor = vec3(0.8, 0.1, 0.0);
    } else if (normalizedTemp < 0.35) {
        // Moderate cool: Red-orange (mid-disk)
        fragColor = vec3(1.0, 0.25, 0.0);
    } else if (normalizedTemp < 0.5) {
        // Moderate: Orange (active regions)
        fragColor = vec3(1.0, 0.5, 0.05);
    } else if (normalizedTemp < 0.65) {
        // Moderate hot: Yellow-orange (high shear zones)
        fragColor = vec3(1.0, 0.7, 0.1);
    } else if (normalizedTemp < 0.8) {
        // Hot: Yellow (viscous heating zones)
        fragColor = vec3(1.0, 0.9, 0.2);
    } else {
        // Very hot: Yellow-white (extreme shear/viscous dissipation)
        fragColor = vec3(1.0, 0.95, 0.6);
    }
    
    // Stable intensity based on temperature
    fragIntensity = 0.3 + normalizedTemp * 0.6;
}
