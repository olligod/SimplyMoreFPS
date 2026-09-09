using System;
using System.Runtime.InteropServices;
using SimplyMoreFPS;
using SimplyMoreFPS.API;

namespace Smf.Camera;

// Managed side of camera_kernel.h. Field order, packing and the Bytes constants are the ABI.

public enum KernelResult
{
    Ok = 0,
    Stale = 1,
    InvalidArgument = -1,
    WrongThread = -2,
    Busy = -3,
    BadSession = -4,
    WrongEpoch = -5,
    Faulted = -6,
    InternalError = -7
}

[Flags]
public enum SettingsFlags : uint
{
    SmoothZoom = 1,
    ZoomToMouse = 2,
    EdgeScroll = 4,
    DisableZoomToMouseWhileShiftHeld = 8
}

[Flags]
public enum InputFlags : uint
{
    ZoomInPulse = 1,
    ZoomOutPulse = 2,
    FastPan = 4,
    MotionBlocked = 8,
    MiddleReleasedPulse = 16,
    AllowEdgeScroll = 32,
    Fullscreen = 64,
    PointerOverUi = 128,
    ClockOnly = 256
}

[StructLayout(LayoutKind.Sequential, Pack = 8)]
public struct KernelInit
{
    public const uint Bytes = 56;
    public uint Version;
    public uint Size;
    public ulong Epoch;
    public int MapId;
    public uint Reserved;
    public double X;
    public double Z;
    public double RootSize;
    public double MonotonicSeconds;
}

[StructLayout(LayoutKind.Sequential, Pack = 8)]
public unsafe struct KernelCurve
{
    public const uint Bytes = 320;
    public uint Mode;
    public uint Domain;
    public uint PointCount;
    public uint Flags;
    public double InputMin;
    public double InputMax;
    public double A;
    public double B;
    public double C;
    public double Exponent;
    public fixed double X[16];
    public fixed double Y[16];
}

[StructLayout(LayoutKind.Sequential, Pack = 8)]
public struct KernelProfilePack
{
    public const uint Bytes = 1608;
    public uint PresentMask;
    public uint Reserved;
    public KernelCurve Projection;
    public KernelCurve KeyboardRate;
    public KernelCurve EdgeRate;
    public KernelCurve MoveSpeed;
    public KernelCurve ZoomSpeed;
}

[StructLayout(LayoutKind.Sequential, Pack = 8)]
public struct KernelTrajectory
{
    public const uint Bytes = 80;
    public ulong Id;
    public uint Kind;
    public uint Flags;
    public double StartSeconds;
    public double DurationSeconds;
    public double SourceX;
    public double SourceZ;
    public double SourceRootSize;
    public double TargetX;
    public double TargetZ;
    public double TargetRootSize;
}

[StructLayout(LayoutKind.Sequential, Pack = 8)]
public struct KernelAxisBounds
{
    public const uint Bytes = 80;
    public uint Enabled;
    public uint Reserved;
    public double MinimumA;
    public double MinimumB;
    public double MinimumLimitA;
    public double MinimumLimitB;
    public double MaximumA;
    public double MaximumB;
    public double MaximumLimitA;
    public double MaximumLimitB;
    public double CollapsePosition;
}

[StructLayout(LayoutKind.Sequential, Pack = 8)]
public struct KernelMovementBounds
{
    public const uint Bytes = 168;
    public KernelAxisBounds X;
    public KernelAxisBounds Z;
    public double MaximumSize;
}

[StructLayout(LayoutKind.Sequential, Pack = 8)]
public struct KernelSettings
{
    public const uint Bytes = 1920;
    public uint Version;
    public uint Size;
    public ulong Revision;
    public uint Flags;
    public uint Reserved;
    public double MapWidth;
    public double MapHeight;
    public double PixelWidth;
    public double PixelHeight;
    public double UiScale;
    public double MinSize;
    public double MaxSize;
    public double DollyRateKeys;
    public double DollyRateScreenEdge;
    public double SpeedDecay;
    public double MoveSpeed;
    public double ZoomSpeed;
    public double ScrollWheelRate;
    public double ZoomPreserveFactor;
    public double DragSensitivity;
    public KernelProfilePack Profile;
    public KernelMovementBounds Bounds;
}

[StructLayout(LayoutKind.Sequential, Pack = 8)]
public struct KernelInput
{
    public const uint Bytes = 192;
    public uint Version;
    public uint Size;
    public ulong Epoch;
    public ulong Sequence;
    public ulong SettingsRevision;
    public uint Flags;
    public uint Reserved;
    public double MonotonicSeconds;
    public double PanX;
    public double PanZ;
    public double DragX;
    public double DragY;
    public double WheelDelta;
    public double PointerX;
    public double PointerY;
    public double InspectPaneHeight;
    public KernelTrajectory Trajectory;
}

[StructLayout(LayoutKind.Sequential, Pack = 8)]
public struct KernelPose
{
    public const uint Bytes = 136;
    public uint Version;
    public uint Size;
    public ulong Session;
    public ulong Epoch;
    public ulong PoseSequence;
    public ulong InputSequence;
    public ulong SettingsRevision;
    public int MapId;
    public int Result;
    public double X;
    public double Z;
    public double RootSize;
    public double DesiredSize;
    public double VelocityX;
    public double VelocityZ;
    public double ProjectionHalfHeight;
    public ulong ActivePanId;
    public ulong FinishedPanId;
    public uint PanFlags;
    public uint Reserved;
}

// Packet validation and conversion to the CameraMotion types.
internal static class Contract
{
    internal const uint Version = 2;
    private const uint KnownSettingsFlags = 15;
    private const uint KnownInputFlags = 511;
    private const uint KnownProfileCurves = 31;

    internal static bool Finite(double v) => !double.IsNaN(v) && !double.IsInfinity(v);

    private static bool Nonnegative(double v) => Finite(v) && v >= 0;

    private static bool Has(uint flags, SettingsFlags flag) => (flags & (uint)flag) != 0;

    private static bool Has(uint flags, InputFlags flag) => (flags & (uint)flag) != 0;

    internal static bool Valid(KernelInit p)
    {
        if (p.Version != Version || p.Size != KernelInit.Bytes) return false;
        if (p.Epoch == 0 || p.MapId < 0 || p.Reserved != 0) return false;
        return Finite(p.X) && Finite(p.Z) && Finite(p.RootSize) && p.RootSize > 0 && Nonnegative(p.MonotonicSeconds);
    }

    internal static bool Valid(KernelSettings p)
    {
        if (p.Version != Version || p.Size != KernelSettings.Bytes) return false;
        if (p.Revision == 0 || (p.Flags & ~KnownSettingsFlags) != 0 || p.Reserved != 0) return false;
        if (!Finite(p.MapWidth) || p.MapWidth < 4 || !Finite(p.MapHeight) || p.MapHeight < 4) return false;
        if (!Finite(p.PixelWidth) || p.PixelWidth <= 0 || !Finite(p.PixelHeight) || p.PixelHeight <= 0) return false;
        if (!Finite(p.UiScale) || p.UiScale <= 0) return false;
        if (!Finite(p.MinSize) || p.MinSize <= 0 || !Finite(p.MaxSize) || p.MaxSize <= p.MinSize) return false;
        if (!Nonnegative(p.DollyRateKeys) || !Nonnegative(p.DollyRateScreenEdge)) return false;
        if (!Nonnegative(p.SpeedDecay) || p.SpeedDecay > 1) return false;
        if (!Nonnegative(p.MoveSpeed) || !Nonnegative(p.ZoomSpeed) || !Nonnegative(p.ScrollWheelRate)) return false;
        if (!Nonnegative(p.ZoomPreserveFactor) || p.ZoomPreserveFactor > 1) return false;
        return Nonnegative(p.DragSensitivity);
    }

    internal static bool Valid(KernelInput p)
    {
        if (p.Version != Version || p.Size != KernelInput.Bytes) return false;
        if (p.Epoch == 0 || p.Sequence == 0 || p.SettingsRevision == 0) return false;
        if ((p.Flags & ~KnownInputFlags) != 0 || p.Reserved != 0) return false;
        if (!Nonnegative(p.MonotonicSeconds)) return false;
        if (!Finite(p.PanX) || p.PanX < -1 || p.PanX > 1 || !Finite(p.PanZ) || p.PanZ < -1 || p.PanZ > 1) return false;
        if (!Finite(p.DragX) || !Finite(p.DragY) || !Finite(p.WheelDelta)) return false;
        if (!Finite(p.PointerX) || !Finite(p.PointerY) || !Nonnegative(p.InspectPaneHeight)) return false;
        if (!Valid(p.Trajectory, p.MonotonicSeconds)) return false;
        if (!Has(p.Flags, InputFlags.ClockOnly)) return true;

        // A clock-only step may carry MotionBlocked but no other flag and no payload.
        if ((p.Flags & ~(uint)(InputFlags.ClockOnly | InputFlags.MotionBlocked)) != 0) return false;
        return p.PanX == 0 && p.PanZ == 0 && p.DragX == 0 && p.DragY == 0 && p.WheelDelta == 0 &&
            p.PointerX == 0 && p.PointerY == 0 && p.InspectPaneHeight == 0 && p.Trajectory.Id == 0;
    }

    internal static bool Valid(KernelTrajectory p, double now)
    {
        if (p.Id == 0)
        {
            return p.Kind == 0 && p.Flags == 0 && p.StartSeconds == 0 && p.DurationSeconds == 0 &&
                p.SourceX == 0 && p.SourceZ == 0 && p.SourceRootSize == 0 &&
                p.TargetX == 0 && p.TargetZ == 0 && p.TargetRootSize == 0;
        }

        // A start in the future would be an unbounded queued command, so it is rejected.
        if (p.Id > long.MaxValue || p.Kind != 1 || p.Flags != 0) return false;
        if (!Nonnegative(p.StartSeconds) || !Nonnegative(now) || p.StartSeconds > now) return false;
        if (!Nonnegative(p.DurationSeconds) || !Finite(p.StartSeconds + p.DurationSeconds)) return false;
        if (!Finite(p.SourceX) || !Finite(p.SourceZ) || !Finite(p.SourceRootSize) || p.SourceRootSize <= 0) return false;
        if (!Finite(p.TargetX) || !Finite(p.TargetZ) || !Finite(p.TargetRootSize) || p.TargetRootSize <= 0) return false;
        return Finite(p.TargetX - p.SourceX) && Finite(p.TargetZ - p.SourceZ) && Finite(p.TargetRootSize - p.SourceRootSize);
    }

    internal static bool Same(KernelTrajectory a, KernelTrajectory b)
    {
        return a.Id == b.Id && a.Kind == b.Kind && a.Flags == b.Flags &&
            a.StartSeconds == b.StartSeconds && a.DurationSeconds == b.DurationSeconds &&
            a.SourceX == b.SourceX && a.SourceZ == b.SourceZ && a.SourceRootSize == b.SourceRootSize &&
            a.TargetX == b.TargetX && a.TargetZ == b.TargetZ && a.TargetRootSize == b.TargetRootSize;
    }

    internal static CameraSettings ToSettings(KernelSettings p) => new CameraSettings
    {
        MinSize = p.MinSize,
        MaxSize = p.MaxSize,
        DollyRateKeys = p.DollyRateKeys,
        DollyRateScreenEdge = p.DollyRateScreenEdge,
        SpeedDecay = p.SpeedDecay,
        MoveSpeed = p.MoveSpeed,
        ZoomSpeed = p.ZoomSpeed,
        ScrollWheelRate = p.ScrollWheelRate,
        ZoomPreserveFactor = p.ZoomPreserveFactor,
        DragSensitivity = p.DragSensitivity,
        SmoothZoom = Has(p.Flags, SettingsFlags.SmoothZoom),
        ZoomToMouse = Has(p.Flags, SettingsFlags.ZoomToMouse),
        EdgeScroll = Has(p.Flags, SettingsFlags.EdgeScroll),
        Profile = DecodeProfile(p.Profile, Has(p.Flags, SettingsFlags.DisableZoomToMouseWhileShiftHeld)),
        Bounds = DecodeBounds(p.Bounds)
    };

    internal static CameraInput ToInput(KernelInput p, KernelSettings s) => new CameraInput
    {
        PanX = p.PanX,
        PanZ = p.PanZ,
        DragX = p.DragX,
        DragY = p.DragY,
        WheelDelta = p.WheelDelta,
        ZoomIn = Has(p.Flags, InputFlags.ZoomInPulse),
        ZoomOut = Has(p.Flags, InputFlags.ZoomOutPulse),
        FastPan = Has(p.Flags, InputFlags.FastPan),
        MotionBlocked = Has(p.Flags, InputFlags.MotionBlocked),
        MiddleReleased = Has(p.Flags, InputFlags.MiddleReleasedPulse),
        AllowEdgeScroll = Has(p.Flags, InputFlags.AllowEdgeScroll),
        Fullscreen = Has(p.Flags, InputFlags.Fullscreen),
        PointerOverUi = Has(p.Flags, InputFlags.PointerOverUi),
        PointerX = p.PointerX,
        PointerY = p.PointerY,
        InspectPaneHeight = p.InspectPaneHeight,
        Width = s.PixelWidth,
        Height = s.PixelHeight,
        UiScale = s.UiScale
    };

    internal static bool FiniteState(CameraMotion m)
    {
        return Finite(m.Pose.X) && Finite(m.Pose.Z) && Finite(m.Pose.Size) && m.Pose.Size > 0 &&
            Finite(m.DesiredSize) && Finite(m.VelocityX) && Finite(m.VelocityZ) &&
            Finite(m.ProjectionHalfHeight) && m.ProjectionHalfHeight > 0;
    }

    internal static bool TrySettings(KernelSettings packet, CameraPose pose, double desired, out CameraSettings values)
    {
        values = null!;
        try
        {
            values = ToSettings(packet);
            ValidateProfile(values, pose.Size, desired);
            ValidateProfile(values, packet.MinSize, packet.MinSize);
            ValidateProfile(values, packet.MaxSize, packet.MaxSize);
            return true;
        }
        catch (ArgumentException)
        {
            return false;
        }
        catch (ArithmeticException)
        {
            return false;
        }
    }

    // Evaluates every curve and bound at the given zoom so a packet that would throw mid-step is refused up front.
    internal static void ValidateProfile(CameraSettings settings, double current, double desired)
    {
        if (settings.Bounds?.MaximumSize is double maximumSize)
        {
            current = Math.Min(current, maximumSize);
            desired = Math.Min(desired, maximumSize);
        }

        settings.Bounds?.X?.Clamp(0, current);
        settings.Bounds?.Z?.Clamp(0, current);

        CameraProfile? profile = settings.Profile;
        if (profile == null) return;
        double projected = profile.Projection?.Evaluate(current) ?? current;
        if (!Finite(projected) || projected <= 0) throw new ArgumentException("Invalid projected size.");

        profile.KeyboardRate?.Evaluate(current, desired, projected);
        profile.EdgeRate?.Evaluate(current, desired, projected);
        profile.MoveSpeed?.Evaluate(current, desired, projected);
        profile.ZoomSpeed?.Evaluate(current, desired, projected);
    }

    private static CameraMotionBounds? DecodeBounds(KernelMovementBounds packet)
    {
        if (!Finite(packet.MaximumSize) || packet.MaximumSize < 0) throw new ArgumentException("Invalid bounds maximum size.");

        CameraAxisBounds? x = DecodeAxis(packet.X);
        CameraAxisBounds? z = DecodeAxis(packet.Z);
        if (x == null && z == null && packet.MaximumSize == 0) return null;
        return new CameraMotionBounds(x, z, packet.MaximumSize == 0 ? (double?)null : packet.MaximumSize);
    }

    private static unsafe CameraAxisBounds? DecodeAxis(KernelAxisBounds packet)
    {
        if (packet.Enabled == 0)
        {
            byte* data = (byte*)&packet;
            for (int i = 0; i < KernelAxisBounds.Bytes; i++)
            {
                if (data[i] != 0) throw new ArgumentException("Absent bounds axis must be zero.");
            }
            return null;
        }

        if (packet.Enabled != 1 || packet.Reserved != 0) throw new ArgumentException("Invalid bounds axis header.");
        return new CameraAxisBounds(packet.MinimumA, packet.MinimumB, packet.MinimumLimitA, packet.MinimumLimitB,
            packet.MaximumA, packet.MaximumB, packet.MaximumLimitA, packet.MaximumLimitB, packet.CollapsePosition);
    }

    private static CameraProfile? DecodeProfile(KernelProfilePack p, bool disableShift)
    {
        if (p.Reserved != 0 || (p.PresentMask & ~KnownProfileCurves) != 0) throw new ArgumentException("Invalid profile mask.");

        CameraCurve? projection = DecodeCurve(p.Projection, (p.PresentMask & 1) != 0);
        CameraCurve? keyboard = DecodeCurve(p.KeyboardRate, (p.PresentMask & 2) != 0);
        CameraCurve? edge = DecodeCurve(p.EdgeRate, (p.PresentMask & 4) != 0);
        CameraCurve? move = DecodeCurve(p.MoveSpeed, (p.PresentMask & 8) != 0);
        CameraCurve? zoom = DecodeCurve(p.ZoomSpeed, (p.PresentMask & 16) != 0);

        if (p.PresentMask == 0 && !disableShift) return null;
        return new CameraProfile(projection, keyboard, edge, move, zoom, disableZoomToMouseWhileShiftHeld: disableShift);
    }

    private static unsafe CameraCurve? DecodeCurve(KernelCurve p, bool present)
    {
        if (!present)
        {
            byte* bytes = (byte*)&p;
            for (int i = 0; i < KernelCurve.Bytes; i++)
            {
                if (bytes[i] != 0) throw new ArgumentException("Absent curve must be zero.");
            }
            return null;
        }

        if (p.Mode > 4 || p.Domain > 2 || p.PointCount > 16 || (p.Flags & ~1u) != 0)
            throw new ArgumentException("Invalid curve header.");
        if (!Finite(p.InputMin) || !Finite(p.InputMax) || !Finite(p.A) || !Finite(p.B) || !Finite(p.C) || !Finite(p.Exponent))
            throw new ArgumentException("Invalid curve header.");

        bool table = p.Mode >= 3;
        if ((!table && p.PointCount != 0) || (table && (p.PointCount == 0 || p.Flags != 0)))
            throw new ArgumentException("Invalid curve table.");
        for (int i = 0; i < 16; i++)
        {
            if (!Finite(p.X[i]) || !Finite(p.Y[i]) || (i >= p.PointCount && (p.X[i] != 0 || p.Y[i] != 0)))
                throw new ArgumentException("Invalid curve points.");
        }

        CameraCurveDomain domain = (CameraCurveDomain)p.Domain;
        bool clampInput = (p.Flags & 1) != 0;

        switch (p.Mode)
        {
            case 0:
                return CameraCurve.Constant(p.A, domain);
            case 1:
                return CameraCurve.PowerRange(p.InputMin, p.InputMax, p.A, p.B, p.Exponent, domain, clampInput);
            case 2:
                return CameraCurve.Polynomial2(p.InputMin, p.InputMax, p.A, p.B, p.C, domain, clampInput);
            default:
                CameraCurvePoint[] points = new CameraCurvePoint[p.PointCount];
                for (int i = 0; i < points.Length; i++)
                {
                    points[i] = new CameraCurvePoint(p.X[i], p.Y[i]);
                }
                return p.Mode == 3 ? CameraCurve.Step(points, domain) : CameraCurve.PiecewiseLinear(points, domain);
        }
    }

    // Mirrors the drag arithmetic in CameraMotion.Update, so an overflow is refused before
    // the map clamp could hide an infinity inside the drag queue.
    internal static bool FiniteDrag(KernelInput p, KernelSettings s, double projectionHalfHeight)
    {
        if (Has(p.Flags, InputFlags.MotionBlocked)) return true;
        double cellsPerPixel = 2 * projectionHalfHeight / s.PixelHeight;
        return Finite(cellsPerPixel) &&
            Finite(-p.DragX * cellsPerPixel * s.DragSensitivity) &&
            Finite(p.DragY * cellsPerPixel * s.DragSensitivity);
    }
}
