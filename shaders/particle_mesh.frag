#version 460 core
#extension GL_EXT_ray_query : require

// Fragment shader for mesh-generated particle billboards with RT shadows
// This creates the visual appearance of each particle

layout(location = 0) in vec4 fragColor;
layout(location = 1) in vec2 fragUV;
layout(location = 2) in vec3 fragWorldPos;

layout(location = 0) out vec4 outColor;

// Job 1003: RT acceleration structure binding
layout(binding = 3) uniform accelerationStructureEXT topLevelAS;

// Push constants (same as mesh shader)
layout(push_constant) uniform PushConstants {
    mat4 viewProj;
    vec3 cameraPos;
    float particleSize;
    uint particleCount;
    float time;
    uint rtEnabled;  // RT toggle
} pc;

// Job 1003: Ray query occlusion function
bool hasOccluderRT(vec3 originWS, vec3 dirWS, float tMax) {
    rayQueryEXT rq;
    const uint flags = gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsOpaqueEXT;
    rayQueryInitializeEXT(rq, topLevelAS, flags, 0xFF, originWS, 0.001, normalize(dirWS), tMax);
    while (rayQueryProceedEXT(rq)) {}
    return rayQueryGetIntersectionTypeEXT(rq, true) != gl_RayQueryCommittedIntersectionNoneEXT;
}

void main() {
    // Create circular particle shape with soft edges
    vec2 coord = fragUV * 2.0 - 1.0;  // Convert to [-1, 1] range
    float dist = length(coord);
    
    // Smooth circular falloff
    float alpha = 1.0 - smoothstep(0.3, 1.0, dist);
    
    // Discard fragments outside circle
    if (alpha <= 0.01) {
        discard;
    }
    
    // Distance-based brightness falloff for depth
    vec3 toCamera = pc.cameraPos - fragWorldPos;
    float distance = length(toCamera);
    float brightness = 1.0 / (1.0 + distance * 0.01);
    
    // Enhanced particle glow with velocity-based intensity
    float velocityIntensity = fragColor.a; // Store velocity magnitude in alpha
    float glowIntensity = 0.8 + velocityIntensity * 0.4;
    
    // Core particle with bright center
    float centerGlow = 1.0 - smoothstep(0.0, 0.4, dist);
    float outerGlow = alpha * 0.6;
    
    vec3 finalColor = fragColor.rgb * brightness * glowIntensity;
    finalColor += finalColor * centerGlow * 0.5; // Bright center

    // Job 1003: RT shadow computation
    float shadowVis = 1.0;
    if (pc.rtEnabled != 0u) {
        vec3 rtLightDir = normalize(vec3(-0.5, -0.8, -0.6));
        if (hasOccluderRT(fragWorldPos, rtLightDir, 1000.0)) {
            shadowVis = 0.0; // Fully shadowed
        }
    }
    finalColor *= shadowVis;

    // Velocity-based saturation boost
    finalColor = mix(finalColor, finalColor * 1.3, velocityIntensity * 0.3);

    outColor = vec4(finalColor, alpha * fragColor.a);
}