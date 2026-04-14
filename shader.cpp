#include "shader.h"

#include <cstdio>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

// ---------------------------------------------------------------------------
// GLSL version selector. Desktop OpenGL gets core profile 3.30; Emscripten
// (WebGL 2) gets GLSL ES 3.00, which requires explicit precision qualifiers.
// ---------------------------------------------------------------------------
#ifdef __EMSCRIPTEN__
#define GLSL_VERSION    "#version 300 es\n"
#define GLSL_FS_PREAMBLE "precision highp float;\nprecision highp int;\nprecision highp sampler2D;\n"
#else
#define GLSL_VERSION    "#version 330 core\n"
#define GLSL_FS_PREAMBLE ""
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

uniform mat4 uModel;
uniform mat4 uView;
uniform mat4 uProjection;
uniform mat3 uNormalMat;
uniform mat4 uLightSpaceMatrix;

out vec3 vNormal;
out vec3 vFragPos;
out vec3 vWorldPos;
out vec4 vLightSpacePos;

void main() {
    vec4 worldPos = uModel * vec4(aPos, 1.0);
    vFragPos = worldPos.xyz;
    vWorldPos = worldPos.xyz;
    vNormal = normalize(uNormalMat * aNormal);
    vLightSpacePos = uLightSpaceMatrix * worldPos;
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

    // Base gradient: bright warm room
    vec3 ceiling  = vec3(1.8, 1.7, 1.5);  // bright warm white above
    vec3 horizon  = vec3(0.6, 0.55, 0.50); // warm gray walls
    vec3 floor_c  = vec3(0.25, 0.22, 0.18); // warm floor (bounce light)

    vec3 env;
    if (y > 0.0) {
        env = mix(horizon, ceiling, smoothstep(0.0, 0.8, y));
    } else {
        env = mix(horizon, floor_c, smoothstep(0.0, -0.5, y));
    }

    // Soft area light overhead (large softbox)
    float lightDist1 = length(dir - normalize(vec3(0.3, 0.95, 0.2)));
    env += vec3(4.0, 3.8, 3.4) * exp(-lightDist1 * lightDist1 * 6.0);

    // Secondary fill from the side
    float lightDist2 = length(dir - normalize(vec3(-0.6, 0.7, -0.4)));
    env += vec3(2.0, 2.1, 2.4) * exp(-lightDist2 * lightDist2 * 10.0);

    // Warm bounce from below (floor reflection)
    float lightDist3 = length(dir - normalize(vec3(0.0, -0.3, 0.5)));
    env += vec3(0.8, 0.6, 0.4) * exp(-lightDist3 * lightDist3 * 12.0);

    // Back fill (reduces dark shadows on far side of pieces)
    float lightDist4 = length(dir - normalize(vec3(0.0, 0.4, -0.8)));
    env += vec3(1.0, 0.95, 0.85) * exp(-lightDist4 * lightDist4 * 12.0);

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

    if (uWoodEffect != 0) {
        albedo = woodColor(albedo, vWorldPos);
        // Wood grain affects roughness slightly
        float grainVar = fbm(vWorldPos.xz * 20.0) * 0.1;
        roughness = clamp(roughness + grainVar, 0.05, 1.0);
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

        // Apply shadow to key light and partially to fill
        if (i == 0) radiance *= (1.0 - shadow * 0.85);
        if (i == 1) radiance *= (1.0 - shadow * 0.3);

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
    // Scale down for rough/dark surfaces to prevent blow-out
    vec3 specularIBL = envSpec * F_env * (1.0 - roughness) * 0.4;

    // Darken ambient slightly in shadowed areas (contact shadow approx)
    float ambientShadow = 1.0 - shadow * 0.25;
    vec3 ambient = (diffuseIBL + specularIBL) * ao * ambientShadow;

    vec3 color = ambient + Lo;

    // --- Tone mapping (ACES filmic) ---
    color = color * 0.75; // exposure
    vec3 x = color;
    color = (x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14);

    // Gamma correction
    color = pow(clamp(color, 0.0, 1.0), vec3(1.0 / 2.2));

    FragColor = vec4(color, 1.0);
}
)";

// ---------------------------------------------------------------------------
// Highlight shaders (for selection halo and valid move indicators)
// ---------------------------------------------------------------------------
const char* highlight_vs_src = GLSL_VERSION R"(
layout(location = 0) in vec3 aPos;

uniform mat4 uMVP;

out vec2 vLocalPos;

void main() {
    vLocalPos = aPos.xz;
    gl_Position = uMVP * vec4(aPos, 1.0);
}
)";

const char* highlight_fs_src = GLSL_VERSION GLSL_FS_PREAMBLE R"(
in vec2 vLocalPos;

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

void main() {
    // Flat mode: when both radii are 0, draw solid color (for UI overlays)
    if (uInnerRadius <= 0.0 && uOuterRadius <= 0.0) {
        if (uUseGradient != 0) {
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
