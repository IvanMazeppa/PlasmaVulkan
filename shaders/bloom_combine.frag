#version 450

// Combine original scene with bloom glow
// Final plasma energy composition

layout(location = 0) in vec2 texCoord;
layout(location = 0) out vec4 fragColor;

layout(binding = 0) uniform sampler2D sceneTexture;  // Original particles
layout(binding = 1) uniform sampler2D bloomTexture;  // Blurred glow

layout(push_constant) uniform PushConstants {
    float bloomStrength;    // Glow intensity (e.g., 0.8)
    float exposure;         // HDR exposure for plasma brightness
    float gamma;            // Gamma correction (usually 2.2)
    float _padding;
} push;

// HDR tone mapping for intense plasma energy
vec3 tonemap(vec3 color) {
    // ACES tone mapping for cinematic plasma look
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    
    return clamp((color * (a * color + b)) / (color * (c * color + d) + e), 0.0, 1.0);
}

void main() {
    vec3 sceneColor = texture(sceneTexture, texCoord).rgb;
    vec3 bloomColor = texture(bloomTexture, texCoord).rgb;
    
    // Combine original particles with bloom glow
    vec3 combined = sceneColor + bloomColor * push.bloomStrength;
    
    // Apply HDR exposure for intense plasma brightness
    combined *= push.exposure;
    
    // Tone mapping to handle bright plasma colors
    combined = tonemap(combined);
    
    // Gamma correction
    combined = pow(combined, vec3(1.0 / push.gamma));
    
    fragColor = vec4(combined, 1.0);
}