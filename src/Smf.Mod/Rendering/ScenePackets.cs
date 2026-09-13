using System;
using System.Runtime.InteropServices;

namespace SimplyMoreFPS.Rendering;

internal static class ScenePackets
{
    internal const uint Version = 1;
    internal const int MaximumImages = 32;
    internal const int MaximumLayers = 16;
    internal const int MaximumEffects = 8;
    internal const int MaximumBackgroundGlows = 4;
    internal const uint NoImage = uint.MaxValue;

    [Flags]
    internal enum ImageFlags : uint
    {
        None = 0,
        FlipY = 1,
        LinearFilter = 2,
        Depth = 4,
        ReversedDepth = 8
    }

    internal enum LayerKind : uint
    {
        LiveMap = 1,
        CachedMap = 2,
        ParallaxMap = 3,
        LiveParallaxMap = 4
    }

    [Flags]
    internal enum LayerFlags : uint
    {
        None = 0,
        DepthOcclusion = 1
    }

    internal enum EffectKind : uint
    {
        ColorCorrection = 1,
        AdditiveImage = 2,
        ImageFilter = 3
    }

    [StructLayout(LayoutKind.Sequential, Pack = 8)]
    internal struct Image
    {
        internal ulong Texture, Serial;
        internal uint Width, Height;
        internal ImageFlags Flags;
        internal uint Reserved;
    }

    [StructLayout(LayoutKind.Sequential, Pack = 8)]
    internal unsafe struct Layer
    {
        internal LayerKind Kind;
        internal uint Color, Probe, Reference;
        internal fixed double Affine[6];
        internal double SourceX, SourceZ, ParallaxX, ParallaxZ;
        internal LayerFlags Flags;
        internal uint Depth;
        internal double DepthX, DepthZ, DepthScale, DepthOffset;
    }

    [StructLayout(LayoutKind.Sequential, Pack = 8)]
    internal unsafe struct BackgroundView
    {
        internal uint Color, Depth, Flags, Reserved;
        internal fixed float Projection[16];
        internal double X, Z;
    }

    [StructLayout(LayoutKind.Sequential, Pack = 8)]
    internal unsafe struct BackgroundGlow
    {
        internal fixed float WorldOrigin[3];
        internal float PlanetRadius;
        internal fixed float PlanetOrigin[3];
        internal float GlowRadius;
        internal fixed float Sun[3];
        internal float Intensity;
        internal fixed float MeshCenter[3];
        internal float MeshRadius;
    }

    [StructLayout(LayoutKind.Sequential, Pack = 8)]
    internal unsafe struct Background
    {
        internal uint LiveColor, Sky, ViewCount, Flags;
        internal fixed float Projection[16];
        internal double SourceX, SourceZ;
        internal double MinX, MaxX, MinZ, MaxZ;
        internal double CameraXPerCell, CameraYPerCell;
        internal double SkyScale, Reserved;
        internal BackgroundView View0, View1, View2, View3;
        internal uint LiveGlowCount, CachedGlowCount, Reserved0, Reserved1;
        internal BackgroundGlow LiveGlow0, LiveGlow1, LiveGlow2, LiveGlow3;
        internal BackgroundGlow CachedGlow0, CachedGlow1, CachedGlow2, CachedGlow3;
    }

    [StructLayout(LayoutKind.Sequential, Pack = 8)]
    internal unsafe struct Effect
    {
        internal EffectKind Kind;
        internal uint FirstImage, SecondImage, Flags;
        internal ulong Program;
        internal uint Pass, Reserved;
        internal fixed float Parameters[16];
    }

    [StructLayout(LayoutKind.Sequential, Pack = 8)]
    internal struct Description
    {
        internal uint Size, Version, ImageCount, LayerCount, EffectCount, Flags;
        internal ulong SourceFrame;
        internal ulong Images, Layers, Effects, Reserved;
        internal Background World;
    }

    internal static void ValidateLayout()
    {
        if (Marshal.SizeOf<Image>() != 32 || Marshal.SizeOf<Layer>() != 136 ||
            Marshal.SizeOf<BackgroundView>() != 96 || Marshal.SizeOf<BackgroundGlow>() != 64 ||
            Marshal.SizeOf<Background>() != 1072 || Marshal.SizeOf<Effect>() != 96 ||
            Marshal.SizeOf<Description>() != 1136 ||
            Marshal.OffsetOf<Layer>(nameof(Layer.Affine)).ToInt32() != 16 ||
            Marshal.OffsetOf<Layer>(nameof(Layer.DepthX)).ToInt32() != 104 ||
            Marshal.OffsetOf<BackgroundView>(nameof(BackgroundView.X)).ToInt32() != 80 ||
            Marshal.OffsetOf<Background>(nameof(Background.SourceX)).ToInt32() != 80 ||
            Marshal.OffsetOf<Background>(nameof(Background.View0)).ToInt32() != 160 ||
            Marshal.OffsetOf<Background>(nameof(Background.LiveGlow0)).ToInt32() != 560 ||
            Marshal.OffsetOf<Effect>(nameof(Effect.Parameters)).ToInt32() != 32 ||
            Marshal.OffsetOf<Description>(nameof(Description.World)).ToInt32() != 64)
        {
            throw new InvalidOperationException("Native scene packet layout mismatch.");
        }
    }
}
