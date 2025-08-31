#version 450

// Input from vertex shader
layout(location = 0) in vec3 fragColor;
layout(location = 1) in float fragIntensity;

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
    
    // Gaussian-like falloff for glow effect
    float alpha = exp(-dist * dist * 4.0) * fragIntensity; // Softer falloff for more glow
    
    // Add inner bright core with color saturation boost
    float coreBrightness = exp(-dist * dist * 16.0);
    vec3 saturatedColor = fragColor * 2.5; // Increased color saturation
    vec3 finalColor = saturatedColor + vec3(coreBrightness * 0.5); // Bright core but preserve color
    
    // Output with enhanced brightness for vibrant plasma effect
    outColor = vec4(finalColor * alpha * 3.0, alpha); // Triple brightness for stunning visuals
}
