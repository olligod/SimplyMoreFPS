#pragma once

namespace smf_scene_source {

    inline constexpr char source[] = R"(
struct BackgroundView
{
    column_major float4x4 projection;
    column_major float4x4 inverseProjection;
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

cbuffer Constants : register(b0)
{
    float4 rows[4];
    float4 dimensions[2];
    uint4 imageFlags[4];
    float4 values;
    column_major float4x4 inverseProjection;
    column_major float4x4 projection;
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

Texture2D<float4> previous : register(t0);
Texture2D<float4> colorImage : register(t1);
Texture2D<float4> probeImage : register(t2);
Texture2D<float4> referenceImage : register(t3);
Texture2D<float4> cacheColor : register(t4);
Texture2D<float4> cacheProbe : register(t5);
Texture2D<float4> extra0 : register(t6);
Texture2D<float4> extra1 : register(t7);
Texture2D<float4> extra2 : register(t8);
Texture2D<float4> extra3 : register(t9);
Texture2D<float4> extra4 : register(t10);
Texture2D<float4> bounds0 : register(t11);
Texture2D<float4> bounds1 : register(t12);
Texture2D<float4> bounds2 : register(t13);
Texture2D<float4> bounds3 : register(t14);
Texture2D<float4> parallaxImages[48] : register(t16);
SamplerState previousSampler : register(s0);
SamplerState colorSampler : register(s1);
SamplerState probeSampler : register(s2);
SamplerState referenceSampler : register(s3);
SamplerState cacheColorSampler : register(s4);
SamplerState cacheProbeSampler : register(s5);
SamplerState extraSampler0 : register(s6);
SamplerState extraSampler1 : register(s7);
SamplerState extraSampler2 : register(s8);
SamplerState extraSampler3 : register(s9);
SamplerState extraSampler4 : register(s10);
SamplerState linearSampler : register(s14);
SamplerState pointSampler : register(s15);
#ifdef REDUCE_DEPTH
RWTexture2D<float> reducedDepth : register(u0);
groupshared float depthBlock[256];

[numthreads(16, 16, 1)]
void reduceDepth(uint3 pixel : SV_DispatchThreadID, uint3 group : SV_GroupID, uint index : SV_GroupIndex)
{
    float depth = 0;
    if (all(pixel.xy < uint2(dimensions[0].xy)))
    {
        depth = referenceImage.Load(int3(pixel.xy, 0)).r;
        if (values.z != 0 && values.w != 0) depth = 1 - depth;
        if (!isfinite(depth) || depth < 0 || depth > 1) depth = 0;
    }
    depthBlock[index] = depth;
    GroupMemoryBarrierWithGroupSync();
    for (uint stride = 128; stride != 0; stride >>= 1)
    {
        if (index < stride) depthBlock[index] = max(depthBlock[index], depthBlock[index + stride]);
        GroupMemoryBarrierWithGroupSync();
    }
    if (index == 0) reducedDepth[group.xy] = depthBlock[0];
}
#endif

struct Vertex
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

Vertex vertex(uint index : SV_VertexID)
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

float4 sampleImage(Texture2D<float4> image, SamplerState imageSampler, float2 uv, uint flags)
{
    return image.SampleLevel(imageSampler, orient(uv, flags), 0);
}

float4 loadImage(Texture2D<float4> image, int2 pixel, int2 size, uint flags)
{
    pixel = clamp(pixel, int2(0, 0), size - 1);
    if ((flags & 1) != 0) pixel.y = size.y - 1 - pixel.y;
    return image.Load(int3(pixel, 0));
}

float3 unproject(float2 uv, float depth, float4x4 inverseMatrix)
{
    if (values.w != 0) depth = depth * 2 - 1;
    float4 viewPoint = mul(inverseMatrix, float4(uv * float2(2, -2) + float2(-1, 1), depth, 1));
    return viewPoint.xyz / viewPoint.w;
}

float2 project(float3 viewPoint, float4x4 projectionMatrix)
{
    float4 clip = mul(projectionMatrix, float4(viewPoint, 1));
    return clip.xy / clip.w * float2(.5, -.5) + .5;
}

float3 atmosphere(float3 ray, float2 delta, bool cached)
{
    ray = normalize(ray);
    float3 result = 0;
    uint count = cached ? glowCounts.y : glowCounts.x;
    [loop]
    for (uint i = 0; i < count; ++i)
    {
        BackgroundGlow glow = liveGlows[i];
        if (cached) glow = cachedGlows[i];
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
        float ring = max(1 - abs(radius - glow.worldOrigin.w) / glow.planetOrigin.w, 0);
        float light = dot(nearest / radius, -glow.sun.xyz) * .5 + .5;
        result += (.28 * light * light + .02) * ring * ring * ring * glow.sun.w * float3(.706, .839, 1);
    }
    return all(isfinite(result)) ? result : 0;
}

float4 worldTexel(Texture2D<float4> image, int2 pixel, int2 size, uint flags,
    float4x4 inverseMatrix, float2 delta, bool cached)
{
    pixel = clamp(pixel, 0, size - 1);
    float4 color = loadImage(image, pixel, size, flags);
    float3 ray = unproject((float2(pixel) + .5) / float2(size), .5, inverseMatrix);
    color.rgb -= atmosphere(ray, delta, cached);
    return color;
}

float4 worldSample(Texture2D<float4> image, SamplerState imageSampler, float2 uv, uint flags,
    float4x4 inverseMatrix, float2 delta, bool cached)
{
    uint width = 0, height = 0;
    image.GetDimensions(width, height);
    int2 size = int2(width, height);
    float2 pixel = uv * float2(size) - .5;
    float4 result = worldTexel(image, int2(floor(pixel + .5)), size, flags, inverseMatrix, delta, cached);
    if ((flags & 2) != 0)
    {
        int2 start = int2(floor(pixel));
        float2 weight = frac(pixel);
        float4 a = worldTexel(image, start, size, flags, inverseMatrix, delta, cached);
        float4 b = worldTexel(image, start + int2(1, 0), size, flags, inverseMatrix, delta, cached);
        float4 c = worldTexel(image, start + int2(0, 1), size, flags, inverseMatrix, delta, cached);
        float4 d = worldTexel(image, start + int2(1, 1), size, flags, inverseMatrix, delta, cached);
        result = lerp(lerp(a, b, weight.x), lerp(c, d, weight.x), weight.y);
    }
    if ((cached ? glowCounts.y : glowCounts.x) == 0) result = sampleImage(image, imageSampler, uv, flags);
    return result;
}

bool surfaceDepth(Texture2D<float4> depthImage, BackgroundView view, float2 uv, uint flags, out float depth)
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
    float farDepth = values.w != 0 ? 1 : 0;
    if (!all(isfinite(samples)) || any(samples == farDepth)) return false;
    float2 weight = frac(pixel);
    depth = lerp(lerp(samples.x, samples.y, weight.x), lerp(samples.z, samples.w, weight.x), weight.y);
    return true;
}

bool renderView(Texture2D<float4> worldColor, SamplerState worldSampler, uint colorFlags,
    Texture2D<float4> worldDepth, uint depthFlags, Texture2D<float4> nearestDepth,
    BackgroundView view, float3 ray, out float4 output)
{
    output = 0;
    float bound = nearestDepth.Load(int3(0, 0, 0)).r;
    if (bound == 0) return false;
    if (values.w != 0) bound = 1 - bound;
    float nearDistance = -unproject(.5, bound, view.inverseProjection).z;
    if (!isfinite(nearDistance) || nearDistance <= 0) return false;
    float2 uv = project(ray * nearDistance + float3(view.delta.xy, 0), view.projection);
    float3 viewPoint = 0;
    bool converged = false;
    [loop]
    for (int iteration = 0; iteration < 16; ++iteration)
    {
        float depth;
        if (!surfaceDepth(worldDepth, view, uv, depthFlags, depth)) return false;
        float distance = -unproject(uv, depth, view.inverseProjection).z;
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
    bool normalValid = surfaceDepth(worldDepth, view, uv + float2(offset.x, 0), depthFlags, xDepth)
        && surfaceDepth(worldDepth, view, uv + float2(0, offset.y), depthFlags, yDepth);
    if (normalValid)
    {
        float3 xPoint = unproject(uv + float2(offset.x, 0), xDepth, view.inverseProjection);
        float3 yPoint = unproject(uv + float2(0, offset.y), yDepth, view.inverseProjection);
        normal = cross(xPoint - cachedPoint, yPoint - cachedPoint);
        if (dot(normal, -cachedPoint) < 0) normal = -normal;
        normalValid = dot(normal, normal) > 1e-12;
        if (normalValid && dot(normal, -viewPoint) <= 0) return false;
    }
    float3 livePoint = viewPoint + float3(cameraDelta.xy, 0);
    float2 liveUv = project(livePoint, projection);
    if (inside(liveUv) && normalValid && dot(normalize(normal), normalize(-livePoint)) > .05)
        output = worldSample(colorImage, colorSampler, liveUv, imageFlags[0].y, inverseProjection, 0, false);
    else
        output = worldSample(worldColor, worldSampler, uv, colorFlags,
            view.inverseProjection, cameraDelta.xy - view.delta.xy, true);
    output.rgb += atmosphere(ray, cameraDelta.xy, false);
    return true;
}

float4 backgroundPixel(Vertex input) : SV_TARGET
{
    if (values.z != 0)
        return sampleImage(colorImage, colorSampler, input.uv, imageFlags[0].y);
    float3 ray = unproject(input.uv, .5, inverseProjection);
    ray /= -ray.z;
    float4 output = 0;
    if (values.x > 0 && renderView(referenceImage, referenceSampler, imageFlags[0].w,
        cacheColor, imageFlags[1].x, bounds0, backgroundViews[0], ray, output)) return output;
    if (values.x > 1 && renderView(cacheProbe, cacheProbeSampler, imageFlags[1].y,
        extra0, imageFlags[1].z, bounds1, backgroundViews[1], ray, output)) return output;
    if (values.x > 2 && renderView(extra1, extraSampler1, imageFlags[1].w,
        extra2, imageFlags[2].x, bounds2, backgroundViews[2], ray, output)) return output;
    if (values.x > 3 && renderView(extra3, extraSampler3, imageFlags[2].y,
        extra4, imageFlags[2].z, bounds3, backgroundViews[3], ray, output)) return output;
    output = sampleImage(probeImage, probeSampler, (input.uv - .5) / values.y + .5, imageFlags[0].z);
    output.rgb += atmosphere(ray, cameraDelta.xy, false);
    return output;
}

struct Coverage
{
    float4 foreground;
    float4 transmission;
};

Coverage liveTexel(int2 pixel)
{
    int2 size = int2(dimensions[0].xy);
    float4 color = loadImage(colorImage, pixel, size, imageFlags[0].y);
    float4 probe = loadImage(probeImage, pixel, size, imageFlags[0].z);
    float4 reference = loadImage(referenceImage, pixel, size, imageFlags[0].w);
    float3 contrast = f16tof32(f32tof16(reference.rgb + 1)) - reference.rgb;
    Coverage output = (Coverage)0;
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

Coverage liveCoverage(float2 uv)
{
    float2 pixel = uv * dimensions[0].xy - .5;
    Coverage output = (Coverage)0;
    if ((imageFlags[0].y & 2) == 0)
    {
        output = liveTexel(int2(floor(pixel + .5)));
    }
    else
    {
        int2 start = int2(floor(pixel));
        float2 weight = frac(pixel);
        Coverage a = liveTexel(start);
        Coverage b = liveTexel(start + int2(1, 0));
        Coverage c = liveTexel(start + int2(0, 1));
        Coverage d = liveTexel(start + int2(1, 1));
        output.foreground = lerp(lerp(a.foreground, b.foreground, weight.x),
            lerp(c.foreground, d.foreground, weight.x), weight.y);
        output.transmission = lerp(lerp(a.transmission, b.transmission, weight.x),
            lerp(c.transmission, d.transmission, weight.x), weight.y);
    }
    return output;
}

float4 layerPixel(Vertex input) : SV_TARGET
{
    if (values.y != 0)
        return loadImage(colorImage, int2(input.position.xy), int2(dimensions[0].xy), imageFlags[0].y);
    float3 pixel = float3(input.position.xy, 1);
    float2 uv = float2(dot(rows[0].xyz, pixel), dot(rows[1].xyz, pixel));
    Coverage coverage = (Coverage)0;
    if (values.x != 0 && inside(uv))
    {
        coverage = liveCoverage(uv);
    }
    else
    {
        uv = float2(dot(rows[2].xyz, pixel), dot(rows[3].xyz, pixel));
        if (!inside(uv)) uv = .5 / dimensions[1].xy;
        float4 black = sampleImage(cacheColor, cacheColorSampler, uv, imageFlags[1].x);
        float4 white = sampleImage(cacheProbe, cacheProbeSampler, uv, imageFlags[1].y);
        coverage.foreground = black;
        coverage.transmission = white - black;
    }
    float4 background = previous.Load(int3(int2(input.position.xy), 0));
    return coverage.foreground + coverage.transmission * background;
}

void surfaceHeight(float depth, float2 pixel, float4 inverseX, float4 inverseZ,
    float4 parameters, out float height, out float error)
{
    float3 position = float3(pixel + .5, 1);
    float3 xTerms = position * inverseX.xyz;
    float3 zTerms = position * inverseZ.xyz;
    float x = dot(xTerms, 1);
    float z = dot(zTerms, 1);
    float4 terms = float4(x, z, depth, 1) * parameters;
    height = dot(terms, 1);
    error = 9.5367431640625e-7 * (dot(abs(terms), 1)
        + abs(parameters.x) * dot(abs(xTerms), 1) + abs(parameters.y) * dot(abs(zTerms), 1));
}

bool liveHeight(int2 pixel, float height, float error, bool allowEmpty)
{
    pixel = clamp(pixel, 0, int2(dimensions[0].xy) - 1);
    float depth = loadImage(cacheColor, pixel, int2(dimensions[0].xy), imageFlags[1].x).r;
    float farDepth = (imageFlags[1].x & 8) != 0 ? 0 : 1;
    if (depth == farDepth) return allowEmpty;
    float actual, uncertainty;
    surfaceHeight(depth, pixel, liveInverseX, liveInverseZ, liveDepthParameters, actual, uncertainty);
    return isfinite(actual) && isfinite(uncertainty) && abs(actual - height) <= error + uncertainty;
}

)" R"(
float4 parallaxPixel(Vertex input) : SV_TARGET
{
    float nearest = 0;
    bool found = false;
    float4 foreground = 0;
    float4 transmission = 1;
    uint selected = 0;
    uint ties = 0;
    float selectedHeight = 0;
    float selectedError = 0;
    [unroll]
    for (uint i = 0; i < 16; ++i)
    {
        if (i < uint(values.x))
        {
            ParallaxLayer layer = parallaxLayers[i];
            float3 pixel = float3(input.position.xy, 1);
            float2 uv = float2(dot(layer.x.xyz, pixel), dot(layer.y.xyz, pixel));
            if (inside(uv))
            {
                int2 depthPixel = clamp(int2(floor(uv * layer.size.xy)), 0, int2(layer.size.xy) - 1);
                float depth = loadImage(parallaxImages[i * 3 + 2], depthPixel, int2(layer.size.xy), layer.flags.z).r;
                bool reversed = (layer.flags.z & 8) != 0;
                float farDepth = reversed ? 0 : 1;
                bool nearer = !found || (reversed ? depth > nearest : depth < nearest);
                bool tied = found && depth == nearest;
                if (isfinite(depth) && depth >= 0 && depth <= 1 && depth != farDepth && (nearer || tied))
                {
                    float4 black = (layer.flags.x & 2) != 0
                        ? sampleImage(parallaxImages[i * 3], linearSampler, uv, layer.flags.x)
                        : sampleImage(parallaxImages[i * 3], pointSampler, uv, layer.flags.x);
                    float4 white = (layer.flags.y & 2) != 0
                        ? sampleImage(parallaxImages[i * 3 + 1], linearSampler, uv, layer.flags.y)
                        : sampleImage(parallaxImages[i * 3 + 1], pointSampler, uv, layer.flags.y);
                    float4 through = white - black;
                    if (nearer)
                    {
                        nearest = depth;
                        foreground = black;
                        transmission = through;
                        found = true;
                        selected = i;
                        ties = 1;
                        surfaceHeight(depth, depthPixel, layer.inverseX, layer.inverseZ, layer.depth, selectedHeight, selectedError);
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
    if (found && ties == 1 && values.y != 0)
    {
        ParallaxLayer layer = parallaxLayers[selected];
        float3 pixel = float3(input.position.xy, 1);
        float2 uv = float2(dot(layer.liveX.xyz, pixel), dot(layer.liveY.xyz, pixel));
        int2 center = clamp(int2(floor(uv * dimensions[0].xy)), 0, int2(dimensions[0].xy) - 1);
        bool matches = isfinite(selectedHeight) && isfinite(selectedError) && inside(uv)
            && liveHeight(center, selectedHeight, selectedError, false);
        float liveDepth = loadImage(cacheColor, center, int2(dimensions[0].xy), imageFlags[1].x).r;
        float height, error;
        surfaceHeight(liveDepth, center, liveInverseX, liveInverseZ, liveDepthParameters, height, error);
        [unroll]
        for (uint other = 0; other < 16; ++other)
        {
            if (matches && other < uint(values.x) && other != selected)
            {
                ParallaxLayer candidate = parallaxLayers[other];
                float2 candidateUv = float2(dot(candidate.x.xyz, pixel), dot(candidate.y.xyz, pixel));
                if (inside(candidateUv))
                {
                    int2 candidatePixel = clamp(int2(floor(candidateUv * candidate.size.xy)), 0, int2(candidate.size.xy) - 1);
                    float candidateDepth = loadImage(parallaxImages[other * 3 + 2], candidatePixel,
                        int2(candidate.size.xy), candidate.flags.z).r;
                    float farDepth = (candidate.flags.z & 8) != 0 ? 0 : 1;
                    if (candidateDepth != farDepth)
                    {
                        float candidateHeight, candidateError;
                        surfaceHeight(candidateDepth, candidatePixel, candidate.inverseX, candidate.inverseZ,
                            candidate.depth, candidateHeight, candidateError);
                        if (!isfinite(candidateHeight) || abs(candidateHeight - height) <= candidateError + error)
                            matches = false;
                    }
                }
            }
        }
        if (matches && (imageFlags[0].y & 2) != 0)
        {
            int2 start = int2(floor(uv * dimensions[0].xy - .5));
            matches = liveHeight(start, selectedHeight, selectedError, true)
                && liveHeight(start + int2(1, 0), selectedHeight, selectedError, true)
                && liveHeight(start + int2(0, 1), selectedHeight, selectedError, true)
                && liveHeight(start + int2(1, 1), selectedHeight, selectedError, true);
        }
        if (matches)
        {
            Coverage live = liveCoverage(uv);
            foreground = live.foreground;
            transmission = live.transmission;
        }
    }
    return foreground + transmission * previous.Load(int3(int2(input.position.xy), 0));
}

float4 correctionPixel(Vertex input) : SV_TARGET
{
    float4 color = previous.Load(int3(int2(input.position.xy), 0));
    float3 corrected;
    corrected.r = sampleImage(colorImage, colorSampler, float2(color.r, .125), imageFlags[0].y).r;
    corrected.g = sampleImage(colorImage, colorSampler, float2(color.g, .375), imageFlags[0].y).g;
    corrected.b = sampleImage(colorImage, colorSampler, float2(color.b, .625), imageFlags[0].y).b;
    float luma = dot(corrected, float3(.220, .707, .071));
    return float4(luma + values.x * (corrected - luma), color.a);
}

float4 additivePixel(Vertex input) : SV_TARGET
{
    float4 color = previous.Load(int3(int2(input.position.xy), 0));
    float2 uv = float2(dot(rows[0].xyz, float3(input.position.xy, 1)),
        dot(rows[1].xyz, float3(input.position.xy, 1)));
    float3 addition = 0;
    if (inside(uv))
    {
        addition = sampleImage(colorImage, colorSampler, uv, imageFlags[0].y).rgb;
    }
    else
    {
        uv = float2(dot(rows[2].xyz, float3(input.position.xy, 1)),
            dot(rows[3].xyz, float3(input.position.xy, 1)));
        if (inside(uv)) addition = sampleImage(cacheColor, cacheColorSampler, uv, imageFlags[1].x).rgb;
    }
    return float4(color.rgb + addition, 1);
}
)";


}
