#nullable disable
using System;
using System.Runtime.InteropServices;

namespace SimplyMoreFPS.Rendering.CameraControl;

// Main-thread only. The native binding copies these PODs across the C ABI;
// no managed reference or callback ever crosses into NativeAOT.
public interface ICameraWorker
{
    // 0 = complete snapshot, 1 = unavailable, <0 = fault.
    int ReadDesired(ref DesiredPose pose);

    // 0 = copied, 1 = busy (retry at the next boundary), <0 = fault.
    int PublishMain(ref MainCameraState state);

    int Relinquish(ulong newEpoch, uint reason);
}

[StructLayout(LayoutKind.Sequential, Pack = 8)]
public struct DesiredPose
{
    public const uint VersionValue = 2, ByteSize = 88;
    public uint Version, Size;
    public ulong Epoch, Sequence;
    public int MapId;
    public uint Flags; // Reserved, must be zero.
    public double X, Z, RootSize;
    public double ProjectionHalfHeight;
    public ulong ActivePanId, FinishedPanId;
    public uint PanFlags, Reserved;
}

[StructLayout(LayoutKind.Sequential, Pack = 8)]
public struct MainCameraState
{
    public const uint VersionValue = 2, ByteSize = 112;
    public uint Version, Size;
    public ulong Epoch, AppliedSequence, SourceFrame;
    public int MapId;
    public uint Flags;
    public double X, Z, RootSize, MinSize, MaxSize, UiScale;
    public uint PixelWidth, PixelHeight, Reason, Reserved;
    public double ProjectionHalfHeight;
}

[Flags]
public enum MainFlags : uint
{
    None = 0,
    Eligible = 1,
    Owned = 2,
    MotionBlocked = 4,
    TextCaptured = 8,
    SearchFocused = 16,
    NativePan = 32
}

public enum ReleaseReason : uint
{
    None,
    Removed,
    MapChanged,
    ExternalCameraCall,
    NativePoseChanged,
    UnsupportedState,
    InvalidPacket,
    WorkerFailure,
    LoopChanged,
    CallbackFailure
}

public static class PoseValidation
{
    public static bool Matches(DesiredPose p, ulong epoch, int mapId, ulong afterSequence)
    {
        return p.Version == DesiredPose.VersionValue
            && p.Size == DesiredPose.ByteSize
            && p.Flags == 0
            && p.Reserved == 0
            && (p.PanFlags & ~1u) == 0
            && p.Epoch == epoch
            && p.MapId == mapId
            && p.Sequence != 0
            && p.Sequence > afterSequence;
    }

    public static bool CanConvertToUnity(DesiredPose p)
    {
        // Vanilla clamps translation before zoom-to-mouse, so a valid pose can
        // sit past the map bounds. Do not add another clamp here.
        return FitsFloat(p.X)
            && FitsFloat(p.Z)
            && FitsFloat(p.RootSize)
            && p.RootSize > 0
            && (float)p.RootSize > 0
            && FitsFloat(p.ProjectionHalfHeight)
            && p.ProjectionHalfHeight > 0
            && (float)p.ProjectionHalfHeight > 0;
    }

    private static bool FitsFloat(double n)
    {
        return !double.IsNaN(n) && !double.IsInfinity(n) && n >= -float.MaxValue && n <= float.MaxValue;
    }
}

// Test double. Tests queue explicit poses using the epoch from LastMain;
// nothing moves or resets on its own.
public sealed class FakeCameraWorker : ICameraWorker
{
    public MainCameraState LastMain;
    public ulong RelinquishedEpoch;
    public uint RelinquishedReason;
    public int Reads, Publications, Relinquishes;
    private DesiredPose pending;
    private bool hasPending;

    public void Queue(DesiredPose pose)
    {
        pending = pose;
        hasPending = true;
    }

    public int ReadDesired(ref DesiredPose pose)
    {
        Reads++;
        if (!hasPending) return 1;
        pose = pending;
        hasPending = false;
        return 0;
    }

    public int PublishMain(ref MainCameraState state)
    {
        LastMain = state;
        Publications++;
        return 0;
    }

    public int Relinquish(ulong epoch, uint reason)
    {
        RelinquishedEpoch = epoch;
        RelinquishedReason = reason;
        Relinquishes++;
        // A queued packet stays queued so tests can prove stale-epoch rejection.
        return 0;
    }
}
