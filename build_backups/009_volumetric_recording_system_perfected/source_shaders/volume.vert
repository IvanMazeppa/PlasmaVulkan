#version 450

// Fullscreen quad vertex shader for volumetric rendering
// Generates a triangle that covers the entire screen

layout(location = 0) out vec2 fragCoord;

void main() {
    // Generate fullscreen triangle vertices
    // Vertex 0: (-1, -1)  bottom-left
    // Vertex 1: (3, -1)   bottom-right (extends beyond screen)
    // Vertex 2: (-1, 3)   top-left (extends beyond screen)
    
    float x = -1.0 + float((gl_VertexIndex & 1) << 2);
    float y = -1.0 + float((gl_VertexIndex & 2) << 1);
    
    gl_Position = vec4(x, y, 0.0, 1.0);
    
    // Convert to texture coordinates [0,1]
    fragCoord = gl_Position.xy * 0.5 + 0.5;
}