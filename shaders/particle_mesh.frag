#version 460 core
#extension GL_EXT_ray_query : require

// Fragment shader for mesh-generated particle billboards with RT shadows
// This creates the visual appearance of each particle

layout(location = 0) in vec4 fragColor;
layout(location = 1) in vec2 fragUV;
layout(location = 2) in vec3 fragWorldPos;

layout(location = 0) out vec4 outColor;

// Job 1003: RT acceleration structure binding for external occluders
layout(binding = 3) uniform accelerationStructureEXT topLevelAS;

// Job 1014: Shell TLAS for self-shadowing (conditional)
#ifdef USE_SHELL_TLAS
layout(binding = 4) uniform accelerationStructureEXT shellTLAS;
#endif

// Push constants (same as mesh shader)
layout(push_constant) uniform PushConstants {
    mat4 viewProj;              // 64 bytes
    vec3 cameraPos;             // 12 bytes
    float particleSize;         // 4 bytes (total 80 bytes)
    vec3 lightDirection;        // 12 bytes
    float lightIntensity;       // 4 bytes (total 96 bytes)
    uint particleCount;         // 4 bytes
    float time;                 // 4 bytes
    uint rtEnabled;             // 4 bytes
    uint occlusionAmplify;      // 4 bytes (total 112 bytes)
    uint rtSelfShadowOverlay;   // 4 bytes
    uint rtCullMaskMode;        // 4 bytes
    uint shellTlasAvailable;    // 4 bytes
    uint _padding;              // 4 bytes (total 128 bytes exactly)
} pc;

// Job 1003: Ray query occlusion function for external occluders
// CR 1018: Updated with instance mask support (external TLAS = 0x01)
float hasOccluderRT(vec3 originWS, vec3 dirWS, float tMax) {
    rayQueryEXT rq;
    const uint flags = gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsOpaqueEXT;
    const uint cullMask = 0x01; // Only hit external TLAS instances
    rayQueryInitializeEXT(rq, topLevelAS, flags, cullMask, originWS, 0.001, normalize(dirWS), tMax);
    while (rayQueryProceedEXT(rq)) {}
    bool hit = rayQueryGetIntersectionTypeEXT(rq, true) != gl_RayQueryCommittedIntersectionNoneEXT;
    if (hit) {
        return rayQueryGetIntersectionTEXT(rq, true); // Return hit distance for overlay
    }
    return -1.0; // No hit
}

// Job 1014: Ray query self-shadowing function using shell TLAS
// CR 1018: Updated with instance mask support (shell TLAS = 0x02)
// CR 1022: Added guard for shell TLAS availability
// CR 1026: Conditional compilation for dual pipeline support
float hasSelfShadowRT(vec3 originWS, vec3 dirWS, float tMax) {
#ifdef USE_SHELL_TLAS
    // CR 1022: Guard against unavailable shell TLAS
    if (pc.shellTlasAvailable == 0u) {
        return -1.0; // No hit when shell TLAS not available
    }

    rayQueryEXT rq;
    const uint flags = gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsOpaqueEXT;
    const uint cullMask = 0x02; // Only hit shell TLAS instances
    rayQueryInitializeEXT(rq, shellTLAS, flags, cullMask, originWS, 0.001, normalize(dirWS), tMax);
    while (rayQueryProceedEXT(rq)) {}
    bool hit = rayQueryGetIntersectionTypeEXT(rq, true) != gl_RayQueryCommittedIntersectionNoneEXT;
    if (hit) {
        return rayQueryGetIntersectionTEXT(rq, true); // Return hit distance for overlay
    }
    return -1.0; // No hit
#else
    // CR 1026: No shell TLAS available in external-only pipeline
    return -1.0; // No self-shadowing when shell TLAS not supported
#endif
}

// CR 1018: Debug overlay function - colors fragments by hit distance
vec3 rayHitOverlay(float hitDistance) {
    if (hitDistance < 0.0) {
        return vec3(0.5, 0.0, 0.5); // Purple = no hit
    }

    // Color by distance buckets: green (close), yellow (mid), red (far)
    if (hitDistance < 5.0) {
        return vec3(0.0, 1.0, 0.0); // Green = close hit
    } else if (hitDistance < 20.0) {
        return vec3(1.0, 1.0, 0.0); // Yellow = medium hit
    } else {
        return vec3(1.0, 0.0, 0.0); // Red = far hit
    }
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

    // Job 1005: Lambert shading with directional light
    vec3 baseColor = fragColor.rgb;

    // Billboard normal approximation (view-facing)
    vec3 normal = normalize(toCamera); // For billboards, approximate normal as view-facing

    // Lambert lighting calculation
    vec3 lightDir = normalize(pc.lightDirection);
    float lambert = max(dot(normal, -lightDir), 0.0);
    float lighting = 0.3 + 0.7 * lambert; // Ambient + diffuse

    vec3 litColor = baseColor * lighting * pc.lightIntensity * brightness * glowIntensity;
    litColor += litColor * centerGlow * 0.5; // Bright center

    // Job 1005: RT shadow computation with amplification
    // Job 1014: Combined external occlusion and self-shadowing
    // CR 1018: Enhanced with instance masks and debug overlay
    float shadowVis = 1.0;
    vec3 overlayColor = vec3(0.0);
    bool useOverlay = false;

    if (pc.rtEnabled != 0u) {
        float externalHitDist = -1.0;
        float selfShadowHitDist = -1.0;

        // Query based on mask mode
        if (pc.rtCullMaskMode == 0u || pc.rtCullMaskMode == 1u) { // Both or external only
            externalHitDist = hasOccluderRT(fragWorldPos, lightDir, 1000.0);
        }
        if (pc.rtCullMaskMode == 0u || pc.rtCullMaskMode == 2u) { // Both or shells only
            selfShadowHitDist = hasSelfShadowRT(fragWorldPos, lightDir, 1000.0);
        }

        // Apply shadows based on hits
        bool externalOcclusion = externalHitDist >= 0.0;
        bool selfShadowing = selfShadowHitDist >= 0.0;

        if (externalOcclusion || selfShadowing) {
            float externalShadow = externalOcclusion ? 0.1 : 1.0;  // Strong external shadows
            float selfShadow = selfShadowing ? 0.3 : 1.0;          // Softer self-shadows
            shadowVis = externalShadow * selfShadow;
        }

        // CR 1018: Debug overlay mode
        if (pc.rtSelfShadowOverlay != 0u) {
            useOverlay = true;
            // Prioritize self-shadow hits for overlay, fall back to external
            float displayHitDist = (selfShadowHitDist >= 0.0) ? selfShadowHitDist : externalHitDist;
            overlayColor = rayHitOverlay(displayHitDist);
        }

        // Job 1005: Debug occlusion amplification
        if (pc.occlusionAmplify != 0u) {
            shadowVis = shadowVis * shadowVis;
        }
    }

    // Apply shadow visibility with configurable weight
    float shadowWeight = (pc.occlusionAmplify != 0u) ? 2.0 : 1.0;
    vec3 finalColor = litColor * mix(1.0, shadowVis, clamp(shadowWeight, 0.0, 1.0));

    // CR 1018: Apply debug overlay if enabled
    if (useOverlay) {
        finalColor = mix(finalColor, overlayColor, 0.7); // Blend overlay with particle color
    } else {
        // Velocity-based saturation boost (only when not in overlay mode)
        finalColor = mix(finalColor, finalColor * 1.3, velocityIntensity * 0.3);
    }

    outColor = vec4(finalColor, alpha * fragColor.a);
}