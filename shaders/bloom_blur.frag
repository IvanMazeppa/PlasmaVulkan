#version 450

// Gaussian blur for bloom glow effect
// Creates smooth plasma energy field

layout(location = 0) in vec2 texCoord;
layout(location = 0) out vec4 fragColor;

layout(binding = 0) uniform sampler2D inputTexture;

layout(push_constant) uniform PushConstants {
    vec2 direction;         // Blur direction (1,0) for horizontal, (0,1) for vertical
    float strength;         // Blur strength/radius
    float _padding;
} push;

void main() {
    vec2 texelSize = 1.0 / textureSize(inputTexture, 0);
    vec3 result = vec3(0.0);
    
    // 9-tap Gaussian blur with optimized weights
    float weights[5] = float[](0.227027, 0.1945946, 0.1216216, 0.054054, 0.016216);
    
    // Center sample
    result += texture(inputTexture, texCoord).rgb * weights[0];
    
    // Symmetrical sampling in specified direction
    for(int i = 1; i < 5; i++) {
        vec2 offset = push.direction * texelSize * i * push.strength;
        
        result += texture(inputTexture, texCoord + offset).rgb * weights[i];
        result += texture(inputTexture, texCoord - offset).rgb * weights[i];
    }
    
    fragColor = vec4(result, 1.0);
}