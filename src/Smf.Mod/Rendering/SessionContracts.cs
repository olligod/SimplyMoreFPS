#nullable disable
using System;
using System.Runtime.InteropServices;
using SimplyMoreFPS.Rendering.Lifecycle;

namespace SimplyMoreFPS.Rendering;

// Everything here runs on the Unity main thread and is not itself the native ABI;
// a platform client copies the plain fields into its own packets.
public interface INativeSession
{
    IntPtr RenderEvent { get; }

    long ClockFrequency { get; }

    // The platform's native clock, not a managed Stopwatch.
    long ClockNow();

    // Every int result: 0 accepted, 1 busy (retry later), negative failure.
    // Submit of RestoreNativeRouting must fence older activations before it returns.
    int Submit(Command command, SceneContext context);

    int PublishWorldFence(WorldFence fence);

    int PollWorldFence(out WorldFence acknowledged);

    int PollAcknowledgement(out Acknowledgement acknowledgement);

    // Queue methods copy the packet and AddRef the texture handles they accept.
    int QueuePreGui(ref FrameBundle frame, out NativeDispatch dispatch);

    int QueueFrame(ref FrameBundle frame, out NativeDispatch dispatch);

    int QueueNativeFrame(ref NativeFrameMarker marker, out NativeDispatch dispatch);

    // 0 also covers a token that was already consumed. Tokens are never reused across sessions.
    int Cancel(NativeDispatch dispatch);

    int RoutingRestored(Command ticket, ulong frame, Evidence evidence);
}

// Optional main-only extras of the process-lifetime platform clients, kept
// separate so the in-memory test clients do not have to implement them.
public interface IRetainedNativeSession
{
    ulong PreviousSession { get; }
}

public interface IProcessExitFence
{
    void FenceProcessExit();
}

public enum PresentationMetric
{
    CompositionCompletions = 1,
    CompletedSwaps = 2,
    PresentedDrawables = 3
}

public readonly struct PresentationSample
{
    public readonly ulong Session;
    public readonly ulong Generation;
    public readonly ulong Count;
    public readonly long Timestamp;
    public readonly long Frequency;
    public readonly PresentationMetric Metric;

    public PresentationSample(ulong session, ulong generation, ulong count, long timestamp, long frequency, PresentationMetric metric)
    {
        Session = session;
        Generation = generation;
        Count = count;
        Timestamp = timestamp;
        Frequency = frequency;
        Metric = metric;
    }
}

public interface IPresentationTelemetry
{
    bool TryReadPresentation(out PresentationSample sample);
}

public struct NativeDispatch
{
    public IntPtr Ticket;
    public int Token;
}

public interface IMainSceneOwner
{
    SceneContext ReadContext();

    bool TryReadMapPose(ulong sourceFrame, out CameraPose pose);

    void ValidateGuiAlpha();

    // Idempotent; must restore camera input routing and velocity before reporting success.
    void ReleaseCamera();

    bool CameraReleased { get; }
}

public interface IMainSceneLifetime
{
    void Start(UnityEngine.GameObject owner);

    void EnableCamera();

    void Stop();
}

[StructLayout(LayoutKind.Sequential, Pack = 8)]
public struct SceneContext : IEquatable<SceneContext>
{
    public ulong Revision;
    public int SceneHandle;
    public int RootId;
    public int MapId;
    public int CameraId;
    public uint Width, Height;
    public uint UiWidth, UiHeight;
    public float UiScale;
    public uint HasMap;
    public uint ColorSpace;
    public uint GraphicsApi;

    // Revision is left out on purpose; camera motion and UI focus are not part of the context at all.
    public bool Equals(SceneContext other)
    {
        return SceneHandle == other.SceneHandle &&
            RootId == other.RootId &&
            MapId == other.MapId &&
            CameraId == other.CameraId &&
            Width == other.Width &&
            Height == other.Height &&
            UiWidth == other.UiWidth &&
            UiHeight == other.UiHeight &&
            UiScale == other.UiScale &&
            HasMap == other.HasMap &&
            ColorSpace == other.ColorSpace &&
            GraphicsApi == other.GraphicsApi;
    }
}

[StructLayout(LayoutKind.Sequential, Pack = 8)]
public struct FrameKey
{
    public ulong Session;
    public ulong Content;
    public ulong Generation;
    public ulong SourceFrame;
}

// The matrices come from the Unity camera that completed this frame, not from a
// reconstructed orthographic guess or the worker's latest desired pose.
[StructLayout(LayoutKind.Sequential, Pack = 8)]
public unsafe struct CameraPose
{
    public uint Size;
    public uint Version;
    public ulong FrameId;
    public ulong UnityFrame;
    public ulong CameraId;
    public ulong Epoch;
    public float X, Y, Z;
    public float OrthographicSize;
    public float PixelX, PixelY, PixelWidth, PixelHeight;
    public fixed float WorldToCamera[16];
    public fixed float Projection[16];
    public ulong CameraEpoch;
    public ulong AppliedSequence;
    public ulong ModelRevision;
    public int MapId;
    public uint Reserved;
    public double RootX, RootY, RootZ;
    public double RootSize;
}

[Flags]
public enum FrameFlags : uint
{
    None = 0,
    FlipY = 1,
    HasMap = 2,
    WorldDispatchCompleted = 4,
    WorldDispatchAbsent = 8,
    CompositeBackQueued = 16
}

// The base layer carries no texture handle: QueuePreGui copies the window's
// full-client render target at this ordered boundary and rejects any other target.
public struct FrameBundle
{
    public FrameKey Key;
    public ulong HudTexture;
    public ulong WorldTexture;
    public uint Width, Height;
    public uint WorldDispatches;
    public FrameFlags Flags;
    public long PreGuiClock;
    public long EofClock;
    public CameraPose Pose;
    public ulong CoverageTexture;
    public ulong CoverageSerial;
    public uint CoverageWidth, CoverageHeight;
    public uint CoverageFlags;
    public uint CoverageReserved;
    public double CoverageA, CoverageB, CoverageC, CoverageD, CoverageE, CoverageF;
}

// Marks an ordered end of frame only; the native owner waits for the matching
// swapchain present to complete before it reports the frame available.
public struct NativeFrameMarker
{
    public ulong Session;
    public ulong Content;
    public ulong Generation;
    public ulong SourceFrame;
    public ulong RestoreSerial;
    public long Clock;
    public uint Width, Height;
    public bool BeginOnly;
}
