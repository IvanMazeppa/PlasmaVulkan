#version 450

// Bright pass filter for bloom effect
// Extracts bright pixels for plasma glow

layout(location = 0) in vec2 texCoord;
layout(location = 0) out vec4 fragColor;

layout(binding = 0) uniform sampler2D sceneTexture;

layout(push_constant) uniform PushConstants {
    float threshold;        // Brightness threshold (e.g., 1.0)
    float intensity;        // Glow intensity multiplier (e.g., 2.0)
    vec2 _padding;
} push;

void main() {
    vec3 color = texture(sceneTexture, texCoord).rgb;
    
    // Calculate luminance for brightness detection
    float luminance = dot(color, vec3(0.2126, 0.7152, 0.0722));
    
    // Extract bright pixels above threshold
    if (luminance > push.threshold) {
        // Boost bright pixels for intense plasma glow
        float boost = (luminance - push.threshold) * push.intensity + 1.0;
        fragColor = vec4(color * boost, 1.0);
    } else {
        // Dark pixels contribute no glow
        fragColor = vec4(0.0, 0.0, 0.0, 1.0);
    }
}