#include "shader.h"

#include <cstdio>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

// ---------------------------------------------------------------------------
// GLSL version selector. Desktop OpenGL gets core profile 3.30; Emscripten
// (WebGL 2) gets GLSL ES 3.00, which requires explicit precision qualifiers.
// The splat shader uses a separate version directive — 4.20 desktop /
// 3.00 ES web — because it relies on unpackHalf2x16 which is core in
// both of those but not in GLSL 3.30. The board / shadow / highlight
// shaders stay on the original 3.30 / 3.00 ES baseline.
// ---------------------------------------------------------------------------
#ifdef __EMSCRIPTEN__
#define GLSL_VERSION         "#version 300 es\n"
#define GLSL_FS_PREAMBLE     "precision highp float;\nprecision highp int;\nprecision highp sampler2D;\n"
#define GLSL_SPLAT_VERSION   "#version 300 es\n"
#define GLSL_SPLAT_PREAMBLE  "precision highp float;\nprecision highp int;\nprecision highp usampler2D;\n"
#else
#define GLSL_VERSION         "#version 330 core\n"
#define GLSL_FS_PREAMBLE     ""
#define GLSL_SPLAT_VERSION   "#version 420 core\n"
#define GLSL_SPLAT_PREAMBLE  ""
#endif

// ---------------------------------------------------------------------------
// Shadow depth shaders
// ---------------------------------------------------------------------------
const char* shadow_vs_src = GLSL_VERSION R"(
layout(location = 1) in vec3 aPos;

uniform mat4 uLightSpaceMatrix;
uniform mat4 uModel;

void main() {
    gl_Position = uLightSpaceMatrix * uModel * vec4(aPos, 1.0);
}
)";

const char* shadow_fs_src = GLSL_VERSION GLSL_FS_PREAMBLE R"(
void main() {
}
)";

// ---------------------------------------------------------------------------
// Shader sources — PBR with Cook-Torrance BRDF and environment reflections
// ---------------------------------------------------------------------------
const char* vertex_shader_src = GLSL_VERSION R"(
layout(location = 0) in vec3 aNormal;
layout(location = 1) in vec3 aPos;
// Optional UV channel — only enabled on meshes that ship with
// per-vertex texture coords (the chess clock's `.uvmesh` body).
// Other meshes (pieces, board, frame) leave location 2 disabled,
// in which case vTexCoord lands at (0,0) and the texture-sample
// branch in the fragment shader is gated off via uClockTextureMode.
layout(location = 2) in vec2 aTexCoord;

uniform mat4 uModel;
uniform mat4 uView;
uniform mat4 uProjection;
uniform mat3 uNormalMat;
uniform mat4 uLightSpaceMatrix;

out vec3 vNormal;
out vec3 vFragPos;
out vec3 vWorldPos;
out vec4 vLightSpacePos;
out vec2 vTexCoord;

void main() {
    vec4 worldPos = uModel * vec4(aPos, 1.0);
    vFragPos = worldPos.xyz;
    vWorldPos = worldPos.xyz;
    vNormal = normalize(uNormalMat * aNormal);
    vLightSpacePos = uLightSpaceMatrix * worldPos;
    vTexCoord = aTexCoord;
    gl_Position = uProjection * uView * worldPos;
}
)";

const char* fragment_shader_src = GLSL_VERSION GLSL_FS_PREAMBLE R"(
in vec3 vNormal;
in vec3 vFragPos;
in vec3 vWorldPos;
in vec4 vLightSpacePos;

out vec4 FragColor;

// Lights
uniform vec3 uLightPositions[4];
uniform vec3 uLightColors[4];
uniform vec3 uViewPos;

// Shadow map
uniform sampler2D uShadowMap;

// Material
uniform vec3 uAlbedo;
uniform float uMetallic;
uniform float uRoughness;
uniform float uAO;         // ambient occlusion
uniform int uWoodEffect;

// Optional textured-wood path (used by the imported Sketchfab
// frame). uWoodTextureMode == 1 → sample uWoodDiffuse and (if
// available) uWoodSpecular via triplanar projection from world
// position. STL has no UVs; triplanar sidesteps that without
// adding an attribute. uWoodScale tunes how many texture
// repeats across one project unit.
uniform int uWoodTextureMode;
uniform sampler2D uWoodDiffuse;
uniform sampler2D uWoodSpecular;
uniform float uWoodScale;

// Per-draw opacity for the FragColor alpha — defaults to 1.0
// (opaque). Used by the chess-clock glass dials (opacity ~0.25
// + GL_BLEND on the caller side).
uniform float uMaterialOpacity;

// UV-mapped diffuse path (used by the chess-clock body + dials).
// uClockTextureMode != 0 → albedo = sample(uClockDiffuse, uv) *
// uAlbedo. uClockPbrMapsMode != 0 → also read roughness +
// metalness from their respective maps so the body's metal
// fittings catch reflections without the dial face going shiny.
uniform int uClockTextureMode;
uniform int uClockPbrMapsMode;
uniform int uClockAlphaCutout;   // discard transparent texels (letter decals)
uniform sampler2D uClockDiffuse;
uniform sampler2D uClockRoughnessTex;
uniform sampler2D uClockMetalnessTex;
in vec2 vTexCoord;

// Planar-reflection path (used by the lacquered squares).
// uPlanarReflectionMode != 0 → mix the env-spec lookup with a
// screen-space sample of uReflectionTex (the prior pass that
// rendered the pieces with the camera mirrored about Y = 0).
// gl_FragCoord is divided by uViewportSize to land the right
// texel.
uniform int uPlanarReflectionMode;
uniform sampler2D uReflectionTex;
uniform vec2 uViewportSize;
uniform float uReflectionStrength;

// =========================================================================
// Procedural noise for wood grain
// =========================================================================
float hash(vec2 p) {
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
}

float hash3(vec3 p) {
    return fract(sin(dot(p, vec3(127.1, 311.7, 74.7))) * 43758.5453);
}

float noise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = hash(i);
    float b = hash(i + vec2(1.0, 0.0));
    float c = hash(i + vec2(0.0, 1.0));
    float d = hash(i + vec2(1.0, 1.0));
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

float fbm(vec2 p) {
    float v = 0.0;
    float a = 0.5;
    mat2 rot = mat2(0.8, 0.6, -0.6, 0.8);
    for (int i = 0; i < 6; i++) {
        v += a * noise(p);
        p = rot * p * 2.0;
        a *= 0.5;
    }
    return v;
}

vec3 woodColor(vec3 baseColor, vec3 pos) {
    vec2 wp = pos.xz * 3.0;

    // Growth ring pattern with turbulence
    float dist = length(wp * vec2(1.0, 0.25));
    float turb = fbm(wp * 1.5) * 2.0;
    float rings = fract(dist * 5.0 + turb);
    rings = smoothstep(0.0, 0.4, rings) * smoothstep(1.0, 0.6, rings);

    // Fine grain lines (anisotropic)
    float grain = fbm(vec2(pos.x * 60.0, pos.z * 3.0)) * 0.12;
    float fineGrain = fbm(vec2(pos.x * 120.0, pos.z * 6.0)) * 0.06;

    // Medullary rays (cross-grain shimmer)
    float rays = noise(vec2(pos.z * 30.0, pos.x * 2.0)) * 0.08;

    // Knot variation
    float knot = fbm(wp * 0.4) * 0.15;

    vec3 darkWood = baseColor * 0.65;
    vec3 lightWood = baseColor * 1.2;
    vec3 col = mix(darkWood, lightWood, rings * 0.55 + 0.45);
    col -= grain + fineGrain;
    col += rays;
    col += knot * 0.25;

    return clamp(col, 0.0, 1.0);
}

// =========================================================================
// PBR functions — Cook-Torrance BRDF
// =========================================================================
const float PI = 3.14159265359;

// GGX/Trowbridge-Reitz normal distribution
float DistributionGGX(vec3 N, vec3 H, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotH = max(dot(N, H), 0.0);
    float NdotH2 = NdotH * NdotH;
    float denom = NdotH2 * (a2 - 1.0) + 1.0;
    return a2 / (PI * denom * denom);
}

// Schlick-GGX geometry function
float GeometrySchlickGGX(float NdotV, float roughness) {
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return NdotV / (NdotV * (1.0 - k) + k);
}

// Smith's geometry function
float GeometrySmith(vec3 N, vec3 V, vec3 L, float roughness) {
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    return GeometrySchlickGGX(NdotV, roughness) *
           GeometrySchlickGGX(NdotL, roughness);
}

// Fresnel-Schlick
vec3 fresnelSchlick(float cosTheta, vec3 F0) {
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

// Fresnel-Schlick with roughness (for IBL)
vec3 fresnelSchlickRoughness(float cosTheta, vec3 F0, float roughness) {
    return F0 + (max(vec3(1.0 - roughness), F0) - F0) *
           pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

// =========================================================================
// Procedural environment map (indoor studio lighting)
// =========================================================================
vec3 sampleEnvironment(vec3 dir) {
    float y = dir.y;

    // Base gradient: warm room. Ceiling intentionally kept under
    // 1.4 so upward-facing surfaces on white pieces don't drown the
    // albedo.
    vec3 ceiling  = vec3(1.35, 1.28, 1.13); // warm white above
    vec3 horizon  = vec3(0.55, 0.50, 0.46); // warm gray walls
    vec3 floor_c  = vec3(0.22, 0.19, 0.16); // warm floor (bounce light)

    vec3 env;
    if (y > 0.0) {
        env = mix(horizon, ceiling, smoothstep(0.0, 0.8, y));
    } else {
        env = mix(horizon, floor_c, smoothstep(0.0, -0.5, y));
    }

    // Soft area light overhead (large softbox). Toned down from 4.0
    // so the top of white pieces doesn't blow out.
    float lightDist1 = length(dir - normalize(vec3(0.3, 0.95, 0.2)));
    env += vec3(2.9, 2.75, 2.45) * exp(-lightDist1 * lightDist1 * 6.0);

    // Secondary fill from the side
    float lightDist2 = length(dir - normalize(vec3(-0.6, 0.7, -0.4)));
    env += vec3(1.55, 1.60, 1.80) * exp(-lightDist2 * lightDist2 * 10.0);

    // Warm bounce from below (floor reflection)
    float lightDist3 = length(dir - normalize(vec3(0.0, -0.3, 0.5)));
    env += vec3(0.65, 0.50, 0.35) * exp(-lightDist3 * lightDist3 * 12.0);

    // Back fill (reduces dark shadows on far side of pieces)
    float lightDist4 = length(dir - normalize(vec3(0.0, 0.4, -0.8)));
    env += vec3(0.80, 0.77, 0.68) * exp(-lightDist4 * lightDist4 * 12.0);

    return env;
}

// Approximate diffuse irradiance from environment
vec3 envDiffuse(vec3 N) {
    // Sample a few directions around the normal for cheap irradiance
    vec3 irradiance = vec3(0.0);
    vec3 up = abs(N.y) < 0.999 ? vec3(0, 1, 0) : vec3(1, 0, 0);
    vec3 right = normalize(cross(up, N));
    up = cross(N, right);

    irradiance += sampleEnvironment(N);
    irradiance += sampleEnvironment(normalize(N + right * 0.5));
    irradiance += sampleEnvironment(normalize(N - right * 0.5));
    irradiance += sampleEnvironment(normalize(N + up * 0.5));
    irradiance += sampleEnvironment(normalize(N - up * 0.5));

    return irradiance / 5.0;
}

// =========================================================================
// Main
// =========================================================================
void main() {
    vec3 N = normalize(vNormal);
    if (!gl_FrontFacing) N = -N;

    vec3 V = normalize(uViewPos - vFragPos);
    float NdotV = max(dot(N, V), 0.0);

    // --- Material ---
    vec3 albedo = uAlbedo;
    float metallic = uMetallic;
    float roughness = uRoughness;
    float ao = uAO;

    // UV-mapped diffuse for meshes that ship with explicit UVs
    // (the chess clock body + dials). Flip V because PNG images
    // load top-down but OpenGL UVs are bottom-up; without the
    // flip the dial faces would render upside-down.
    if (uClockTextureMode != 0) {
        vec2 uv = vec2(vTexCoord.x, 1.0 - vTexCoord.y);
        vec4 texc = texture(uClockDiffuse, uv);
        if (uClockAlphaCutout != 0 && texc.a < 0.5) discard;
        vec3 tex = texc.rgb;
        albedo = tex * uAlbedo;
        if (uClockPbrMapsMode != 0) {
            // Roughness + metalness sampled from their bundled
            // grayscale maps. uMetallic / uRoughness pass-through
            // as multipliers so the caller can still bias the
            // overall feel from C++.
            roughness = texture(uClockRoughnessTex, uv).r * uRoughness;
            metallic  = texture(uClockMetalnessTex, uv).r * uMetallic;
            roughness = clamp(roughness, 0.04, 1.0);
        }
    }

    if (uWoodEffect != 0) {
        albedo = woodColor(albedo, vWorldPos);
        // Wood grain affects roughness slightly
        float grainVar = fbm(vWorldPos.xz * 20.0) * 0.1;
        roughness = clamp(roughness + grainVar, 0.05, 1.0);
    }

    // Textured-wood path — STL has no UVs so we triplanar-project
    // the walnut diffuse + spec maps from world position. Blend
    // weights are |normal| normalised so the three samples sum to
    // one and faces tilted between axes don't go dark.
    if (uWoodTextureMode != 0) {
        vec3 wp = vWorldPos * uWoodScale;
        vec3 absN = abs(N);
        float sum = max(absN.x + absN.y + absN.z, 1e-3);
        absN /= sum;
        vec3 diffX = texture(uWoodDiffuse, wp.zy).rgb;
        vec3 diffY = texture(uWoodDiffuse, wp.xz).rgb;
        vec3 diffZ = texture(uWoodDiffuse, wp.xy).rgb;
        vec3 woodDiffuse = diffX * absN.x + diffY * absN.y + diffZ * absN.z;
        // uAlbedo doubles as a tint multiplier; pass (1,1,1) for
        // pure-texture display, or a warm tone to bias.
        albedo = woodDiffuse * uAlbedo;

        float specX = texture(uWoodSpecular, wp.zy).g;
        float specY = texture(uWoodSpecular, wp.xz).g;
        float specZ = texture(uWoodSpecular, wp.xy).g;
        float spec  = specX * absN.x + specY * absN.y + specZ * absN.z;
        // Sketchfab's spec map is bright = shiny → low roughness.
        // Bend it into a [0.15, 0.55] band centered around the
        // user-provided uRoughness so a polished walnut stays
        // glossy where the grain is dense.
        float specRoughness = mix(0.55, 0.15, spec);
        roughness = clamp(mix(uRoughness, specRoughness, 0.7),
                          0.05, 1.0);
    }

    // Base reflectance (dielectric = 0.04, metallic = albedo)
    vec3 F0 = vec3(0.04);
    F0 = mix(F0, albedo, metallic);

    // --- Shadow calculation (PCF soft shadows from key light) ---
    float shadow = 0.0;
    {
        vec3 projCoords = vLightSpacePos.xyz / vLightSpacePos.w;
        projCoords = projCoords * 0.5 + 0.5;

        if (projCoords.z <= 1.0 && projCoords.x >= 0.0 && projCoords.x <= 1.0
            && projCoords.y >= 0.0 && projCoords.y <= 1.0) {
            float currentDepth = projCoords.z;
            vec3 keyLightDir = normalize(uLightPositions[0]);
            float bias = max(0.003 * (1.0 - dot(N, keyLightDir)), 0.0005);

            // 5x5 PCF for soft shadow edges
            vec2 texelSize = 1.0 / vec2(textureSize(uShadowMap, 0));
            for (int x = -2; x <= 2; x++) {
                for (int y = -2; y <= 2; y++) {
                    float pcfDepth = texture(uShadowMap,
                        projCoords.xy + vec2(x, y) * texelSize).r;
                    shadow += (currentDepth - bias > pcfDepth) ? 1.0 : 0.0;
                }
            }
            shadow /= 25.0;
        }
    }

    // --- Direct lighting (4 point/directional lights) ---
    vec3 Lo = vec3(0.0);

    for (int i = 0; i < 4; i++) {
        vec3 L = normalize(uLightPositions[i]);
        vec3 H = normalize(V + L);
        float NdotL = max(dot(N, L), 0.0);

        vec3 radiance = uLightColors[i];

        // Apply shadow to key light and partially to fill. Shadow
        // strength softened from 0.85 so lit and shadowed areas
        // aren't as far apart — reduces overall contrast.
        if (i == 0) radiance *= (1.0 - shadow * 0.72);
        if (i == 1) radiance *= (1.0 - shadow * 0.25);

        // Cook-Torrance BRDF
        float D = DistributionGGX(N, H, roughness);
        float G = GeometrySmith(N, V, L, roughness);
        vec3  F = fresnelSchlick(max(dot(H, V), 0.0), F0);

        vec3 numerator = D * G * F;
        float denominator = 4.0 * NdotV * NdotL + 0.0001;
        vec3 specular = numerator / denominator;

        vec3 kS = F;
        vec3 kD = (1.0 - kS) * (1.0 - metallic);

        Lo += (kD * albedo / PI + specular) * radiance * NdotL;
    }

    // --- Image-based lighting (environment reflections) ---
    vec3 F_env = fresnelSchlickRoughness(NdotV, F0, roughness);

    // Diffuse IBL
    vec3 kS_env = F_env;
    vec3 kD_env = (1.0 - kS_env) * (1.0 - metallic);
    vec3 irradiance = envDiffuse(N);
    vec3 diffuseIBL = kD_env * albedo * irradiance;

    // Specular IBL — sample environment in reflection direction
    vec3 R = reflect(-V, N);
    vec3 envSampleDir = mix(R, N, roughness * roughness);
    vec3 envSpec = sampleEnvironment(envSampleDir);
    // Scale down for rough/dark surfaces to prevent blow-out.
    // Dropped from 0.4 to 0.30 so specular highlights on white
    // pieces don't clip to pure white.
    vec3 specularIBL = envSpec * F_env * (1.0 - roughness) * 0.30;

    // Planar reflection — when the prior pass dropped a mirrored
    // render of the pieces into uReflectionTex, replace the env-
    // spec sample with a screen-space lookup. Squares with low
    // roughness pick this up and pieces appear reflected on the
    // glassy surface like lacquer / piano black.
    if (uPlanarReflectionMode != 0) {
        vec2 ruv = gl_FragCoord.xy / max(uViewportSize, vec2(1.0));
        vec3 planarRefl = texture(uReflectionTex, ruv).rgb;
        // Strength is weighted by Fresnel so reflections are
        // subtle at normal incidence and stronger at grazing
        // angles, the way real glass behaves.
        float fresnelW = pow(1.0 - clamp(NdotV, 0.0, 1.0), 4.0);
        float w = clamp(uReflectionStrength *
                        ((1.0 - roughness) * 0.6 + fresnelW * 0.4),
                        0.0, 1.0);
        specularIBL = mix(specularIBL, planarRefl, w);
    }

    // Darken ambient slightly in shadowed areas (contact shadow approx)
    float ambientShadow = 1.0 - shadow * 0.18;
    vec3 ambient = (diffuseIBL + specularIBL) * ao * ambientShadow;

    vec3 color = ambient + Lo;

    // --- Tone mapping (ACES filmic) ---
    color = color * 0.66; // exposure (was 0.75)
    vec3 x = color;
    color = (x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14);

    // Gamma correction
    color = pow(clamp(color, 0.0, 1.0), vec3(1.0 / 2.2));

    // uMaterialOpacity defaults to 1.0 in glsl (which we set
    // explicitly from the C++ side per-draw). Glass dials use
    // ~0.25 with GL_BLEND for see-through.
    float opacity = uMaterialOpacity > 0.0 ? uMaterialOpacity : 1.0;
    FragColor = vec4(color, opacity);
}
)";

// ---------------------------------------------------------------------------
// Highlight shaders (for selection halo and valid move indicators)
// ---------------------------------------------------------------------------
const char* highlight_vs_src = GLSL_VERSION R"(
layout(location = 0) in vec3 aPos;
// Optional per-vertex color, used by the withdraw flag's cloth to
// carry normal-based lighting. When uUseVertexColor is 0 (the
// default) this attribute is ignored and uColor is used for the
// whole draw, preserving existing call sites.
layout(location = 1) in vec3 aVColor;

uniform mat4 uMVP;

out vec2 vLocalPos;
out vec3 vVColor;

void main() {
    vLocalPos = aPos.xz;
    vVColor = aVColor;
    gl_Position = uMVP * vec4(aPos, 1.0);
}
)";

const char* highlight_fs_src = GLSL_VERSION GLSL_FS_PREAMBLE R"(
in vec2 vLocalPos;
in vec3 vVColor;

out vec4 FragColor;

uniform vec4 uColor;
uniform float uInnerRadius;
uniform float uOuterRadius;

// Optional horizontal gradient mode for flat UI overlays. Used by the
// pregame ELO slider to paint the capsule with a green→red gradient
// without needing a separate shader. When uUseGradient != 0 the flat
// color branch mixes uColor and uColorB based on vLocalPos.x normalized
// over [uGradX0, uGradX1]. Leave at 0 for ordinary flat-color draws.
uniform int   uUseGradient;
uniform vec4  uColorB;
uniform float uGradX0;
uniform float uGradX1;

// Optional per-vertex color mode for the withdraw flag's cloth
// (normal-based lighting). When non-zero, flat mode uses vVColor.rgb
// instead of uColor.rgb; uColor.a is still respected.
uniform int   uUseVertexColor;

void main() {
    // Flat mode: when both radii are 0, draw solid color (for UI overlays)
    if (uInnerRadius <= 0.0 && uOuterRadius <= 0.0) {
        if (uUseVertexColor != 0) {
            FragColor = vec4(vVColor, uColor.a);
        } else if (uUseGradient != 0) {
            float span = uGradX1 - uGradX0;
            float t = (span > 0.0) ? (vLocalPos.x - uGradX0) / span : 0.0;
            t = clamp(t, 0.0, 1.0);
            FragColor = mix(uColor, uColorB, t);
        } else {
            FragColor = uColor;
        }
        return;
    }

    float dist = length(vLocalPos);

    // Ring center radius and half-width
    float mid = (uInnerRadius + uOuterRadius) * 0.5;
    float half_w = (uOuterRadius - uInnerRadius) * 0.5;

    // Smooth ring shape: peak at center of ring, fade to edges
    float ring = 1.0 - smoothstep(0.0, half_w, abs(dist - mid));

    // Glow: softer falloff beyond the ring
    float glow = exp(-3.0 * pow(abs(dist - mid) / half_w, 2.0));

    float alpha = max(ring * 0.8, glow * 0.35) * uColor.a;
    if (alpha < 0.01) discard;

    FragColor = vec4(uColor.rgb, alpha);
}
)";

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// Glass shatter transition shader
// Each vertex carries: shard center (NDC), local offset from center (NDC),
// texture UV, and a per-shard random seed.
// ---------------------------------------------------------------------------
const char* shatter_vs_src = GLSL_VERSION R"(
layout(location = 0) in vec2 aCenterNDC;
layout(location = 1) in vec2 aLocalOffset;
layout(location = 2) in vec2 aUV;
layout(location = 3) in float aSeed;

uniform float uTime;

out vec2 vUV;
out float vAlpha;

void main() {
    float t = uTime;

    // Outward velocity from screen center — stronger at the edges
    vec2 from_impact = aCenterNDC;
    float dist = length(from_impact);
    vec2 vel = dist > 0.001 ? normalize(from_impact) : vec2(0);
    vel *= 0.9 + 0.8 * aSeed + dist * 1.5;

    // Random additional spread
    vec2 rand_push = vec2(sin(aSeed * 7.3), cos(aSeed * 11.7)) * 0.4;
    vel += rand_push;

    // Gravity (screen space)
    vec2 grav = vec2(0.0, -3.5);

    // Rotation: each shard spins independently
    float spin_speed = (aSeed - 0.5) * 14.0;
    // Shards closer to the impact point rotate faster
    spin_speed *= (1.5 - dist);
    float ang = t * spin_speed;
    float c = cos(ang), s = sin(ang);
    mat2 rot = mat2(c, -s, s, c);
    vec2 rotated = rot * aLocalOffset;

    // Shrink slightly (depth-away illusion)
    float shrink = 1.0 - t * 0.3;
    rotated *= shrink;

    vec2 pos = aCenterNDC + rotated + vel * t + 0.5 * grav * t * t;

    gl_Position = vec4(pos, 0.0, 1.0);
    vUV = aUV;
    // Fade out at the end of the transition
    vAlpha = 1.0 - smoothstep(0.75, 1.3, t);
}
)";

const char* shatter_fs_src = GLSL_VERSION GLSL_FS_PREAMBLE R"(
in vec2 vUV;
in float vAlpha;

out vec4 FragColor;

uniform sampler2D uTex;

void main() {
    vec3 col = texture(uTex, vUV).rgb;
    // Slight darkening on edges of shards (thin outline)
    // simulate cracks using UV derivatives not available without dFdx, so skip
    FragColor = vec4(col, vAlpha);
}
)";

// Text shader (textured quads with alpha from font atlas)
// ---------------------------------------------------------------------------
const char* text_vs_src = GLSL_VERSION R"(
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aTexCoord;

uniform mat4 uMVP;

out vec2 vTexCoord;

void main() {
    vTexCoord = aTexCoord;
    gl_Position = uMVP * vec4(aPos, 1.0);
}
)";

const char* text_fs_src = GLSL_VERSION GLSL_FS_PREAMBLE R"(
in vec2 vTexCoord;

out vec4 FragColor;

uniform sampler2D uFontTex;
uniform vec4 uColor;

void main() {
    float a = texture(uFontTex, vTexCoord).r;
    if (a < 0.01) discard;
    FragColor = vec4(uColor.rgb, uColor.a * a);
}
)";

// ---------------------------------------------------------------------------
// Etched-label shader for the rank/file coordinates. Reads the same
// font alpha texture as the regular text shader, but instead of
// painting a flat color it fakes a recessed engraving on the wood:
//
//   * The body of each glyph blends a very dark stain over the wood
//     so the underlying grain stays visible (wood × 0.15 + stain).
//   * A bright tan rim runs along the glyph edge that faces the key
//     light — the upward-facing wall of the engraved groove.
//   * A near-black rim runs along the opposite edge — the downward-
//     facing wall, in shadow.
//
// uTexelSize is 1.0 / atlas_dims, supplied by the caller; the rim
// taps offset by ~1.5 texels in the world ±Z direction (which maps
// to ∓v in the atlas because the quad's local-up axis is +v0/-v1).
// ---------------------------------------------------------------------------
const char* etched_vs_src = GLSL_VERSION R"(
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aTexCoord;

uniform mat4 uMVP;

out vec2 vTexCoord;

void main() {
    vTexCoord = aTexCoord;
    gl_Position = uMVP * vec4(aPos, 1.0);
}
)";

const char* etched_fs_src = GLSL_VERSION GLSL_FS_PREAMBLE R"(
in vec2 vTexCoord;

out vec4 FragColor;

uniform sampler2D uFontTex;
uniform vec2 uTexelSize;
uniform vec3 uEtchTint;
uniform vec3 uHighlight;
uniform vec3 uShadowRim;
uniform float uOpacity;

void main() {
    float a = texture(uFontTex, vTexCoord).r;
    if (a < 0.02) discard;

    // The label quad is built so that v decreases toward world +Z
    // ("top" of the glyph as the player reads it from white's side)
    // and v increases toward world -Z. The studio rig's key light
    // sits roughly above + behind the +Z half of the board, so the
    // engraved wall facing world -Z catches the light and the wall
    // facing world +Z stays in shadow.
    float a_topward = texture(uFontTex,
                               vTexCoord - vec2(0.0, uTexelSize.y * 1.5)).r;
    float a_botward = texture(uFontTex,
                               vTexCoord + vec2(0.0, uTexelSize.y * 1.5)).r;

    float lit_rim    = clamp(a - a_botward, 0.0, 1.0);
    float shadow_rim = clamp(a - a_topward, 0.0, 1.0);
    float body       = max(a - max(lit_rim, shadow_rim), 0.0);

    vec3 color = uEtchTint  * body
               + uHighlight * lit_rim
               + uShadowRim * shadow_rim;

    // Cap alpha below 1 so the wood grain shows through the dark
    // stain — pure-opaque body would read as paint, not engraving.
    FragColor = vec4(color, a * uOpacity);
}
)";

// ---------------------------------------------------------------------------
// Wood-grain menu button shader. Replaces the flat-colored button
// quads on the main menu with chamfered walnut blocks. The vertex
// shader passes through the local UV (0..1 within the button) so
// the fragment shader can sample the walnut diffuse texture and
// also compute distance-to-nearest-edge for a faux 3D bevel:
//
//   * top edge falls off into a bright highlight (key light hits
//     the upward-facing chamfer face),
//   * bottom edge into a soft shadow (downward-facing face),
//   * sides into a subtler darken (side faces, halfway-lit).
//
// uHoverBoost lifts the surface a touch on the hovered button so
// the user gets clear feedback without the old per-button hue
// differentiation (start=blue, options=purple, etc.) — all buttons
// now share the same warm walnut palette as the board frame.
// ---------------------------------------------------------------------------
const char* wood_button_vs_src = GLSL_VERSION R"(
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUV;

uniform mat4 uMVP;

out vec2 vUV;

void main() {
    vUV = aUV;
    gl_Position = uMVP * vec4(aPos, 1.0);
}
)";

const char* wood_button_fs_src = GLSL_VERSION GLSL_FS_PREAMBLE R"(
in vec2 vUV;

out vec4 FragColor;

uniform sampler2D uWoodTex;
uniform vec2  uButtonSize;     // (W, H) in NDC — used to scale the
                                // wood UVs so grain density doesn't
                                // depend on aspect.
uniform float uChamferSize;    // chamfer width in NDC units
uniform float uHoverBoost;     // 0.0 idle, ~0.18 hover

void main() {
    // The walnut diffuse has its natural grain running vertically
    // in the source image. We want the grain to read horizontally
    // across the button (a walnut plank cut along the width), so
    // we swap U/V before sampling. A small height-modulated horizontal
    // wobble (sin term) breaks the dead-straight tiling into the
    // soft cathedral curves real walnut planks show; the second
    // sample at a different scale + a heavier wobble adds the long-
    // wavelength figure that makes the grain feel like a single
    // continuous board rather than a repeating texture.
    float aspect = uButtonSize.x / uButtonSize.y;
    vec2 base_uv = vec2(vUV.y, vUV.x);

    // Layer A — fine grain (the close-spaced pencil-line streaks).
    // Sampled at high frequency along the button's long axis so we
    // get many streaks across; the warp curves them into shallow
    // arcs instead of dead-straight ruler strokes.
    vec2 uv_a = base_uv * vec2(2.4, aspect * 2.0);
    uv_a.x += 0.12 * sin(vUV.x * 6.2831 * 1.5 + vUV.y * 2.4);
    uv_a.y += 0.04 * sin(vUV.x * 6.2831 * 3.0);
    vec3 wood_a = texture(uWoodTex, uv_a).rgb;

    // Layer B — long-wavelength figure (the broader light/dark
    // banding that makes one half of the plank read warmer than
    // the other). Lower freq, bigger warp; heavier offset so the
    // sample lands somewhere uncorrelated with layer A.
    vec2 uv_b = base_uv * vec2(0.42, aspect * 0.32)
              + vec2(0.31, 0.17);
    uv_b.x += 0.18 * sin(vUV.x * 6.2831 + vUV.y * 1.1 + 0.7);
    vec3 wood_b = texture(uWoodTex, uv_b).rgb;

    vec3 wood = mix(wood_a, wood_b, 0.40);
    // A tiny luminance-only contrast lift sharpens the grain so the
    // streaks read at button scale without darkening the body.
    float l = dot(wood, vec3(0.299, 0.587, 0.114));
    wood = mix(vec3(l), wood, 1.05);
    wood = (wood - 0.5) * 1.12 + 0.5;

    // Distance from each edge in NDC units, then collapse to the
    // single nearest-edge distance — we want a symmetric inset
    // bevel (same shadow + lit-rim pattern on all four sides), not
    // a directional one.
    float d_left  = vUV.x        * uButtonSize.x;
    float d_right = (1.0 - vUV.x) * uButtonSize.x;
    float d_bot   = vUV.y        * uButtonSize.y;
    float d_top   = (1.0 - vUV.y) * uButtonSize.y;
    float d_min   = min(min(d_left, d_right), min(d_top, d_bot));

    // The chamfer is split into two visible bands:
    //   * outer 25% — a thin, near-perimeter shadow line ("rabbet"
    //     where the chamfer drops away into the void),
    //   * next 50% — a soft lit ramp peaking mid-chamfer (the
    //     bevel face catching the studio rig from above),
    //   * remainder — flat plank top, unchanged.
    float t = clamp(d_min / uChamferSize, 0.0, 1.0);
    float edge_dark = (1.0 - smoothstep(0.0, 0.25, t)) * 0.55;
    float bevel_lit = smoothstep(0.20, 0.55, t) * (1.0 - smoothstep(0.55, 1.0, t)) * 0.45;
    float shading   = 1.0 - edge_dark + bevel_lit;
    shading         = max(shading, 0.30);

    // Slight overall warm tint and a hover-time lift so the wood
    // doesn't read as muddy.
    vec3 color = wood * shading * (1.05 + uHoverBoost);
    color = clamp(color, 0.0, 1.0);

    FragColor = vec4(color, 1.0);
}
)";

// ---------------------------------------------------------------------------
// Splat-backdrop blit. The splat scene is rendered into a single-sample
// off-screen FBO with its own depth + blend state; this shader simply
// pastes that FBO's colour texture into the main pass as a full-screen
// quad, isolating the splat-specific GL state from every subsequent
// draw (chessboard, pieces, UI). Pure pass-through — no shading, no
// gamma, no alpha.
// ---------------------------------------------------------------------------
const char* splat_blit_vs_src = GLSL_VERSION R"(
layout(location = 0) in vec2 aPos;

out vec2 vUV;

void main() {
    vUV = aPos * 0.5 + 0.5;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)";

const char* splat_blit_fs_src = GLSL_VERSION GLSL_FS_PREAMBLE R"(
in vec2 vUV;

out vec4 FragColor;

uniform sampler2D uTex;

void main() {
    FragColor = texture(uTex, vUV);
}
)";

// Depth-blit: write the GL-compute splat backdrop's surface distance into
// the main pass depth buffer (gl_FragDepth) so the chess board depth-tests
// against the room — depth-correct compositing instead of a flat overlay.
// Reuses splat_blit_vs_src for the fullscreen quad. Color writes are masked
// off by the caller; this shader only produces depth.
const char* splat_depth_blit_fs_src = GLSL_VERSION GLSL_FS_PREAMBLE R"(
in vec2 vUV;

uniform sampler2D uSurfDist;   // R32F view-space surface distance; 1e30 = none
uniform float uProjA;          // proj[2][2]  (column-major m[10])
uniform float uProjB;          // proj[3][2]  (column-major m[14])

void main() {
    float dist = texture(uSurfDist, vUV).r;
    if (dist >= 1.0e29) {
        gl_FragDepth = 1.0;        // no splat surface → board draws in front
        return;
    }
    // Camera looks down -Z so the surface's view-space z is -dist. Push it
    // through the projection's depth row: clip.z = uProjA*z + uProjB,
    // clip.w = -z = dist; window depth = (clip.z/clip.w)*0.5 + 0.5.
    float view_z = -dist;
    float ndc_z  = (uProjA * view_z + uProjB) / dist;
    gl_FragDepth = clamp(ndc_z * 0.5 + 0.5, 0.0, 1.0);
}
)";

// ---------------------------------------------------------------------------
// Compilation helpers
// ---------------------------------------------------------------------------
static void shader_log_error(const char* kind, const char* stage,
                             const char* log, const char* src) {
#ifdef __EMSCRIPTEN__
    // EM_ASM bypasses stdio entirely so the error is guaranteed to
    // reach the JS console no matter how stdio buffering behaves.
    EM_ASM({
        console.error('[shader]', UTF8ToString($0), UTF8ToString($1), '\n',
                      UTF8ToString($2));
        // Also log a chunk of source so we can correlate line numbers
        // with the error message.
        console.error('[shader src]', UTF8ToString($3));
    }, kind, stage ? stage : "", log, src ? src : "");
#else
    std::fprintf(stderr, "%s%s%s:\n%s\n",
                 kind, stage ? " " : "", stage ? stage : "", log);
    if (src) std::fprintf(stderr, "Source:\n%s\n", src);
#endif
}

// ---------------------------------------------------------------------------
// Skybox: equirectangular panorama background. Renders a fullscreen quad at
// the far plane (z=1.0) with no depth writes, then for each pixel
// reconstructs the view ray from the inverse projection, rotates into world
// space using the inverse of the view's rotation-only part, and samples the
// panorama texture at the spherical UV. Drawn FIRST in the scene FBO so the
// chess pieces / clock overdraw it where geometry exists.
// ---------------------------------------------------------------------------
const char* skybox_vs_src = GLSL_VERSION R"(
layout(location = 0) in vec2 aPos;

out vec2 vClip;

void main() {
    vClip = aPos;
    // z=1.0 puts us at the far plane.
    gl_Position = vec4(aPos, 1.0, 1.0);
}
)";

const char* skybox_fs_src = GLSL_VERSION GLSL_FS_PREAMBLE R"(
in vec2 vClip;
out vec4 FragColor;

uniform sampler2D uPano;
uniform mat4      uInvViewRot;   // inverse of view's rotation part
uniform mat4      uInvProj;      // inverse of perspective matrix

const float PI     = 3.14159265358979;
const float TWO_PI = 6.28318530717959;

void main() {
    // Reconstruct the view-space ray for this pixel.
    vec4 view_h = uInvProj * vec4(vClip, 0.0, 1.0);
    vec3 view_dir = normalize(view_h.xyz);
    // Rotate into world space (translation removed — skybox sits
    // at infinite distance).
    vec3 world_dir = (uInvViewRot * vec4(view_dir, 0.0)).xyz;
    // Equirectangular sample: u from atan2(z, x), v from asin(y).
    // Note our engine has +Y up, +Z forward (camera looks toward -Z).
    float u = atan(world_dir.z, world_dir.x) / TWO_PI + 0.5;
    float v = asin(clamp(world_dir.y, -1.0, 1.0)) / PI + 0.5;
    // stb_image loads with (0,0) at top-left while OpenGL UV is
    // bottom-left, so flip V.
    FragColor = texture(uPano, vec2(u, 1.0 - v));
}
)";

// ---------------------------------------------------------------------------
// Anisotropic Gaussian splat (EWA splatting, Zwicker 2001 — same approach
// Spark / 3DGS reference renderers use). Each splat carries a 3D rotation
// (quaternion) and per-axis scales; the vertex shader builds the 3D
// covariance, projects it through the perspective Jacobian to a 2D
// screen-space ellipse, and places an instanced quad at the splat's NDC
// position with extents along the cov2D eigenvectors. The fragment shader
// does the standard Gaussian falloff exp(-½‖z‖²) over the quad's local
// UV in std-dev units.
//
// Instance attributes:
//   location 0 (vec2)  : aQuadUV    — unit-quad corner (-1..+1)
//   location 1 (vec3)  : aSplatPos
//   location 2 (vec3)  : aSplatScale  (per-axis, splat-local units)
//   location 3 (vec4)  : aSplatQuat   (xyz, w)
//   location 4 (vec4)  : aSplatRgba   (0..1)
// ---------------------------------------------------------------------------
// Splat renderer ported from marble_viewer/native — texture-packed
// per-splat data + an ordering texture for sort indirection. The
// per-frame upload is just N × 4 bytes (the index list) instead of
// N × 56 bytes the old vertex-attribute layout pushed every frame.
// EWA covariance projection follows Spark's splatVertex.glsl line
// for line; full alpha curve (Mip-Splatting AA pre-blur, eigendecomp,
// pixel-space ellipse) lives here.
const char* splat_vs_src = GLSL_SPLAT_VERSION R"(
layout(location = 0) in vec2 aQuadUV;

uniform mat4  uView;
uniform mat4  uProjection;
uniform mat4  uModel;
uniform float uModelScale;
uniform vec2  uRenderSize;
uniform float uMaxStdDev;
uniform float uMaxPixelRadius;
uniform float uFalloff;
uniform bool  uEnable2DGS;
uniform bool  uLogDepth;

uniform highp usampler2D uSplatA;
uniform highp usampler2D uSplatB;
uniform highp usampler2D uOrdering;
uniform int   uTexWidth;

// Debug shadow input — fed by the renderer only while the D-key
// light-positioning mode is active. uShadowEnabled is 1.0 in that
// mode and 0.0 otherwise.
//
// Shadow casting model: the table top is a perfect square in world
// space (XZ ∈ [−7, +7] at Y = TABLE_TOP_Y). Instead of sampling a
// depth map and inheriting its 3D precision artefacts, the splat
// shader traces a ray from the splat's world position toward the
// light and tests whether it crosses the table-top rectangle. This
// gives a clean polygonal shadow with no acne / no peter-panning
// regardless of light angle.
uniform float uShadowEnabled;
uniform vec3  uShadowLightDir;        // normalized, world space
uniform float uShadowTableTopY;
uniform vec2  uShadowTableHalfExtent; // XZ half-extents of the table top

out vec2  vSplatUv;
out vec4  vColor;
flat out float vAdjStdDev;
flat out float vSplatShadow;

mat3 rot_from_quat(vec4 q) {
    float x = q.x, y = q.y, z = q.z, w = q.w;
    return mat3(
        1.0 - 2.0*(y*y + z*z), 2.0*(x*y + z*w),       2.0*(x*z - y*w),
        2.0*(x*y - z*w),       1.0 - 2.0*(x*x + z*z), 2.0*(y*z + x*w),
        2.0*(x*z + y*w),       2.0*(y*z - x*w),       1.0 - 2.0*(x*x + y*y)
    );
}

ivec2 splat_coord(int idx) {
    return ivec2(idx % uTexWidth, idx / uTexWidth);
}

void main() {
    ivec2 oc = splat_coord(gl_InstanceID);
    uint splatIdx = texelFetch(uOrdering, oc, 0).r;
    if (splatIdx == 0xFFFFFFFFu) {
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
        return;
    }
    ivec2 sc = splat_coord(int(splatIdx));
    uvec4 packA = texelFetch(uSplatA, sc, 0);
    uvec4 packB = texelFetch(uSplatB, sc, 0);

    vec2 pXY    = unpackHalf2x16(packA.x);
    vec2 pZqX   = unpackHalf2x16(packA.y);
    vec2 qYZ    = unpackHalf2x16(packA.z);
    vec2 alpha0 = unpackHalf2x16(packA.w);
    vec2 rgRG   = unpackHalf2x16(packB.x);
    vec2 rgB_lsX = unpackHalf2x16(packB.y);
    vec2 lsYZ   = unpackHalf2x16(packB.z);
    vec2 qW0    = unpackHalf2x16(packB.w);

    vec3 center = vec3(pXY,  pZqX.x);
    vec3 rgb    = vec3(rgRG, rgB_lsX.x);
    vec3 scales = exp(vec3(rgB_lsX.y, lsYZ));
    vec4 quat   = vec4(pZqX.y, qYZ, qW0.x);
    vec4 rgba   = vec4(rgb, alpha0.x);

    vec3 worldPos = (uModel * vec4(center, 1.0)).xyz;
    vec4 viewCenter4 = uView * vec4(worldPos, 1.0);
    vec3 viewCenter  = viewCenter4.xyz;
    if (viewCenter.z >= 0.0) {
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
        return;
    }
    bvec3 zeroScales = lessThanEqual(scales, vec3(1e-6));

    {
        vec4 cc = uProjection * vec4(viewCenter, 1.0);
        float clip = 1.4 * cc.w;
        if (abs(cc.x) > clip || abs(cc.y) > clip) {
            gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
            return;
        }
    }

    float a_in = rgba.a * 2.0;
    float adjStdDev = uMaxStdDev;
    if (a_in > 1.0) {
        a_in = min(a_in * 4.0 - 3.0, 2.5);
        adjStdDev = uMaxStdDev + 0.3 * (a_in - 1.0);
    }

    mat3 modelLinear = mat3(uModel);
    mat3 R  = rot_from_quat(quat);
    mat3 RS_local = mat3(R[0] * scales.x, R[1] * scales.y, R[2] * scales.z);
    mat3 RS_world = modelLinear * RS_local;
    mat3 cov3D    = RS_world * transpose(RS_world);

    mat3 V3 = mat3(uView);
    cov3D = V3 * cov3D * transpose(V3);

    vec2 focal = 0.5 * uRenderSize *
                 vec2(uProjection[0][0], uProjection[1][1]);
    float invZ = 1.0 / viewCenter.z;
    vec2 J1 = focal * invZ;
    vec2 J2 = -(J1 * viewCenter.xy) * invZ;
    mat3 J = mat3(
        J1.x, 0.0,  J2.x,
        0.0,  J1.y, J2.y,
        0.0,  0.0,  0.0
    );

    mat3 cov2D3 = transpose(J) * cov3D * J;
    float a0 = cov2D3[0][0];
    float d0 = cov2D3[1][1];
    float b0 = cov2D3[0][1];
    float preBlur = 0.3;
    float a = a0 + preBlur;
    float d = d0 + preBlur;
    float b = b0;
    float detOrig = a * d - b * b;
    float det = detOrig;
    if (det <= 0.0) {
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
        return;
    }
    float blurAdjust = sqrt(max(0.0, detOrig / det));
    vColor = vec4(rgba.rgb, a_in * blurAdjust);

    float mid  = 0.5 * (a + d);
    float disc = sqrt(max(0.0, mid * mid - det));
    float lambda1 = mid + disc;
    float lambda2 = max(0.001, mid - disc);
    vec2 v1 = (abs(b) > 1e-4) ? normalize(vec2(b, lambda1 - a))
            : ((a >= d) ? vec2(1.0, 0.0) : vec2(0.0, 1.0));
    vec2 v2 = vec2(v1.y, -v1.x);

    float r1 = min(uMaxPixelRadius, adjStdDev * sqrt(lambda1));
    float r2 = min(uMaxPixelRadius, adjStdDev * sqrt(lambda2));

    vec4 clipCenter = uProjection * viewCenter4;
    vec3 ndcCenter  = clipCenter.xyz / clipCenter.w;
    vec2 pixelOffset = aQuadUV.x * v1 * r1 + aQuadUV.y * v2 * r2;
    vec2 ndcOffset   = (2.0 / uRenderSize) * pixelOffset;

    if (uEnable2DGS && any(zeroScales)) {
        // Fall through — the regular EWA path produces sensible
        // (slightly thicker) output for 2DGS sheets too.
    }

    vSplatUv   = aQuadUV * adjStdDev;
    vAdjStdDev = adjStdDev;

    // Table-shadow test — debug-only, gated on uShadowEnabled so
    // normal play pays nothing for it. Trace a ray from the splat
    // toward the light; if it crosses the table-top rectangle
    // (axis-aligned XZ box at Y = uShadowTableTopY) the splat is
    // in shadow. Splats above the table top can't be shadowed by it.
    float shadow = 0.0;
    if (uShadowEnabled > 0.5 &&
        worldPos.y < uShadowTableTopY &&
        uShadowLightDir.y > 0.001) {
        float t = (uShadowTableTopY - worldPos.y) / uShadowLightDir.y;
        if (t > 0.0) {
            float hitX = worldPos.x + t * uShadowLightDir.x;
            float hitZ = worldPos.z + t * uShadowLightDir.z;
            if (abs(hitX) <= uShadowTableHalfExtent.x &&
                abs(hitZ) <= uShadowTableHalfExtent.y) {
                shadow = 1.0;
            }
        }
    }
    vSplatShadow = shadow;

    vec3 ndc = vec3(ndcCenter.xy + ndcOffset, ndcCenter.z);
    gl_Position = vec4(ndc.xy * clipCenter.w, clipCenter.zw);

    if (uLogDepth) {
        const float logK = 1.4426950408889634;
        float wd = max(1e-7, gl_Position.w);
        gl_Position.z = (log2(wd) * logK - 1.0) * gl_Position.w;
    }
}
)";

const char* splat_fs_src = GLSL_SPLAT_VERSION GLSL_SPLAT_PREAMBLE R"(
in vec2  vSplatUv;
in vec4  vColor;
flat in float vAdjStdDev;
flat in float vSplatShadow;
out vec4 FragColor;

uniform float uMinAlpha;
uniform float uFalloff;

void main() {
    float z2 = dot(vSplatUv, vSplatUv);
    if (z2 > vAdjStdDev * vAdjStdDev) discard;

    float a;
    if (vColor.a <= 1.0) {
        a = mix(vColor.a, vColor.a * exp(-0.5 * z2), uFalloff);
    } else {
        float k = exp((vColor.a * vColor.a - 1.0) / 2.7182818284);
        float saturating = 1.0 - pow(1.0 - exp(-0.5 * z2), k);
        a = mix(1.0, saturating, uFalloff);
    }
    if (a < uMinAlpha) discard;
    // Darken splats that are in shadow. vSplatShadow is 0 when the
    // D-key light-positioning mode is off, so the multiplication is
    // a no-op for normal play.
    float shadowFactor = 1.0 - vSplatShadow * 0.55;
    FragColor = vec4(vColor.rgb * shadowFactor * a, a);
}
)";

GLuint compile_shader(GLenum type, const char* src) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &src, nullptr);
    glCompileShader(shader);
    int ok;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        const char* stage = (type == GL_VERTEX_SHADER) ? "VERTEX" : "FRAGMENT";
        shader_log_error("compile error", stage, log, src);
    }
    return shader;
}

GLuint create_program(const char* vs_src, const char* fs_src) {
    GLuint vs = compile_shader(GL_VERTEX_SHADER, vs_src);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fs_src);
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    int ok;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetProgramInfoLog(prog, sizeof(log), nullptr, log);
        shader_log_error("link error", nullptr, log, nullptr);
    }
    glDeleteShader(vs);
    glDeleteShader(fs);
    return prog;
}
