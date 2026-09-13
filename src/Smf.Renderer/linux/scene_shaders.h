#pragma once

namespace smf_scene {

    constexpr const char* scene_vertex = R"(#version 420 core
void main() {
    vec2 uv = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    gl_Position = vec4(uv * 2. - 1., 0., 1.);
}
)";

    constexpr const char* scene_fragment = R"(
struct BackgroundView {
    mat4 projection;
    mat4 inverseProjection;
    vec4 delta;
    vec4 size;
};

struct BackgroundGlow {
    vec4 worldOrigin;
    vec4 planetOrigin;
    vec4 sun;
    vec4 mesh;
};

struct ParallaxLayer {
    vec4 x;
    vec4 y;
    vec4 size;
    uvec4 flags;
    vec4 inverseX;
    vec4 inverseZ;
    vec4 liveX;
    vec4 liveY;
    vec4 depth;
};

layout(std140, binding = 0) uniform Constants {
    vec4 rows[4];
    vec4 dimensions[2];
    uvec4 imageFlags[4];
    vec4 values;
    mat4 inverseProjection;
    mat4 projection;
    vec4 cameraDelta;
    BackgroundView backgroundViews[4];
    uvec4 glowCounts;
    BackgroundGlow liveGlows[4];
    BackgroundGlow cachedGlows[4];
    ParallaxLayer layer;
    vec4 liveInverseX;
    vec4 liveInverseZ;
    vec4 liveDepthParameters;
    vec4 outputSize;
};

layout(binding = 0) uniform sampler2D images[15];
layout(location = 0) out vec4 result;
#if defined(NEAREST) || defined(CANDIDATE)
layout(location = 1) out vec4 second;
layout(location = 2) out vec4 third;
#endif

bool finite(float v) { return !isnan(v) && !isinf(v); }
bool finite(vec4 v) { return !any(isnan(v)) && !any(isinf(v)); }
bool inside(vec2 uv) { return all(greaterThanEqual(uv, vec2(0))) && all(lessThanEqual(uv, vec2(1))); }
vec2 pixel() { return vec2(gl_FragCoord.x, outputSize.y - gl_FragCoord.y); }
ivec2 storedPixel() { return ivec2(gl_FragCoord.xy); }
uint flags(int index) { return imageFlags[index / 4][index % 4]; }

vec2 orient(vec2 uv, uint imageFlags) {
    if ((imageFlags & 1u) == 0u) uv.y = 1. - uv.y;
    return uv;
}

vec4 sampleImage(int index, vec2 uv, uint imageFlags) {
    return textureLod(images[index], orient(uv, imageFlags), 0.);
}

vec4 loadImage(int index, ivec2 p, ivec2 size, uint imageFlags) {
    p = clamp(p, ivec2(0), size - 1);
    if ((imageFlags & 1u) == 0u) p.y = size.y - 1 - p.y;
    return texelFetch(images[index], p, 0);
}

vec3 unproject(vec2 uv, float depth, mat4 inverseMatrix) {
    if (values.w != 0.) depth = depth * 2. - 1.;
    vec4 point = inverseMatrix * vec4(uv * vec2(2, -2) + vec2(-1, 1), depth, 1);
    return point.xyz / point.w;
}

vec2 project(vec3 point, mat4 matrix) {
    vec4 clip = matrix * vec4(point, 1);
    return clip.xy / clip.w * vec2(.5, -.5) + .5;
}

vec3 atmosphere(vec3 ray, vec2 delta, bool cached) {
    ray = normalize(ray);
    vec3 color = vec3(0);
    uint count = cached ? glowCounts.y : glowCounts.x;
    for (uint i = 0u; i < count; ++i) {
        BackgroundGlow glow = cached ? cachedGlows[i] : liveGlows[i];
        vec3 mesh = glow.mesh.xyz - vec3(delta, 0);
        float distance = dot(ray, mesh);
        vec3 meshNearest = mesh - ray * distance;
        if (distance <= 0. || dot(meshNearest, meshNearest) >= glow.mesh.w * glow.mesh.w) continue;
        vec3 origin = glow.worldOrigin.xyz - vec3(delta, 0);
        vec3 planet = glow.planetOrigin.xyz - vec3(delta, 0);
        vec3 nearest = ray * dot(ray, planet) - origin;
        float radius = length(nearest);
        if (radius <= 0.) continue;
        float ring = max(1. - abs(radius - glow.worldOrigin.w) / glow.planetOrigin.w, 0.);
        float light = dot(nearest / radius, -glow.sun.xyz) * .5 + .5;
        color += (.28 * light * light + .02) * ring * ring * ring * glow.sun.w * vec3(.706, .839, 1);
    }
    return color;
}

vec4 worldTexel(int index, ivec2 p, ivec2 size, uint imageFlags,
    mat4 inverseMatrix, vec2 delta, bool cached) {
    p = clamp(p, ivec2(0), size - 1);
    vec4 color = loadImage(index, p, size, imageFlags);
    vec3 ray = unproject((vec2(p) + .5) / vec2(size), .5, inverseMatrix);
    color.rgb -= atmosphere(ray, delta, cached);
    return color;
}

vec4 worldSample(int index, vec2 uv, uint imageFlags,
    mat4 inverseMatrix, vec2 delta, bool cached) {
    if ((cached ? glowCounts.y : glowCounts.x) == 0u) return sampleImage(index, uv, imageFlags);
    ivec2 size = textureSize(images[index], 0);
    vec2 p = uv * vec2(size) - .5;
    if ((imageFlags & 2u) == 0u)
        return worldTexel(index, ivec2(floor(p + .5)), size, imageFlags, inverseMatrix, delta, cached);
    ivec2 start = ivec2(floor(p));
    vec2 weight = fract(p);
    vec4 a = worldTexel(index, start, size, imageFlags, inverseMatrix, delta, cached);
    vec4 b = worldTexel(index, start + ivec2(1, 0), size, imageFlags, inverseMatrix, delta, cached);
    vec4 c = worldTexel(index, start + ivec2(0, 1), size, imageFlags, inverseMatrix, delta, cached);
    vec4 d = worldTexel(index, start + ivec2(1, 1), size, imageFlags, inverseMatrix, delta, cached);
    return mix(mix(a, b, weight.x), mix(c, d, weight.x), weight.y);
}

bool surfaceDepth(int index, BackgroundView view, vec2 uv, uint imageFlags, out float depth) {
    depth = 0.;
    if (!inside(uv)) return false;
    vec2 p = uv * view.size.xy - .5;
    ivec2 start = ivec2(floor(p));
    ivec2 size = ivec2(view.size.xy);
    vec4 samples = vec4(loadImage(index, start, size, imageFlags).r,
        loadImage(index, start + ivec2(1, 0), size, imageFlags).r,
        loadImage(index, start + ivec2(0, 1), size, imageFlags).r,
        loadImage(index, start + ivec2(1, 1), size, imageFlags).r);
    float farDepth = values.w != 0. ? 1. : 0.;
    if (!finite(samples) || any(equal(samples, vec4(farDepth)))) return false;
    vec2 weight = fract(p);
    depth = mix(mix(samples.x, samples.y, weight.x), mix(samples.z, samples.w, weight.x), weight.y);
    return true;
}

bool renderView(int colorIndex, int depthIndex, int boundIndex, BackgroundView view, vec3 ray, out vec4 color) {
    color = vec4(0);
    float bound = texelFetch(images[boundIndex], ivec2(0), 0).r;
    if (bound == 0.) return false;
    if (values.w != 0.) bound = 1. - bound;
    float nearDistance = -unproject(vec2(.5), bound, view.inverseProjection).z;
    if (!finite(nearDistance) || nearDistance <= 0.) return false;
    vec2 uv = project(ray * nearDistance + vec3(view.delta.xy, 0), view.projection);
    vec3 viewPoint = vec3(0);
    bool converged = false;
    for (int iteration = 0; iteration < 16; ++iteration) {
        float depth;
        if (!surfaceDepth(depthIndex, view, uv, flags(depthIndex), depth)) return false;
        float distance = -unproject(uv, depth, view.inverseProjection).z;
        if (!finite(distance) || distance <= 0.) return false;
        viewPoint = ray * distance;
        vec2 nextUv = project(viewPoint + vec3(view.delta.xy, 0), view.projection);
        if (all(lessThan(abs(nextUv - uv) * view.size.xy, vec2(.02)))) {
            uv = nextUv;
            converged = true;
            break;
        }
        uv = nextUv;
    }
    if (!converged || !inside(uv)) return false;
    vec3 cachedPoint = viewPoint + vec3(view.delta.xy, 0);
    vec3 normal = vec3(0);
    vec2 offset = 1. / view.size.xy;
    float xDepth, yDepth;
    bool normalValid = surfaceDepth(depthIndex, view, uv + vec2(offset.x, 0), flags(depthIndex), xDepth)
        && surfaceDepth(depthIndex, view, uv + vec2(0, offset.y), flags(depthIndex), yDepth);
    if (normalValid) {
        vec3 xPoint = unproject(uv + vec2(offset.x, 0), xDepth, view.inverseProjection);
        vec3 yPoint = unproject(uv + vec2(0, offset.y), yDepth, view.inverseProjection);
        normal = cross(xPoint - cachedPoint, yPoint - cachedPoint);
        if (dot(normal, -cachedPoint) < 0.) normal = -normal;
        normalValid = dot(normal, normal) > 1e-12;
        if (normalValid && dot(normal, -viewPoint) <= 0.) return false;
    }
    vec3 livePoint = viewPoint + vec3(cameraDelta.xy, 0);
    vec2 liveUv = project(livePoint, projection);
    if (inside(liveUv) && normalValid && dot(normalize(normal), normalize(-livePoint)) > .05)
        color = worldSample(1, liveUv, flags(1), inverseProjection, vec2(0), false);
    else
        color = worldSample(colorIndex, uv, flags(colorIndex), view.inverseProjection,
            cameraDelta.xy - view.delta.xy, true);
    color.rgb += atmosphere(ray, cameraDelta.xy, false);
    return true;
}

struct Coverage {
    vec4 foreground;
    vec4 transmission;
};

float halfRound(float value) { return unpackHalf2x16(packHalf2x16(vec2(value, 0))).x; }

Coverage liveTexel(ivec2 p) {
    ivec2 size = ivec2(dimensions[0].xy);
    vec4 color = loadImage(1, p, size, flags(1));
    vec4 probe = loadImage(2, p, size, flags(2));
    vec4 reference = loadImage(3, p, size, flags(3));
    vec3 contrast = vec3(halfRound(reference.r + 1.), halfRound(reference.g + 1.), halfRound(reference.b + 1.)) - reference.rgb;
    if (!finite(color) || !finite(probe) || !finite(reference) || any(lessThan(abs(contrast), vec3(.5))))
        return Coverage(finite(color) ? color : vec4(0, 0, 0, 1), vec4(0));
    vec4 transmission = vec4((probe.rgb - color.rgb) / contrast, 0);
    return Coverage(vec4(color.rgb - transmission.rgb * reference.rgb, color.a), transmission);
}

Coverage liveCoverage(vec2 uv) {
    vec2 p = uv * dimensions[0].xy - .5;
    if ((flags(1) & 2u) == 0u) return liveTexel(ivec2(floor(p + .5)));
    ivec2 start = ivec2(floor(p));
    vec2 weight = fract(p);
    Coverage a = liveTexel(start);
    Coverage b = liveTexel(start + ivec2(1, 0));
    Coverage c = liveTexel(start + ivec2(0, 1));
    Coverage d = liveTexel(start + ivec2(1, 1));
    return Coverage(mix(mix(a.foreground, b.foreground, weight.x), mix(c.foreground, d.foreground, weight.x), weight.y),
        mix(mix(a.transmission, b.transmission, weight.x), mix(c.transmission, d.transmission, weight.x), weight.y));
}

void surfaceHeight(float depth, vec2 p, vec4 inverseX, vec4 inverseZ, vec4 parameters, out float height, out float error) {
    vec3 position = vec3(p + .5, 1);
    vec3 xTerms = position * inverseX.xyz;
    vec3 zTerms = position * inverseZ.xyz;
    vec4 terms = vec4(dot(xTerms, vec3(1)), dot(zTerms, vec3(1)), depth, 1) * parameters;
    height = dot(terms, vec4(1));
    error = 9.5367431640625e-7 * (dot(abs(terms), vec4(1))
        + abs(parameters.x) * dot(abs(xTerms), vec3(1)) + abs(parameters.y) * dot(abs(zTerms), vec3(1)));
}

bool liveHeight(ivec2 p, float height, float error, bool allowEmpty) {
    p = clamp(p, ivec2(0), ivec2(dimensions[0].xy) - 1);
    float depth = loadImage(4, p, ivec2(dimensions[0].xy), flags(4)).r;
    float farDepth = (flags(4) & 8u) != 0u ? 0. : 1.;
    if (depth == farDepth) return allowEmpty;
    float actual, uncertainty;
    surfaceHeight(depth, vec2(p), liveInverseX, liveInverseZ, liveDepthParameters, actual, uncertainty);
    return finite(actual) && finite(uncertainty) && abs(actual - height) <= error + uncertainty;
}

void main() {
#ifdef REDUCE
    ivec2 start = ivec2(gl_FragCoord.xy) * 16;
    float nearest = 0.;
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            ivec2 p = start + ivec2(x, y);
            if (any(greaterThanEqual(p, ivec2(dimensions[0].xy)))) continue;
            float depth = texelFetch(images[3], p, 0).r;
            if (values.z != 0. && values.w != 0.) depth = 1. - depth;
            if (finite(depth) && depth >= 0. && depth <= 1.) nearest = max(nearest, depth);
        }
    }
    result = vec4(nearest);
#elif defined(BACKGROUND)
    vec2 uv = pixel() / outputSize.xy;
    if (values.z != 0.) { result = sampleImage(1, uv, flags(1)); return; }
    vec3 ray = unproject(uv, .5, inverseProjection);
    ray /= -ray.z;
    if (renderView(3, 4, 11, backgroundViews[0], ray, result)) return;
    if (renderView(5, 6, 12, backgroundViews[1], ray, result)) return;
    if (renderView(7, 8, 13, backgroundViews[2], ray, result)) return;
    if (renderView(9, 10, 14, backgroundViews[3], ray, result)) return;
    result = sampleImage(2, (uv - .5) / values.y + .5, flags(2));
    result.rgb += atmosphere(ray, cameraDelta.xy, false);
#elif defined(MAP_LAYER)
    if (values.y != 0.) { result = loadImage(1, ivec2(pixel()), ivec2(dimensions[0].xy), flags(1)); return; }
    vec3 p = vec3(pixel(), 1);
    vec2 uv = vec2(dot(rows[0].xyz, p), dot(rows[1].xyz, p));
    Coverage coverage;
    if (values.x != 0. && inside(uv)) coverage = liveCoverage(uv);
    else {
        uv = vec2(dot(rows[2].xyz, p), dot(rows[3].xyz, p));
        if (!inside(uv)) uv = .5 / dimensions[1].xy;
        vec4 black = sampleImage(4, uv, flags(4));
        coverage = Coverage(black, sampleImage(5, uv, flags(5)) - black);
    }
    result = coverage.foreground + coverage.transmission * texelFetch(images[0], storedPixel(), 0);
#elif defined(NEAREST)
    result = values.y != 0. ? vec4(0) : texelFetch(images[4], storedPixel(), 0);
    second = values.y != 0. ? vec4(0) : texelFetch(images[5], storedPixel(), 0);
    third = values.y != 0. ? vec4(1) : texelFetch(images[6], storedPixel(), 0);
    vec3 p = vec3(pixel(), 1);
    vec2 uv = vec2(dot(layer.x.xyz, p), dot(layer.y.xyz, p));
    if (!inside(uv)) return;
    ivec2 dp = clamp(ivec2(floor(uv * layer.size.xy)), ivec2(0), ivec2(layer.size.xy) - 1);
    float depth = loadImage(3, dp, ivec2(layer.size.xy), layer.flags.z).r;
    bool reversed = (layer.flags.z & 8u) != 0u;
    bool nearer = result.w == 0. || (reversed ? depth > result.x : depth < result.x);
    bool tied = result.w != 0. && depth == result.x;
    if (!finite(depth) || depth < 0. || depth > 1. || depth == (reversed ? 0. : 1.) || !(nearer || tied)) return;
    vec4 black = sampleImage(1, uv, layer.flags.x);
    vec4 through = sampleImage(2, uv, layer.flags.y) - black;
    if (nearer) {
        result = vec4(depth, values.x, 1, 1);
        second = black;
        third = through;
    } else {
        second = black + through * second;
        third *= through;
        result.z += 1.;
    }
#elif defined(CANDIDATE)
    vec4 selected = texelFetch(images[5], storedPixel(), 0);
    if (selected.w == 0. || selected.z != 1. || selected.y != values.x) discard;
    vec3 p = vec3(pixel(), 1);
    vec2 cachedUv = vec2(dot(layer.x.xyz, p), dot(layer.y.xyz, p));
    ivec2 dp = clamp(ivec2(floor(cachedUv * layer.size.xy)), ivec2(0), ivec2(layer.size.xy) - 1);
    float height, error;
    surfaceHeight(selected.x, vec2(dp), layer.inverseX, layer.inverseZ, layer.depth, height, error);
    vec2 uv = vec2(dot(layer.liveX.xyz, p), dot(layer.liveY.xyz, p));
    ivec2 center = clamp(ivec2(floor(uv * dimensions[0].xy)), ivec2(0), ivec2(dimensions[0].xy) - 1);
    bool matches = finite(height) && finite(error) && inside(uv) && liveHeight(center, height, error, false);
    if (matches && (flags(1) & 2u) != 0u) {
        ivec2 start = ivec2(floor(uv * dimensions[0].xy - .5));
        matches = liveHeight(start, height, error, true) && liveHeight(start + ivec2(1, 0), height, error, true)
            && liveHeight(start + ivec2(0, 1), height, error, true) && liveHeight(start + ivec2(1, 1), height, error, true);
    }
    float liveDepth = loadImage(4, center, ivec2(dimensions[0].xy), flags(4)).r;
    surfaceHeight(liveDepth, vec2(center), liveInverseX, liveInverseZ, liveDepthParameters, height, error);
    Coverage live = liveCoverage(uv);
    result = live.foreground;
    second = live.transmission;
    third = vec4(height, error, selected.y, matches ? 1 : 0);
#elif defined(AMBIGUITY)
    result = texelFetch(images[0], storedPixel(), 0);
    if (result.w == 0. || result.z == values.x) return;
    vec3 p = vec3(pixel(), 1);
    vec2 uv = vec2(dot(layer.x.xyz, p), dot(layer.y.xyz, p));
    if (!inside(uv)) return;
    ivec2 dp = clamp(ivec2(floor(uv * layer.size.xy)), ivec2(0), ivec2(layer.size.xy) - 1);
    float depth = loadImage(3, dp, ivec2(layer.size.xy), layer.flags.z).r;
    float farDepth = (layer.flags.z & 8u) != 0u ? 0. : 1.;
    if (depth != farDepth) {
        float height, error;
        surfaceHeight(depth, vec2(dp), layer.inverseX, layer.inverseZ, layer.depth, height, error);
        if (!finite(height) || abs(height - result.x) <= error + result.y) result.w = 0.;
    }
#elif defined(PARALLAX_RESOLVE)
    ivec2 p = storedPixel();
    bool live = values.x != 0. && texelFetch(images[5], p, 0).w != 0.;
    vec4 foreground = live ? texelFetch(images[3], p, 0) : texelFetch(images[1], p, 0);
    vec4 through = live ? texelFetch(images[4], p, 0) : texelFetch(images[2], p, 0);
    result = foreground + through * texelFetch(images[0], p, 0);
#elif defined(CORRECTION)
    vec4 color = texelFetch(images[0], storedPixel(), 0);
    vec3 corrected = vec3(sampleImage(1, vec2(color.r, .125), flags(1)).r,
        sampleImage(1, vec2(color.g, .375), flags(1)).g, sampleImage(1, vec2(color.b, .625), flags(1)).b);
    float luma = dot(corrected, vec3(.220, .707, .071));
    result = vec4(luma + values.x * (corrected - luma), color.a);
#elif defined(ADDITIVE)
    vec4 color = texelFetch(images[0], storedPixel(), 0);
    vec3 p = vec3(pixel(), 1);
    vec2 uv = vec2(dot(rows[0].xyz, p), dot(rows[1].xyz, p));
    vec3 addition = vec3(0);
    if (inside(uv)) addition = sampleImage(1, uv, flags(1)).rgb;
    else {
        uv = vec2(dot(rows[2].xyz, p), dot(rows[3].xyz, p));
        if (inside(uv)) addition = sampleImage(4, uv, flags(4)).rgb;
    }
    result = vec4(color.rgb + addition, 1);
#endif
}
)";

}
