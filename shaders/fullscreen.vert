#version 450

// Fullscreen triangle vertex shader for post-processing
// Efficient single-triangle technique

layout(location = 0) out vec2 texCoord;

void main() {
    // Generate fullscreen triangle using vertex ID
    // Triangle covers entire screen with minimal overhead
    texCoord = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(texCoord * 2.0 - 1.0, 0.0, 1.0);
}