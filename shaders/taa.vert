#version 450

// TAA (Temporal Anti-Aliasing) vertex shader - fullscreen triangle
// Generates vertices for fullscreen rendering without vertex buffer

layout(location = 0) out vec2 fragCoord;

void main() {
    // Generate fullscreen triangle vertices procedurally
    // Vertex 0: (-1, -1), Vertex 1: (3, -1), Vertex 2: (-1, 3)
    // This creates a triangle that covers the entire screen
    vec2 positions[3] = vec2[](
        vec2(-1.0, -1.0),
        vec2( 3.0, -1.0),
        vec2(-1.0,  3.0)
    );
    
    gl_Position = vec4(positions[gl_VertexIndex], 0.0, 1.0);
    
    // Convert from NDC [-1, 1] to texture coordinates [0, 1]
    fragCoord = positions[gl_VertexIndex] * 0.5 + 0.5;
}