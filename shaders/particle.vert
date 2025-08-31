#version 450

// Input from vertex buffer
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inVelocity;

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
    
    // Enhanced color mapping for wider velocity range visualization
    float speed = length(inVelocity);
    float normalizedSpeed = clamp(speed * 0.15, 0.0, 1.0); // Much lower sensitivity to spread colors
    
    // Extended plasma spectrum with more color bands
    vec3 verySlowColor = vec3(0.2, 0.0, 0.8);  // Deep blue (very slow)
    vec3 slowColor = vec3(0.0, 0.6, 1.0);      // Cyan (slow)  
    vec3 mediumColor = vec3(0.4, 1.0, 0.6);    // Green (medium)
    vec3 fastColor = vec3(1.0, 0.8, 0.0);      // Yellow (fast)
    vec3 veryFastColor = vec3(1.0, 0.4, 0.0);  // Orange (very fast)
    vec3 extremeColor = vec3(1.0, 0.2, 0.2);   // Red (extreme speed)
    
    // 6-band color spectrum for better velocity visualization
    if (normalizedSpeed < 0.2) {
        fragColor = mix(verySlowColor, slowColor, normalizedSpeed * 5.0);
    } else if (normalizedSpeed < 0.4) {
        fragColor = mix(slowColor, mediumColor, (normalizedSpeed - 0.2) * 5.0);
    } else if (normalizedSpeed < 0.6) {
        fragColor = mix(mediumColor, fastColor, (normalizedSpeed - 0.4) * 5.0);
    } else if (normalizedSpeed < 0.8) {
        fragColor = mix(fastColor, veryFastColor, (normalizedSpeed - 0.6) * 5.0);
    } else {
        fragColor = mix(veryFastColor, extremeColor, (normalizedSpeed - 0.8) * 5.0);
    }
    
    // Enhanced intensity for more vibrant glow effect
    fragIntensity = 0.7 + normalizedSpeed * 0.8; // Brighter base intensity
}
