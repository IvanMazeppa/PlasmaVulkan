#version 460 core
#extension GL_EXT_ray_query : require

layout(location = 0) in vec2 fragCoord;
layout(location = 0) out vec4 fragColor;

layout(binding = 0) uniform accelerationStructureEXT topLevelAS;

void main() {
    // Minimal ray query test - doesn't need to do anything meaningful
    rayQueryEXT rq;

    // Simple test ray (origin at camera, direction forward)
    vec3 rayOrigin = vec3(0.0, 0.0, 0.0);
    vec3 rayDir = vec3(0.0, 0.0, 1.0);

    // Initialize ray query
    rayQueryInitializeEXT(rq, topLevelAS,
        gl_RayFlagsTerminateOnFirstHitEXT,
        0xFF, rayOrigin, 0.001, rayDir, 1000.0);

    // Process ray query
    while (rayQueryProceedEXT(rq)) {}

    // Check if we hit anything
    bool hasHit = rayQueryGetIntersectionTypeEXT(rq, true) != gl_RayQueryCommittedIntersectionNoneEXT;

    // Output test color based on hit result
    fragColor = hasHit ? vec4(1.0, 0.0, 0.0, 1.0) : vec4(0.0, 1.0, 0.0, 1.0);
}