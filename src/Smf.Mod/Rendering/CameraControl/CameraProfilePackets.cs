using System;
using System.Runtime.InteropServices;
using SimplyMoreFPS.API;

namespace SimplyMoreFPS.Rendering.CameraControl;

public static partial class NativeCameraBridge
{
    [StructLayout(LayoutKind.Sequential, Pack = 8)]
    public struct KernelAxisBounds
    {
        public uint Enabled, Reserved;
        public double MinimumA, MinimumB, MinimumLimitA, MinimumLimitB;
        public double MaximumA, MaximumB, MaximumLimitA, MaximumLimitB, CollapsePosition;
    }

    [StructLayout(LayoutKind.Sequential, Pack = 8)]
    public struct KernelMovementBounds
    {
        public KernelAxisBounds X, Z;
        public double MaximumSize;
    }

    [StructLayout(LayoutKind.Sequential, Pack = 8)]
    public unsafe struct KernelCurve
    {
        public uint Mode, Domain, PointCount, Flags;
        public double InputMin, InputMax, A, B, C, Exponent;
        public fixed double X[16];
        public fixed double Y[16];
    }

    [StructLayout(LayoutKind.Sequential, Pack = 8)]
    public struct KernelProfilePack
    {
        public uint PresentMask, Reserved;
        public KernelCurve Projection, KeyboardRate, EdgeRate, MoveSpeed, ZoomSpeed;
    }

    [StructLayout(LayoutKind.Sequential, Pack = 8)]
    public struct KernelTrajectory
    {
        public ulong Id;
        public uint Kind, Flags;
        public double StartSeconds, DurationSeconds, SourceX, SourceZ, SourceRootSize;
        public double TargetX, TargetZ, TargetRootSize;
    }

    // Profiles and bounds are immutable, so the last encoding is reused
    // while the same instance is published.
    private static CameraMotionBounds? lastBounds;
    private static KernelMovementBounds lastBoundsPacket;
    private static CameraProfile? lastProfile;
    private static KernelProfilePack lastProfilePacket;

    internal static KernelMovementBounds EncodeBounds(CameraMotionBounds? bounds)
    {
        if (ReferenceEquals(bounds, lastBounds)) return lastBoundsPacket;

        var packet = new KernelMovementBounds
        {
            X = EncodeAxis(bounds?.X),
            Z = EncodeAxis(bounds?.Z),
            MaximumSize = bounds?.MaximumSize ?? 0
        };

        lastBounds = bounds;
        lastBoundsPacket = packet;
        return packet;
    }

    private static KernelAxisBounds EncodeAxis(CameraAxisBounds? axis)
    {
        if (axis == null) return default;
        return new KernelAxisBounds
        {
            Enabled = 1,
            MinimumA = axis.MinimumA,
            MinimumB = axis.MinimumB,
            MinimumLimitA = axis.MinimumLimitA,
            MinimumLimitB = axis.MinimumLimitB,
            MaximumA = axis.MaximumA,
            MaximumB = axis.MaximumB,
            MaximumLimitA = axis.MaximumLimitA,
            MaximumLimitB = axis.MaximumLimitB,
            CollapsePosition = axis.CollapsePosition
        };
    }

    private static KernelProfilePack EncodeProfile(CameraProfile? profile)
    {
        if (ReferenceEquals(profile, lastProfile)) return lastProfilePacket;

        KernelProfilePack pack = default;
        if (profile != null)
        {
            pack.Projection = EncodeCurve(profile.Projection, 1, ref pack.PresentMask);
            pack.KeyboardRate = EncodeCurve(profile.KeyboardRate, 2, ref pack.PresentMask);
            pack.EdgeRate = EncodeCurve(profile.EdgeRate, 4, ref pack.PresentMask);
            pack.MoveSpeed = EncodeCurve(profile.MoveSpeed, 8, ref pack.PresentMask);
            pack.ZoomSpeed = EncodeCurve(profile.ZoomSpeed, 16, ref pack.PresentMask);
        }

        lastProfile = profile;
        lastProfilePacket = pack;
        return pack;
    }

    private static unsafe KernelCurve EncodeCurve(CameraCurve? curve, uint bit, ref uint mask)
    {
        if (curve == null) return default;
        mask |= bit;

        var packet = new KernelCurve
        {
            Mode = (uint)curve.Mode,
            Domain = (uint)curve.Domain,
            PointCount = (uint)curve.Points.Count,
            Flags = curve.ClampInput ? 1u : 0u,
            InputMin = curve.InputMin,
            InputMax = curve.InputMax,
            A = curve.A,
            B = curve.B,
            C = curve.C,
            Exponent = curve.Exponent
        };

        for (int i = 0; i < curve.Points.Count; i++)
        {
            packet.X[i] = curve.Points[i].X;
            packet.Y[i] = curve.Points[i].Y;
        }

        return packet;
    }

    // Both packets are fully initialized blittable PODs (reserved fields
    // included), so a word-wise compare is exact.
    private static unsafe bool SameBounds(KernelMovementBounds a, KernelMovementBounds b)
    {
        ulong* left = (ulong*)&a;
        ulong* right = (ulong*)&b;

        for (int i = 0; i < sizeof(KernelMovementBounds) / sizeof(ulong); i++)
        {
            if (left[i] != right[i]) return false;
        }

        return true;
    }

    private static unsafe bool SameProfile(KernelProfilePack a, KernelProfilePack b)
    {
        ulong* left = (ulong*)&a;
        ulong* right = (ulong*)&b;

        for (int i = 0; i < sizeof(KernelProfilePack) / sizeof(ulong); i++)
        {
            if (left[i] != right[i]) return false;
        }

        return true;
    }

    internal static double NowSeconds()
    {
        if (current == null || current.Stopped) throw new InvalidOperationException("Camera clock is unavailable.");

        double now = current.Now();
        if (double.IsNaN(now) || double.IsInfinity(now) || now < 0)
            throw new InvalidOperationException("Native camera clock returned an invalid time.");
        return now;
    }
}
