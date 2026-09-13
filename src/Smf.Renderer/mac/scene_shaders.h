#pragma once

namespace mac {

    const char scene_shader_source[] = R"metal(
#include <metal_stdlib>
using namespace metal;


// Explicit bindings preserve individual resource slots across Metal drivers.
#define SCENE_ARGUMENTS \
    texture2d<float> image0 [[texture(0)]], \
    texture2d<float> image1 [[texture(1)]], \
    texture2d<float> image2 [[texture(2)]], \
    texture2d<float> image3 [[texture(3)]], \
    texture2d<float> image4 [[texture(4)]], \
    texture2d<float> image5 [[texture(5)]], \
    texture2d<float> image6 [[texture(6)]], \
    texture2d<float> image7 [[texture(7)]], \
    texture2d<float> image8 [[texture(8)]], \
    texture2d<float> image9 [[texture(9)]], \
    texture2d<float> image10 [[texture(10)]], \
    texture2d<float> image11 [[texture(11)]], \
    texture2d<float> image12 [[texture(12)]], \
    texture2d<float> image13 [[texture(13)]], \
    texture2d<float> image14 [[texture(14)]], \
    texture2d<float> image15 [[texture(15)]], \
    texture2d<float> image16 [[texture(16)]], \
    texture2d<float> image17 [[texture(17)]], \
    texture2d<float> image18 [[texture(18)]], \
    texture2d<float> image19 [[texture(19)]], \
    texture2d<float> image20 [[texture(20)]], \
    texture2d<float> image21 [[texture(21)]], \
    texture2d<float> image22 [[texture(22)]], \
    texture2d<float> image23 [[texture(23)]], \
    texture2d<float> image24 [[texture(24)]], \
    texture2d<float> image25 [[texture(25)]], \
    texture2d<float> image26 [[texture(26)]], \
    texture2d<float> image27 [[texture(27)]], \
    texture2d<float> image28 [[texture(28)]], \
    texture2d<float> image29 [[texture(29)]], \
    texture2d<float> image30 [[texture(30)]], \
    texture2d<float> image31 [[texture(31)]], \
    texture2d<float> image32 [[texture(32)]], \
    texture2d<float> image33 [[texture(33)]], \
    texture2d<float> image34 [[texture(34)]], \
    texture2d<float> image35 [[texture(35)]], \
    texture2d<float> image36 [[texture(36)]], \
    texture2d<float> image37 [[texture(37)]], \
    texture2d<float> image38 [[texture(38)]], \
    texture2d<float> image39 [[texture(39)]], \
    texture2d<float> image40 [[texture(40)]], \
    texture2d<float> image41 [[texture(41)]], \
    texture2d<float> image42 [[texture(42)]], \
    texture2d<float> image43 [[texture(43)]], \
    texture2d<float> image44 [[texture(44)]], \
    texture2d<float> image45 [[texture(45)]], \
    texture2d<float> image46 [[texture(46)]], \
    texture2d<float> image47 [[texture(47)]], \
    texture2d<float> image48 [[texture(48)]], \
    texture2d<float> image49 [[texture(49)]], \
    texture2d<float> image50 [[texture(50)]], \
    texture2d<float> image51 [[texture(51)]], \
    texture2d<float> image52 [[texture(52)]], \
    texture2d<float> image53 [[texture(53)]], \
    texture2d<float> image54 [[texture(54)]], \
    texture2d<float> image55 [[texture(55)]], \
    texture2d<float> image56 [[texture(56)]], \
    texture2d<float> image57 [[texture(57)]], \
    texture2d<float> image58 [[texture(58)]], \
    texture2d<float> image59 [[texture(59)]], \
    texture2d<float> image60 [[texture(60)]], \
    texture2d<float> image61 [[texture(61)]], \
    texture2d<float> image62 [[texture(62)]], \
    texture2d<float> image63 [[texture(63)]], \
    sampler sample0 [[sampler(0)]], \
    sampler sample1 [[sampler(1)]], \
    sampler sample2 [[sampler(2)]], \
    sampler sample3 [[sampler(3)]], \
    sampler sample4 [[sampler(4)]], \
    sampler sample5 [[sampler(5)]], \
    sampler sample6 [[sampler(6)]], \
    sampler sample7 [[sampler(7)]], \
    sampler sample8 [[sampler(8)]], \
    sampler sample9 [[sampler(9)]], \
    sampler sample10 [[sampler(10)]], \
    sampler sample11 [[sampler(11)]], \
    sampler sample12 [[sampler(12)]], \
    sampler sample13 [[sampler(13)]], \
    sampler sample14 [[sampler(14)]], \
    sampler sample15 [[sampler(15)]]

#define SCENE_RESOURCES \
    array<texture2d<float>, 64> images = { \
        image0, image1, image2, image3, image4, image5, image6, image7, \
        image8, image9, image10, image11, image12, image13, image14, image15, \
        image16, image17, image18, image19, image20, image21, image22, image23, \
        image24, image25, image26, image27, image28, image29, image30, image31, \
        image32, image33, image34, image35, image36, image37, image38, image39, \
        image40, image41, image42, image43, image44, image45, image46, image47, \
        image48, image49, image50, image51, image52, image53, image54, image55, \
        image56, image57, image58, image59, image60, image61, image62, image63 \
    }; \
    array<sampler, 16> samplers = { \
        sample0, sample1, sample2, sample3, sample4, sample5, sample6, sample7, \
        sample8, sample9, sample10, sample11, sample12, sample13, sample14, sample15 \
    };

struct BackgroundView
{
    float4x4 projection;
    float4x4 inverseProjection;
    float4 delta;
    float4 size;
};

struct ParallaxLayer
{
    float4 x;
    float4 y;
    float4 size;
    uint4 flags;
    float4 inverseX;
    float4 inverseZ;
    float4 liveX;
    float4 liveY;
    float4 depth;
};

struct BackgroundGlow
{
    float4 worldOrigin;
    float4 planetOrigin;
    float4 sun;
    float4 mesh;
};

struct Constants
{
    float4 rows[4];
    float4 dimensions[2];
    uint4 imageFlags[4];
    float4 values;
    float4x4 inverseProjection;
    float4x4 projection;
    float4 cameraDelta;
    BackgroundView backgroundViews[4];
    ParallaxLayer parallaxLayers[16];
    float4 liveInverseX;
    float4 liveInverseZ;
    float4 liveDepthParameters;
    uint4 glowCounts;
    BackgroundGlow liveGlows[4];
    BackgroundGlow cachedGlows[4];
};

struct Vertex
{
    float4 position [[position]];
    float2 uv;
};

vertex Vertex sceneVertex(uint index [[vertex_id]])
{
    Vertex output;
    output.uv = float2((index << 1) & 2, index & 2);
    output.position = float4(output.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return output;
}

float2 orient(float2 uv, uint flags)
{
    if ((flags & 1) != 0) uv.y = 1 - uv.y;
    return uv;
}

bool inside(float2 uv)
{
    return all(uv >= 0) && all(uv <= 1);
}

float4 sampleImage(texture2d<float> image, sampler imageSampler, float2 uv, uint flags)
{
    return image.sample(imageSampler, orient(uv, flags), level(0));
}

float4 loadImage(texture2d<float> image, int2 pixel, int2 size, uint flags)
{
    pixel = clamp(pixel, int2(0, 0), size - 1);
    if ((flags & 1) != 0) pixel.y = size.y - 1 - pixel.y;
    return image.read(uint2(pixel));
}

float3 unproject(constant Constants& u, float2 uv, float depth, float4x4 inverseMatrix)
{
    if (u.values.w != 0) depth = depth * 2 - 1;
    float4 viewPoint = (inverseMatrix * float4(uv * float2(2, -2) + float2(-1, 1), depth, 1));
    return viewPoint.xyz / viewPoint.w;
}

float2 project(float3 viewPoint, float4x4 projectionMatrix)
{
    float4 clip = (projectionMatrix * float4(viewPoint, 1));
    return clip.xy / clip.w * float2(.5, -.5) + .5;
}

float3 atmosphere(constant Constants& u, float3 ray, float2 delta, bool cached)
{
    ray = normalize(ray);
    float3 result = 0;
    uint count = cached ? u.glowCounts.y : u.glowCounts.x;
    for (uint i = 0; i < count; ++i)
    {
        BackgroundGlow glow = u.liveGlows[i];
        if (cached) glow = u.cachedGlows[i];
        float3 mesh = glow.mesh.xyz - float3(delta, 0);
        float distance = dot(ray, mesh);
        float3 meshNearest = mesh - ray * distance;
        if (distance <= 0 || dot(meshNearest, meshNearest) >= glow.mesh.w * glow.mesh.w)
            continue;

        float3 origin = glow.worldOrigin.xyz - float3(delta, 0);
        float3 planet = glow.planetOrigin.xyz - float3(delta, 0);
        float3 nearest = ray * dot(ray, planet) - origin;
        float radius = length(nearest);
        if (radius <= 0) continue;
        float ring = max(1 - abs(radius - glow.worldOrigin.w) / glow.planetOrigin.w, 0.0f);
        float light = dot(nearest / radius, -glow.sun.xyz) * .5 + .5;
        result += (.28 * light * light + .02) * ring * ring * ring * glow.sun.w * float3(.706, .839, 1);
    }
    return all(isfinite(result)) ? result : float3(0);
}

float4 worldTexel(constant Constants& u, texture2d<float> image, int2 pixel, int2 size, uint flags,
    float4x4 inverseMatrix, float2 delta, bool cached)
{
    pixel = clamp(pixel, int2(0), size - 1);
    float4 color = loadImage(image, pixel, size, flags);
    float3 ray = unproject(u, (float2(pixel) + .5) / float2(size), .5, inverseMatrix);
    color.rgb -= atmosphere(u, ray, delta, cached);
    return color;
}

float4 worldSample(constant Constants& u, texture2d<float> image, sampler imageSampler, float2 uv, uint flags,
    float4x4 inverseMatrix, float2 delta, bool cached)
{
    if ((cached ? u.glowCounts.y : u.glowCounts.x) == 0) return sampleImage(image, imageSampler, uv, flags);
    int2 size = int2(image.get_width(), image.get_height());
    float2 pixel = uv * float2(size) - .5;
    float4 result = worldTexel(u, image, int2(floor(pixel + .5)), size, flags, inverseMatrix, delta, cached);
    if ((flags & 2) != 0)
    {
        int2 start = int2(floor(pixel));
        float2 weight = fract(pixel);
        float4 a = worldTexel(u, image, start, size, flags, inverseMatrix, delta, cached);
        float4 b = worldTexel(u, image, start + int2(1, 0), size, flags, inverseMatrix, delta, cached);
        float4 c = worldTexel(u, image, start + int2(0, 1), size, flags, inverseMatrix, delta, cached);
        float4 d = worldTexel(u, image, start + int2(1, 1), size, flags, inverseMatrix, delta, cached);
        result = mix(mix(a, b, weight.x), mix(c, d, weight.x), weight.y);
    }
    return result;
}

bool surfaceDepth(constant Constants& u, texture2d<float> depthImage, BackgroundView view, float2 uv, uint flags, thread float& depth)
{
    depth = 0;
    if (!inside(uv)) return false;
    float2 pixel = uv * view.size.xy - .5;
    int2 start = int2(floor(pixel));
    int2 size = int2(view.size.xy);
    float4 samples = float4(loadImage(depthImage, start, size, flags).r,
        loadImage(depthImage, start + int2(1, 0), size, flags).r,
        loadImage(depthImage, start + int2(0, 1), size, flags).r,
        loadImage(depthImage, start + int2(1, 1), size, flags).r);
    float farDepth = u.values.w != 0 ? 1 : 0;
    if (!all(isfinite(samples)) || any(samples == farDepth)) return false;
    float2 weight = fract(pixel);
    depth = mix(mix(samples.x, samples.y, weight.x), mix(samples.z, samples.w, weight.x), weight.y);
    return true;
}

bool renderView(constant Constants& u, array<texture2d<float>, 64> images, array<sampler, 16> samplers, texture2d<float> worldColor, sampler worldSampler, uint colorFlags,
    texture2d<float> worldDepth, uint depthFlags, texture2d<float> nearestDepth,
    BackgroundView view, float3 ray, thread float4& output)
{
    output = 0;
    float bound = nearestDepth.read(uint2(0)).r;
    if (bound == 0) return false;
    if (u.values.w != 0) bound = 1 - bound;
    float nearDistance = -unproject(u, .5, bound, view.inverseProjection).z;
    if (!isfinite(nearDistance) || nearDistance <= 0) return false;
    float2 uv = project(ray * nearDistance + float3(view.delta.xy, 0), view.projection);
    float3 viewPoint = 0;
    bool converged = false;

    for (int iteration = 0; iteration < 16; ++iteration)
    {
        float depth;
        if (!surfaceDepth(u, worldDepth, view, uv, depthFlags, depth)) return false;
        float distance = -unproject(u, uv, depth, view.inverseProjection).z;
        if (!isfinite(distance) || distance <= 0) return false;
        viewPoint = ray * distance;
        float2 nextUv = project(viewPoint + float3(view.delta.xy, 0), view.projection);
        if (all(abs(nextUv - uv) * view.size.xy < .02))
        {
            uv = nextUv;
            converged = true;
            break;
        }
        uv = nextUv;
    }
    if (!converged || !inside(uv)) return false;

    float3 cachedPoint = viewPoint + float3(view.delta.xy, 0);
    float3 normal = 0;
    float2 offset = 1 / view.size.xy;
    float xDepth, yDepth;
    bool normalValid = surfaceDepth(u, worldDepth, view, uv + float2(offset.x, 0), depthFlags, xDepth)
        && surfaceDepth(u, worldDepth, view, uv + float2(0, offset.y), depthFlags, yDepth);
    if (normalValid)
    {
        float3 xPoint = unproject(u, uv + float2(offset.x, 0), xDepth, view.inverseProjection);
        float3 yPoint = unproject(u, uv + float2(0, offset.y), yDepth, view.inverseProjection);
        normal = cross(xPoint - cachedPoint, yPoint - cachedPoint);
        if (dot(normal, -cachedPoint) < 0) normal = -normal;
        normalValid = dot(normal, normal) > 1e-12;
        if (normalValid && dot(normal, -viewPoint) <= 0) return false;
    }
    float3 livePoint = viewPoint + float3(u.cameraDelta.xy, 0);
    float2 liveUv = project(livePoint, u.projection);
    if (inside(liveUv) && normalValid && dot(normalize(normal), normalize(-livePoint)) > .05)
        output = worldSample(u, images[1], samplers[1], liveUv, u.imageFlags[0].y, u.inverseProjection, 0, false);
    else
        output = worldSample(u, worldColor, worldSampler, uv, colorFlags,
            view.inverseProjection, u.cameraDelta.xy - view.delta.xy, true);
    output.rgb += atmosphere(u, ray, u.cameraDelta.xy, false);
    return true;
}

fragment float4 backgroundPixel(Vertex input [[stage_in]], constant Constants& u [[buffer(0)]],
    SCENE_ARGUMENTS)
{
    SCENE_RESOURCES
    if (u.values.z != 0)
        return sampleImage(images[1], samplers[1], input.uv, u.imageFlags[0].y);
    float3 ray = unproject(u, input.uv, .5, u.inverseProjection);
    ray /= -ray.z;
    float4 output = 0;
    if (u.values.x > 0 && renderView(u, images, samplers, images[3], samplers[3], u.imageFlags[0].w,
        images[4], u.imageFlags[1].x, images[11], u.backgroundViews[0], ray, output)) return output;
    if (u.values.x > 1 && renderView(u, images, samplers, images[5], samplers[5], u.imageFlags[1].y,
        images[6], u.imageFlags[1].z, images[12], u.backgroundViews[1], ray, output)) return output;
    if (u.values.x > 2 && renderView(u, images, samplers, images[7], samplers[7], u.imageFlags[1].w,
        images[8], u.imageFlags[2].x, images[13], u.backgroundViews[2], ray, output)) return output;
    if (u.values.x > 3 && renderView(u, images, samplers, images[9], samplers[9], u.imageFlags[2].y,
        images[10], u.imageFlags[2].z, images[14], u.backgroundViews[3], ray, output)) return output;
    output = sampleImage(images[2], samplers[2], (input.uv - .5) / u.values.y + .5, u.imageFlags[0].z);
    output.rgb += atmosphere(u, ray, u.cameraDelta.xy, false);
    return output;
}

struct Coverage
{
    float4 foreground;
    float4 transmission;
};

Coverage liveTexel(constant Constants& u, array<texture2d<float>, 64> images, array<sampler, 16> samplers, int2 pixel)
{
    int2 size = int2(u.dimensions[0].xy);
    float4 color = loadImage(images[1], pixel, size, u.imageFlags[0].y);
    float4 probe = loadImage(images[2], pixel, size, u.imageFlags[0].z);
    float4 reference = loadImage(images[3], pixel, size, u.imageFlags[0].w);
    float3 contrast = float3(half3(reference.rgb + 1)) - reference.rgb;
    Coverage output = Coverage{};
    if (!all(isfinite(color)) || !all(isfinite(probe)) || !all(isfinite(reference)) ||
        any(abs(contrast) < .5))
    {
        output.transmission = 0;
        output.foreground = all(isfinite(color)) ? color : float4(0, 0, 0, 1);
        return output;
    }
    output.transmission = float4((probe.rgb - color.rgb) / contrast, 0);
    output.foreground = float4(color.rgb - output.transmission.rgb * reference.rgb, color.a);
    return output;
}

Coverage liveCoverage(constant Constants& u, array<texture2d<float>, 64> images, array<sampler, 16> samplers, float2 uv)
{
    float2 pixel = uv * u.dimensions[0].xy - .5;
    Coverage output = Coverage{};
    if ((u.imageFlags[0].y & 2) == 0)
    {
        output = liveTexel(u, images, samplers, int2(floor(pixel + .5)));
    }
    else
    {
        int2 start = int2(floor(pixel));
        float2 weight = fract(pixel);
        Coverage a = liveTexel(u, images, samplers, start);
        Coverage b = liveTexel(u, images, samplers, start + int2(1, 0));
        Coverage c = liveTexel(u, images, samplers, start + int2(0, 1));
        Coverage d = liveTexel(u, images, samplers, start + int2(1, 1));
        output.foreground = mix(mix(a.foreground, b.foreground, weight.x),
            mix(c.foreground, d.foreground, weight.x), weight.y);
        output.transmission = mix(mix(a.transmission, b.transmission, weight.x),
            mix(c.transmission, d.transmission, weight.x), weight.y);
    }
    return output;
}

fragment float4 layerPixel(Vertex input [[stage_in]], constant Constants& u [[buffer(0)]],
    SCENE_ARGUMENTS)
{
    SCENE_RESOURCES
    if (u.values.y != 0)
        return loadImage(images[1], int2(input.position.xy), int2(u.dimensions[0].xy), u.imageFlags[0].y);
    float3 pixel = float3(input.position.xy, 1);
    float2 uv = float2(dot(u.rows[0].xyz, pixel), dot(u.rows[1].xyz, pixel));
    Coverage coverage = Coverage{};
    if (u.values.x != 0 && inside(uv))
    {
        coverage = liveCoverage(u, images, samplers, uv);
    }
    else
    {
        uv = float2(dot(u.rows[2].xyz, pixel), dot(u.rows[3].xyz, pixel));
        if (!inside(uv)) uv = .5 / u.dimensions[1].xy;
        float4 black = sampleImage(images[4], samplers[4], uv, u.imageFlags[1].x);
        float4 white = sampleImage(images[5], samplers[5], uv, u.imageFlags[1].y);
        coverage.foreground = black;
        coverage.transmission = white - black;
    }
    float4 background = images[0].read(uint2(input.position.xy));
    return coverage.foreground + coverage.transmission * background;
}

void surfaceHeight(float depth, float2 pixel, float4 inverseX, float4 inverseZ,
    float4 parameters, thread float& height, thread float& error)
{
    float3 position = float3(pixel + .5, 1);
    float3 xTerms = position * inverseX.xyz;
    float3 zTerms = position * inverseZ.xyz;
    float x = dot(xTerms, float3(1));
    float z = dot(zTerms, float3(1));
    float4 terms = float4(x, z, depth, 1) * parameters;
    height = dot(terms, float4(1));
    error = 9.5367431640625e-7 * (dot(abs(terms), float4(1))
        + abs(parameters.x) * dot(abs(xTerms), float3(1)) + abs(parameters.y) * dot(abs(zTerms), float3(1)));
}

bool liveHeight(constant Constants& u, array<texture2d<float>, 64> images, array<sampler, 16> samplers, int2 pixel, float height, float error, bool allowEmpty)
{
    pixel = clamp(pixel, 0, int2(u.dimensions[0].xy) - 1);
    float depth = loadImage(images[4], pixel, int2(u.dimensions[0].xy), u.imageFlags[1].x).r;
    float farDepth = (u.imageFlags[1].x & 8) != 0 ? 0 : 1;
    if (depth == farDepth) return allowEmpty;
    float actual, uncertainty;
    surfaceHeight(depth, float2(pixel), u.liveInverseX, u.liveInverseZ, u.liveDepthParameters, actual, uncertainty);
    return isfinite(actual) && isfinite(uncertainty) && abs(actual - height) <= error + uncertainty;
}


fragment float4 parallaxPixel(Vertex input [[stage_in]], constant Constants& u [[buffer(0)]],
    SCENE_ARGUMENTS)
{
    SCENE_RESOURCES
    float nearest = 0;
    bool found = false;
    float4 foreground = 0;
    float4 transmission = 1;
    uint selected = 0;
    uint ties = 0;
    float selectedHeight = 0;
    float selectedError = 0;

    for (uint i = 0; i < 16; ++i)
    {
        if (i < uint(u.values.x))
        {
            ParallaxLayer layer = u.parallaxLayers[i];
            float3 pixel = float3(input.position.xy, 1);
            float2 uv = float2(dot(layer.x.xyz, pixel), dot(layer.y.xyz, pixel));
            if (inside(uv))
            {
                int2 depthPixel = clamp(int2(floor(uv * layer.size.xy)), 0, int2(layer.size.xy) - 1);
                float depth = loadImage(images[16 + i * 3 + 2], depthPixel, int2(layer.size.xy), layer.flags.z).r;
                bool reversed = (layer.flags.z & 8) != 0;
                float farDepth = reversed ? 0 : 1;
                bool nearer = !found || (reversed ? depth > nearest : depth < nearest);
                bool tied = found && depth == nearest;
                if (isfinite(depth) && depth >= 0 && depth <= 1 && depth != farDepth && (nearer || tied))
                {
                    float4 black = (layer.flags.x & 2) != 0
                        ? sampleImage(images[16 + i * 3], samplers[14], uv, layer.flags.x)
                        : sampleImage(images[16 + i * 3], samplers[15], uv, layer.flags.x);
                    float4 white = (layer.flags.y & 2) != 0
                        ? sampleImage(images[16 + i * 3 + 1], samplers[14], uv, layer.flags.y)
                        : sampleImage(images[16 + i * 3 + 1], samplers[15], uv, layer.flags.y);
                    float4 through = white - black;
                    if (nearer)
                    {
                        nearest = depth;
                        foreground = black;
                        transmission = through;
                        found = true;
                        selected = i;
                        ties = 1;
                        surfaceHeight(depth, float2(depthPixel), layer.inverseX, layer.inverseZ, layer.depth, selectedHeight, selectedError);
                    }
                    else
                    {
                        foreground = black + through * foreground;
                        transmission *= through;
                        ++ties;
                    }
                }
            }
        }
    }
    if (found && ties == 1 && u.values.y != 0)
    {
        ParallaxLayer layer = u.parallaxLayers[selected];
        float3 pixel = float3(input.position.xy, 1);
        float2 uv = float2(dot(layer.liveX.xyz, pixel), dot(layer.liveY.xyz, pixel));
        int2 center = clamp(int2(floor(uv * u.dimensions[0].xy)), 0, int2(u.dimensions[0].xy) - 1);
        bool matches = isfinite(selectedHeight) && isfinite(selectedError) && inside(uv)
            && liveHeight(u, images, samplers, center, selectedHeight, selectedError, false);
        float liveDepth = loadImage(images[4], center, int2(u.dimensions[0].xy), u.imageFlags[1].x).r;
        float height, error;
        surfaceHeight(liveDepth, float2(center), u.liveInverseX, u.liveInverseZ, u.liveDepthParameters, height, error);

        for (uint other = 0; other < 16; ++other)
        {
            if (matches && other < uint(u.values.x) && other != selected)
            {
                ParallaxLayer candidate = u.parallaxLayers[other];
                float2 candidateUv = float2(dot(candidate.x.xyz, pixel), dot(candidate.y.xyz, pixel));
                if (inside(candidateUv))
                {
                    int2 candidatePixel = clamp(int2(floor(candidateUv * candidate.size.xy)), 0, int2(candidate.size.xy) - 1);
                    float candidateDepth = loadImage(images[16 + other * 3 + 2], candidatePixel,
                        int2(candidate.size.xy), candidate.flags.z).r;
                    float farDepth = (candidate.flags.z & 8) != 0 ? 0 : 1;
                    if (candidateDepth != farDepth)
                    {
                        float candidateHeight, candidateError;
                        surfaceHeight(candidateDepth, float2(candidatePixel), candidate.inverseX, candidate.inverseZ,
                            candidate.depth, candidateHeight, candidateError);
                        if (!isfinite(candidateHeight) || abs(candidateHeight - height) <= candidateError + error)
                            matches = false;
                    }
                }
            }
        }
        if (matches && (u.imageFlags[0].y & 2) != 0)
        {
            int2 start = int2(floor(uv * u.dimensions[0].xy - .5));
            matches = liveHeight(u, images, samplers, start, selectedHeight, selectedError, true)
                && liveHeight(u, images, samplers, start + int2(1, 0), selectedHeight, selectedError, true)
                && liveHeight(u, images, samplers, start + int2(0, 1), selectedHeight, selectedError, true)
                && liveHeight(u, images, samplers, start + int2(1, 1), selectedHeight, selectedError, true);
        }
        if (matches)
        {
            Coverage live = liveCoverage(u, images, samplers, uv);
            foreground = live.foreground;
            transmission = live.transmission;
        }
    }
    return foreground + transmission * images[0].read(uint2(input.position.xy));
}

fragment float4 correctionPixel(Vertex input [[stage_in]], constant Constants& u [[buffer(0)]],
    SCENE_ARGUMENTS)
{
    SCENE_RESOURCES
    float4 color = images[0].read(uint2(input.position.xy));
    float3 corrected;
    corrected.r = sampleImage(images[1], samplers[1], float2(color.r, .125), u.imageFlags[0].y).r;
    corrected.g = sampleImage(images[1], samplers[1], float2(color.g, .375), u.imageFlags[0].y).g;
    corrected.b = sampleImage(images[1], samplers[1], float2(color.b, .625), u.imageFlags[0].y).b;
    float luma = dot(corrected, float3(.220, .707, .071));
    return float4(luma + u.values.x * (corrected - luma), color.a);
}

fragment float4 additivePixel(Vertex input [[stage_in]], constant Constants& u [[buffer(0)]],
    SCENE_ARGUMENTS)
{
    SCENE_RESOURCES
    float4 color = images[0].read(uint2(input.position.xy));
    float2 uv = float2(dot(u.rows[0].xyz, float3(input.position.xy, 1)),
        dot(u.rows[1].xyz, float3(input.position.xy, 1)));
    float3 addition = 0;
    if (inside(uv))
    {
        addition = sampleImage(images[1], samplers[1], uv, u.imageFlags[0].y).rgb;
    }
    else
    {
        uv = float2(dot(u.rows[2].xyz, float3(input.position.xy, 1)),
            dot(u.rows[3].xyz, float3(input.position.xy, 1)));
        if (inside(uv)) addition = sampleImage(images[4], samplers[4], uv, u.imageFlags[1].x).rgb;
    }
    return float4(color.rgb + addition, 1);
}
kernel void reduceDepth(constant Constants& u [[buffer(0)]],
    texture2d<float, access::read> source [[texture(0)]],
    texture2d<float, access::write> target [[texture(1)]],
    uint2 pixel [[thread_position_in_grid]], uint2 group [[threadgroup_position_in_grid]],
    uint index [[thread_index_in_threadgroup]])
{
    threadgroup float depthBlock[256];
    float depth = 0;
    if (all(pixel < uint2(u.dimensions[0].xy)))
    {
        depth = source.read(pixel).r;
        if (u.values.z != 0 && u.values.w != 0) depth = 1 - depth;
        if (!isfinite(depth) || depth < 0 || depth > 1) depth = 0;
    }
    depthBlock[index] = depth;
    threadgroup_barrier(mem_flags::mem_threadgroup);
    for (uint stride = 128; stride != 0; stride >>= 1)
    {
        if (index < stride) depthBlock[index] = max(depthBlock[index], depthBlock[index + stride]);
        threadgroup_barrier(mem_flags::mem_threadgroup);
    }
    if (index == 0) target.write(float4(depthBlock[0]), group);
}
)metal";

}
