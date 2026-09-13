#nullable disable
using System;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using SimplyMoreFPS.Rendering.Lifecycle;
using SimplyMoreFPS.Rendering.Mac;
using UnityEngine;
using UnityEngine.Rendering;

namespace SimplyMoreFPS.Rendering;

// Main-thread client of the Metal renderer bundle that Unity loaded itself.
public sealed partial class MacRendererApi : INativeSession, IRetainedNativeSession, IProcessExitFence, IPresentationTelemetry
{
    private readonly MacUnityModule module;
    private readonly int owner = Thread.CurrentThread.ManagedThreadId;
    private readonly ulong window;
    private readonly BaseFn stageBase;
    private readonly CapabilitiesFn capabilities;
    private readonly ClockFn nativeNow;
    private readonly QuitFn quit;
    private readonly StartFn start;
    private readonly CommandFn command;
    private readonly FenceFn fence;
    private readonly AckFn ack;
    private readonly StatusFn status;
    private readonly PreGuiFn preGui;
    private readonly FrameFn frame;
    private readonly NativeFrameFn nativeFrame;
    private readonly CancelFn cancel;
    private readonly JoinedFn joined;
    private PresentationFn presentation;
    private readonly RendererAcknowledgementState acknowledgements = new RendererAcknowledgementState();
    private ulong startedSession;
    private ulong fenceDelivered;
    private ulong fencePublished;
    private ulong reportedFailureSession;

    public IntPtr RenderEvent
    {
        get;
    }
    public long ClockFrequency
    {
        get;
    }
    public ulong PreviousSession
    {
        get;
    }

    public static int FindOriginalWindow(string bundleDirectory, out ulong originalWindow)
    {
        var module = MacUnityModule.Open(bundleDirectory);
        return module.Bind<WindowFn>("smf_mac_find_original_window")(out originalWindow);
    }

    public MacRendererApi(string bundleDirectory, ulong originalWindow)
    {
        if (IntPtr.Size != 8
            || Application.platform != RuntimePlatform.OSXPlayer
            || originalWindow == 0
            || SystemInfo.graphicsDeviceType != GraphicsDeviceType.Metal)
            throw new PlatformNotSupportedException("The persistent Mac renderer requires Metal and the verified original NSWindow.");

        AssertAbi();
        window = originalWindow;
        module = MacUnityModule.Open(bundleDirectory);

        stageBase = module.Bind<BaseFn>("smf_mac_source_base");
        capabilities = module.Bind<CapabilitiesFn>("smf_mac_capabilities");
        enableSourceTarget = module.Bind<SourceTargetEnableFn>("smf_mac_source_target_enable");
        stageSourceTarget = module.Bind<SourceTargetFn>("smf_mac_source_target");
        stageNativeTarget = module.Bind<NativeTargetFn>("smf_mac_native_target");
        start = module.Bind<StartFn>("smf_session_start");
        command = module.Bind<CommandFn>("smf_session_command");
        fence = module.Bind<FenceFn>("smf_session_content_fence");
        ack = module.Bind<AckFn>("smf_session_ack");
        status = module.Bind<StatusFn>("smf_session_status");
        preGui = module.Bind<PreGuiFn>("smf_session_pre_gui");
        frame = module.Bind<FrameFn>("smf_session_frame");
        nativeFrame = module.Bind<NativeFrameFn>("smf_session_native_frame");
        cancel = module.Bind<CancelFn>("smf_session_cancel");

        RenderEvent = module.Bind<EventFn>("smf_session_render_event")();
        module.VerifyCallback(RenderEvent);

        var loaded = new MacCapabilities { Size = 136, Version = 1 };
        if (capabilities(ref loaded, 136) != 0 || loaded.Size != 136 || loaded.Version != 1 || loaded.NativeLoads != 1 || loaded.Interfaces == 0)
            throw new InvalidOperationException("Unity did not register the external native renderer through its plugin loader.");
        joined = module.Bind<JoinedFn>("smf_session_poll_joined");

        // The native module outlives this assembly. A new session id must stay
        // above whatever the module already handed out.
        var previous = new StatusPacket { Size = 472, Version = 1 };
        int previousResult = status(ref previous, 472);
        if (previousResult != 0)
            throw new InvalidOperationException("Native session identity is busy or unavailable; retry installation after owner retirement.");
        if (previous.Session != 0 && (previous.Size != 472 || previous.Version != 1))
            throw new InvalidOperationException("Native session identity ABI mismatch.");

        PreviousSession = previous.Session;

        nativeNow = module.Bind<ClockFn>("smf_session_clock_now");
        long frequency = module.Bind<ClockFn>("smf_session_clock_frequency")();
        if (frequency <= 0 || RenderEvent == IntPtr.Zero)
            throw new InvalidOperationException("Native render callback/platform monotonic clock is unavailable.");
        ClockFrequency = frequency;
        quit = module.Bind<QuitFn>("smf_session_quit");
    }

    private void RequireMainThread()
    {
        if (Thread.CurrentThread.ManagedThreadId != owner)
            throw new InvalidOperationException("Session ABI client belongs to Unity main.");
    }

    public long ClockNow()
    {
        RequireMainThread();
        return nativeNow();
    }

    public void FenceProcessExit()
    {
        RequireMainThread();
        if (quit() != 0)
            throw new InvalidOperationException("Mac native process-exit fence failed.");
    }

    public int Submit(Command value, SceneContext context)
    {
        RequireMainThread();
        if (value.Session != startedSession)
        {
            int started = start(window, value.Session);
            if (started != 0)
                return started;

            startedSession = value.Session;
            fenceDelivered = 0;
            fencePublished = 0;
            acknowledgements.ClearWarmupMarkers();
        }

        if (value.Operation == Operation.PrepareHiddenGeneration || value.Operation == Operation.PrepareReplacementGeneration)
        {
            if (sourceTargetSession != value.Session)
            {
                int enabled = enableSourceTarget(value.Session);
                if (enabled != 0)
                    return enabled;
                sourceTargetSession = value.Session;
            }

            // The first fence may have been queued before native start, so
            // publish it before the prepare is validated against it.
            if (value.ContentRevision > fencePublished)
            {
                int published = fence(value.Session, value.ContentRevision);
                if (published != 0)
                    return published;
                fencePublished = value.ContentRevision;
            }
        }

        var packet = new CommandPacket
        {
            Size = 88,
            Version = 1,
            Session = value.Session,
            Serial = value.Serial,
            Generation = value.Generation,
            Content = value.ContentRevision,
            Previous = value.PreviousGeneration,
            AfterFrame = value.AfterFrame,
            Operation = (uint)value.Operation,
            Flags = value.HasWorld ? 1u : 0u,
            Width = context.Width,
            Height = context.Height,
            // Reserved clear color; the ABI keeps the fields.
            EmptyR = 0,
            EmptyG = 0,
            EmptyB = 0,
            EmptyA = 1
        };

        int result = command(ref packet, 88);
        if (result == 0)
        {
            acknowledgements.Accept(value);
        }

        return result;
    }

    public int PublishWorldFence(WorldFence value)
    {
        RequireMainThread();
        if (value.Session != startedSession)
            return 1;
        if (value.ContentRevision <= fencePublished)
            return 0;

        int result = fence(value.Session, value.ContentRevision);
        if (result == 0)
            fencePublished = value.ContentRevision;
        return result;
    }

    private static bool ReportNativeFailureOnce(ulong session, int result, ref ulong reported)
    {
        if (session == 0 || result >= 0 || reported == session)
            return false;
        reported = session;
        return true;
    }

    private int ReportCaptureResult(int result)
    {
        // A capture can surface the latched owner fault before the next fence
        // poll. Mark it delivered so the poll does not report it as a cleanup failure.
        if (result < 0 && ReadStatus(out StatusPacket state) == 0 && state.Result == result)
            ReportNativeFailureOnce(state.Session, state.Result, ref reportedFailureSession);
        return result;
    }

    public int PollWorldFence(out WorldFence value)
    {
        RequireMainThread();
        value = default;
        if (startedSession == 0)
            return 1;

        int result = ReadStatus(out StatusPacket state);
        if (result != 0)
            return result;

        // Report a permanent owner fault once; throwing every poll would stop
        // the lifecycle from reaching cleanup.
        if (ReportNativeFailureOnce(state.Session, state.Result, ref reportedFailureSession))
            return state.Result;
        if (state.ContentAcknowledged == 0 || state.ContentAcknowledged <= fenceDelivered)
            return 1;

        fenceDelivered = state.ContentAcknowledged;
        value = new WorldFence { Session = state.Session, ContentRevision = fenceDelivered };
        return 0;
    }

    public int PollAcknowledgement(out Acknowledgement value)
    {
        RequireMainThread();
        value = default;
        if (!acknowledgements.Pending.HasValue)
            return 1;
        Command wanted = acknowledgements.Pending.Value;
        if (wanted.Operation == Operation.StopWorker)
        {
            int joinResult = joined(wanted.Session);
            if (joinResult != 0)
                return joinResult;
        }

        var packet = new AckPacket { Size = 80, Version = 1 };
        int result = ack(wanted.Session, wanted.Serial, ref packet, 80);
        if (result != 0)
            return result;

        if (packet.Size != 80 || packet.Version != 1)
        {
            throw new InvalidOperationException("Native acknowledgment ABI mismatch.");
        }

        acknowledgements.ValidateTicket(packet.Session, packet.Serial, packet.Generation,
            packet.Content, packet.Operation, packet.Disposition);
        bool success = packet.Disposition == 1 && packet.Result == 0;
        if (!acknowledgements.TryReadEvidence(packet.Evidence, success, out Evidence evidence))
        {
            return 1;
        }

        ulong sourceFrame = packet.SourceFrame;
        if (success && wanted.Operation == Operation.RestoreNativeRouting)
        {
            // Native completion can follow the main report, so keep the later frame.
            sourceFrame = Math.Max(acknowledgements.RoutingFrame, packet.SourceFrame);
        }

        if (success && wanted.Operation == Operation.PrepareHiddenGeneration)
        {
            // Submission alone is not a reveal. Stay pending until native has
            // actually shown a frame composited from our source.
            int read = ReadStatus(out StatusPacket state);
            if (read != 0)
                return read;

            if (acknowledgements.TrySupersedePreparation(sourceFrame, evidence,
                state.ContentFence, state.ContentAcknowledged, out value))
            {
                return 0;
            }

            if (!acknowledgements.WarmupRevealed(sourceFrame, state.Flags, state.NativeRevealFrame,
                state.NativeSubmittedGeneration, state.NativeSubmittedContent, state.NativeSubmittedRestoreSerial))
            {
                return 1;
            }

            evidence |= Evidence.NativeFullUiMaintained;
        }

        return acknowledgements.Complete(packet.Disposition, packet.Result, evidence, sourceFrame, out value);
    }

    public int RoutingRestored(Command ticket, ulong actualFrame, Evidence actualEvidence)
    {
        RequireMainThread();
        acknowledgements.RoutingRestored(ticket, actualFrame, actualEvidence);
        return 0;
    }

    public int QueuePreGui(ref FrameBundle value, out NativeDispatch dispatch)
    {
        RequireMainThread();
        dispatch = default;
        int staged = StageCurrentSourceTarget(ref value);
        if (staged != 0)
            return staged;

        var packet = new PreGuiPacket
        {
            Size = 64,
            Version = 1,
            Session = value.Key.Session,
            Content = value.Key.Content,
            Generation = value.Key.Generation,
            SourceFrame = value.Key.SourceFrame,
            Width = value.Width,
            Height = value.Height,
            Bootstrap = value.HudTexture,
            Flags = EncodeFlags(value.Flags) & 1u
        };

        int result = preGui(ref packet, 64, out IntPtr ticket, out int token);
        dispatch = new NativeDispatch { Ticket = ticket, Token = token };
        return ReportCaptureResult(result);
    }

    public int QueueFrame(ref FrameBundle value, out NativeDispatch dispatch)
    {
        RequireMainThread();

        var packet = new FramePacket
        {
            Size = 424,
            Version = 4,
            Session = value.Key.Session,
            Content = value.Key.Content,
            Generation = value.Key.Generation,
            SourceFrame = value.Key.SourceFrame,
            World = value.WorldTexture,
            Hud = value.HudTexture,
            EofQpc = value.EofClock,
            Flags = EncodeFlags(value.Flags),
            WorldDispatches = value.WorldDispatches,
            Pose = value.Pose,
            CoverageTexture = value.CoverageTexture,
            CoverageSerial = value.CoverageSerial,
            CoverageWidth = value.CoverageWidth,
            CoverageHeight = value.CoverageHeight,
            CoverageFlags = value.CoverageFlags,
            CoverageReserved = value.CoverageReserved,
            CoverageA = value.CoverageA,
            CoverageB = value.CoverageB,
            CoverageC = value.CoverageC,
            CoverageD = value.CoverageD,
            CoverageE = value.CoverageE,
            CoverageF = value.CoverageF,
            SceneDescription = value.SceneDescription
        };

        int result = frame(ref packet, 424, out IntPtr ticket, out int token);
        dispatch = new NativeDispatch { Ticket = ticket, Token = token };
        return ReportCaptureResult(result);
    }

    public int QueueNativeFrame(ref NativeFrameMarker value, out NativeDispatch dispatch)
    {
        RequireMainThread();
        dispatch = default;
        if (!value.BeginOnly)
        {
            int staged = StageCurrentNativeTarget(ref value);
            if (staged != 0)
                return staged;
        }

        var packet = new NativeFramePacket
        {
            Size = 64,
            Version = 1,
            Session = value.Session,
            RestoreSerial = value.RestoreSerial,
            SourceFrame = value.SourceFrame,
            Content = value.Content,
            Generation = value.Generation,
            Width = value.Width,
            Height = value.Height,
            Flags = value.BeginOnly ? 1u : 0u
        };

        int result = nativeFrame(ref packet, 64, out IntPtr ticket, out int token);
        dispatch = new NativeDispatch { Ticket = ticket, Token = token };
        if (result == 0)
        {
            acknowledgements.RecordWarmupMarker(value);
        }

        return result;
    }

    public int Cancel(NativeDispatch value)
    {
        RequireMainThread();
        return cancel(value.Ticket, value.Token);
    }

    private int ReadStatus(out StatusPacket value)
    {
        value = new StatusPacket { Size = 472, Version = 1 };
        if (startedSession == 0)
            return 1;
        int result = status(ref value, 472);
        if (result == 0 && (value.Size != 472 || value.Version != 1 || value.Session != startedSession))
            throw new InvalidOperationException("Session status ABI/session mismatch.");
        return result;
    }

    public string ReadFailureDiagnostics()
    {
        RequireMainThread();
        string geometry;
        try
        {
            geometry = ReadGeometryFailureDiagnostics();
        }
        catch (Exception error)
        {
            geometry = "geometryFailure=read-failed\n" + RendererDiagnostics.FormatException(error);
        }

        return geometry + "\n" + ReadTerminalFailureDiagnostics();
    }

    // Optional export; older bundles do not have it.
    private string ReadGeometryFailureDiagnostics()
    {
        try
        {
            var read = module.Bind<GeometryFailureFn>("smf_mac_geometry_failure");
            var value = new GeometryFailurePacket { Size = 96, Version = 1 };
            int result = read(ref value, 96);

            if (result != 0)
                return "geometryFailureRead=" + result;
            if (value.Size != 96 || value.Version != 1)
                return "geometryFailureAbiMismatch";

            var text = new StringBuilder("Mac display setup failed: ").Append(GeometryFailureReason(value.Reason));
            text.Append("; game image=").Append(value.RequestedWidth).Append('x').Append(value.RequestedHeight)
                .Append("; window pixels=").Append(value.RefreshedWidth).Append('x').Append(value.RefreshedHeight)
                .Append("; backing scale=").Append(value.BackingScale).AppendLine();
            text.Append("geometryFailure");
            foreach (var field in typeof(GeometryFailurePacket).GetFields())
            {
                text.Append(' ').Append(field.Name).Append('=').Append(field.GetValue(value));
            }

            return text.ToString();
        }
        catch (EntryPointNotFoundException)
        {
            return "geometryFailure=unavailable-in-native-build";
        }
    }

    private static string GeometryFailureReason(uint reason)
    {
        switch (reason)
        {
            case 1:
                return "the original Unity window or attached rendering layer is unavailable";
            case 2:
                return "the window bounds or display scale are invalid";
            case 3:
                return "the window pixel dimensions are unsupported";
            case 4:
                return "Unity's rendering layer has an unsupported layout, transform, or drawable size";
            case 5:
                return "Unity's color format or extended dynamic range mode is unsupported";
            case 6:
                return "the captured image and original window do not share the same Metal device";
            case 7:
                return "the game image size differs from the window's physical pixel size";
            case 8:
                return "the attached rendering layer changed Metal device";
            default:
                return "unclassified geometry failure (reason " + reason + ")";
        }
    }

    private static uint EncodeFlags(FrameFlags flags)
    {
        uint native = (flags & FrameFlags.HasMap) != 0 ? 1u : 0u;
        if ((flags & FrameFlags.WorldDispatchCompleted) != 0)
            native |= 2u;
        if ((flags & FrameFlags.WorldDispatchAbsent) != 0)
            native |= 4u;
        if ((flags & FrameFlags.FlipY) != 0)
            native |= 8u | 16u;

        return native;
    }

    public bool TryReadPresentation(out PresentationSample sample)
    {
        RequireMainThread();
        sample = default;
        if (startedSession == 0)
            return false;

        // Bound lazily so a bundle without the meter export still installs.
        if (presentation == null)
            presentation = module.Bind<PresentationFn>("smf_mac_presentation");

        var packet = new PresentationPacket { Size = 48, Version = 1 };
        int result = presentation(ref packet, 48);
        if (result < 0)
            throw new InvalidOperationException("Metal presentation telemetry failed: " + result);
        if (result != 0)
            return false;
        if (packet.Size != 48 || packet.Version != 1 || packet.Session != startedSession || packet.Generation == 0 || packet.Frequency <= 0)
            throw new InvalidOperationException("Metal presentation telemetry ABI/session mismatch.");

        sample = new PresentationSample(packet.Session, packet.Generation, packet.Count, packet.Timestamp, packet.Frequency, PresentationMetric.PresentedDrawables);
        return true;
    }

    public int StageSourceBase(ref MacSourceBase value)
    {
        RequireMainThread();
        if (value.Size != 96
            || value.Version != 1
            || value.Session != startedSession
            || value.Session == 0
            || value.Generation == 0
            || value.ContentRevision == 0
            || value.SourceFrame == 0
            || value.Width == 0
            || value.Height == 0
            || value.Width > 32768
            || value.Height > 32768
            || value.Texture == 0
            || value.NativeRenderBuffer == 0
            || value.OriginalWindow != window
            || value.OriginalLayer == 0
            || value.PixelFormat != 80 && value.PixelFormat != 81
            || (value.Flags & ~1u) != 0
            || value.Reserved != 0)
            throw new ArgumentException("An explicit matching owned source texture descriptor is required.");

        return stageBase(ref value, 96);
    }

    public int ReadCapabilities(out MacCapabilities value)
    {
        RequireMainThread();
        value = new MacCapabilities { Size = 136, Version = 1 };
        int result = capabilities(ref value, 136);
        if (result == 0 && (value.Size != 136 || value.Version != 1 || value.Session != startedSession))
            throw new InvalidOperationException("Mac capabilities ABI/session mismatch.");

        return result;
    }

    // Sizes and offsets are a contract with the native side.
    public static void AssertAbi()
    {
        AssertSourceTargetAbi();
        AssertSize<MacSourceBase>(96);
        AssertSize<MacCapabilities>(136);
        AssertSize<CameraPose>(264);
        AssertSize<CommandPacket>(88);
        AssertSize<PreGuiPacket>(64);
        AssertSize<FramePacket>(424);
        AssertSize<NativeFramePacket>(64);
        AssertSize<AckPacket>(80);
        AssertSize<GenerationStatus>(128);
        AssertSize<StatusPacket>(472);
        AssertSize<GeometryFailurePacket>(96);

        if ((int)Marshal.OffsetOf(typeof(CameraPose), nameof(CameraPose.RootX)) != 232
            || (int)Marshal.OffsetOf(typeof(FramePacket), nameof(FramePacket.Pose)) != 72
            || (int)Marshal.OffsetOf(typeof(FramePacket), nameof(FramePacket.CoverageTexture)) != 336
            || (int)Marshal.OffsetOf(typeof(FramePacket), nameof(FramePacket.CoverageSerial)) != 344
            || (int)Marshal.OffsetOf(typeof(FramePacket), nameof(FramePacket.CoverageA)) != 368
            || (int)Marshal.OffsetOf(typeof(FramePacket), nameof(FramePacket.SceneDescription)) != 416)
            throw new InvalidOperationException("Session ABI field offsets differ from the native header.");
    }

    private static void AssertSize<T>(int expected)
    {
        if (Marshal.SizeOf(typeof(T)) != expected)
            throw new InvalidOperationException(typeof(T).Name + " ABI size mismatch.");
    }

    [StructLayout(LayoutKind.Sequential, Pack = 8)]
    private struct CommandPacket
    {
        public uint Size, Version;
        public ulong Session, Serial, Generation, Content, Previous, AfterFrame;
        public uint Operation, Flags, Width, Height;
        public float EmptyR, EmptyG, EmptyB, EmptyA;
    }

    [StructLayout(LayoutKind.Sequential, Pack = 8)]
    private struct PreGuiPacket
    {
        public uint Size, Version;
        public ulong Session, Content, Generation, SourceFrame;
        public uint Width, Height;
        public ulong Bootstrap;
        public uint Flags, Reserved;
    }

    [StructLayout(LayoutKind.Sequential, Pack = 8)]
    private struct FramePacket
    {
        public uint Size, Version;
        public ulong Session, Content, Generation, SourceFrame, World, Hud;
        public long EofQpc;
        public uint Flags, WorldDispatches;
        public CameraPose Pose;
        public ulong CoverageTexture, CoverageSerial;
        public uint CoverageWidth, CoverageHeight, CoverageFlags, CoverageReserved;
        public double CoverageA, CoverageB, CoverageC, CoverageD, CoverageE, CoverageF;
        public ulong SceneDescription;
    }

    [StructLayout(LayoutKind.Sequential, Pack = 8)]
    private struct NativeFramePacket
    {
        public uint Size, Version;
        public ulong Session, RestoreSerial, SourceFrame, Content, Generation;
        public uint Width, Height, Flags, Reserved;
    }

    [StructLayout(LayoutKind.Sequential, Pack = 8)]
    private struct AckPacket
    {
        public uint Size, Version;
        public ulong Session, Serial, Generation, Content, SourceFrame;
        public uint Operation, Evidence;
        public int Result;
        public uint Disposition;
        public ulong CommitSerial;
        public long CompletedQpc;
    }

    [StructLayout(LayoutKind.Sequential, Pack = 8)]
    private struct GenerationStatus
    {
        public ulong Generation, Content, LastFrame, PreparedFrame, Frames, SourceCommit, CompletedCommit, Attachment, AttachmentCompleted, RetiredSerial;
        public uint State, Width, Height, Flags;
        public ulong RetireRequested, StagedFrame;
        public uint BaseFormat, WorldFormat, HudFormat, Reserved;
    }

    [StructLayout(LayoutKind.Sequential, Pack = 8)]
    private struct StatusPacket
    {
        public uint Size, Version;
        public ulong Session, ContentFence, ContentAcknowledged, OperationFence, ActiveGeneration, ActiveFrame, LastAckSerial;
        public uint MainThread, RenderThread, WorkerThread, CompletionThread, WorkerState, Stage;
        public int Result;
        public uint Flags;
        public ulong SourceCommit, SourceCompleted, WorkerCommit, WorkerCompleted, DroppedFrames, QueuedCallbacks;
        public GenerationStatus Generation0, Generation1;
        public ulong NativeOrderedFrame, NativeOrderedRestoreSerial, NativeSubmittedFrame, NativeRevealFrame;
        public ulong NativeSubmittedGeneration, NativeSubmittedContent, NativeSubmittedRestoreSerial, NativePresentSerial, NativeBackbuffer;
    }

    [StructLayout(LayoutKind.Sequential, Pack = 8)]
    private struct PresentationPacket
    {
        public uint Size, Version;
        public ulong Session, Generation, Count;
        public long Timestamp, Frequency;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct GeometryFailurePacket
    {
        public uint Size, Version;
        public int Result;
        public uint Reason;
        public uint RequestedWidth, RequestedHeight, RefreshedWidth, RefreshedHeight;
        public double BoundsWidth, BoundsHeight, BackingScale, DrawableWidth, DrawableHeight;
        public ulong SourceDevice, OriginalDevice;
        public uint Flags, Reserved;
    }

    [StructLayout(LayoutKind.Sequential, Pack = 8)]
    public struct MacSourceBase
    {
        public uint Size, Version;
        public ulong Session, ContentRevision, Generation, SourceFrame;
        public ulong Texture, NativeRenderBuffer, OriginalWindow, OriginalLayer;
        public uint Width, Height, PixelFormat, Flags;
        public ulong Reserved;
    }

    [StructLayout(LayoutKind.Sequential, Pack = 8)]
    public struct MacCapabilities
    {
        public uint Size, Version, Flags, Result;
        public ulong Session, OriginalWindow, OriginalLayer, OverlayLayer;
        public ulong NativeLoads, Interfaces, SourceDevice, MainThread, RenderThread, WorkerThread;
        public ulong SourceCompleted, WorkerCompleted, WorkerPresented;
        public ulong OriginalPresented, OriginalPresentedWithOverlay;
    }

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate int BaseFn(ref MacSourceBase value, uint bytes);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate int WindowFn(out ulong originalNSWindow);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate int CapabilitiesFn(ref MacCapabilities value, uint bytes);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate int StartFn(ulong hwnd, ulong session);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate int CommandFn(ref CommandPacket packet, uint bytes);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate int FenceFn(ulong session, ulong revision);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate int AckFn(ulong session, ulong serial, ref AckPacket packet, uint bytes);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate int StatusFn(ref StatusPacket packet, uint bytes);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate int PreGuiFn(ref PreGuiPacket packet, uint bytes, out IntPtr ticket, out int token);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate int FrameFn(ref FramePacket packet, uint bytes, out IntPtr ticket, out int token);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate int NativeFrameFn(ref NativeFramePacket packet, uint bytes, out IntPtr ticket, out int token);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate int CancelFn(IntPtr ticket, int token);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate IntPtr EventFn();

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate int JoinedFn(ulong session);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate long ClockFn();

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate int QuitFn();

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate int GeometryFailureFn(ref GeometryFailurePacket value, uint bytes);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate int PresentationFn(ref PresentationPacket packet, uint bytes);
}
