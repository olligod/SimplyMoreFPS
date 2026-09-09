#nullable disable
using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.InteropServices;
using System.Threading;
using SimplyMoreFPS.Rendering.Lifecycle;
using UnityEngine;

namespace SimplyMoreFPS.Rendering;

// Unity main-thread client for the Linux renderer; same session ABI as Windows plus a native clock and quit fence.
public sealed class LinuxRendererApi : INativeSession, IRetainedNativeSession, IProcessExitFence, IPresentationTelemetry
{
    // Loaded modules stay mapped until the process exits.
    private static readonly Dictionary<string, NativeModule> Modules = new Dictionary<string, NativeModule>(StringComparer.Ordinal);

    private readonly int mainThread = Thread.CurrentThread.ManagedThreadId;
    private readonly ulong window;
    private readonly StartFn startSession;
    private readonly CommandFn sendCommand;
    private readonly FenceFn publishFence;
    private readonly AckFn readAck;
    private readonly StatusFn queryStatus;
    private readonly PlatformStatusFn queryPlatformStatus;
    private readonly PreGuiFn queuePreGui;
    private readonly FrameFn queueFrame;
    private readonly NativeFrameFn queueNativeFrame;
    private readonly CancelFn cancelTicket;
    private readonly JoinedFn pollJoined;
    private readonly ClockFn nativeNow;
    private readonly QuitFn quit;
    private readonly Dictionary<ulong, ulong> warmupMarkers = new Dictionary<ulong, ulong>();
    private ulong startedSession;
    private ulong fenceDelivered;
    private ulong fencePublished;
    private Command? pending;
    private Evidence routingEvidence;
    private ulong routingFrame;

    public IntPtr RenderEvent { get; }
    public long ClockFrequency { get; }
    public ulong PreviousSession { get; }

    public static int FindOriginalWindow(string libraryPath, out ulong unityWindow)
    {
        if (IntPtr.Size != 8 || Application.platform != RuntimePlatform.LinuxPlayer)
            throw new PlatformNotSupportedException("The original X11 window belongs to the 64-bit Linux player.");
        return NativeModule.Open(libraryPath).Bind<FindWindowFn>("smf_session_find_original_window")(out unityWindow);
    }

    public LinuxRendererApi(string libraryPath, ulong unityWindow)
    {
        if (IntPtr.Size != 8 || Application.platform != RuntimePlatform.LinuxPlayer || unityWindow == 0)
            throw new PlatformNotSupportedException("The Linux renderer needs the 64-bit GLX player and the Unity X11 window id.");
        AssertAbi();
        window = unityWindow;

        string path = Path.GetFullPath(libraryPath);
        if (!File.Exists(path)) throw new FileNotFoundException("Renderer library is missing.", path);

        if (!Modules.TryGetValue(path, out NativeModule module))
        {
            module = NativeModule.Open(path);
            Modules.Add(path, module);
        }

        startSession = module.Bind<StartFn>("smf_session_start");
        sendCommand = module.Bind<CommandFn>("smf_session_command");
        publishFence = module.Bind<FenceFn>("smf_session_content_fence");
        readAck = module.Bind<AckFn>("smf_session_ack");
        queryStatus = module.Bind<StatusFn>("smf_session_status");
        queuePreGui = module.Bind<PreGuiFn>("smf_session_pre_gui");
        queryPlatformStatus = module.Bind<PlatformStatusFn>("smf_session_platform_status");
        queueFrame = module.Bind<FrameFn>("smf_session_frame");
        queueNativeFrame = module.Bind<NativeFrameFn>("smf_session_native_frame");
        cancelTicket = module.Bind<CancelFn>("smf_session_cancel");
        RenderEvent = module.Bind<EventFn>("smf_session_render_event")();
        pollJoined = module.Bind<JoinedFn>("smf_session_poll_joined");

        // The module outlives managed reloads, so the next session id must go past the one it last saw.
        var previous = new StatusPacket { Size = 472, Version = 1 };
        int previousResult = queryStatus(ref previous, 472);
        if (previousResult != 0)
            throw new InvalidOperationException("Native session identity is busy or unavailable; retry installation after owner retirement.");
        if (previous.Session != 0 && (previous.Size != 472 || previous.Version != 1))
            throw new InvalidOperationException("Native session identity ABI mismatch.");

        PreviousSession = previous.Session;

        nativeNow = module.Bind<ClockFn>("smf_session_clock_now");
        long frequency = module.Bind<ClockFn>("smf_session_clock_frequency")();
        if (frequency <= 0 || RenderEvent == IntPtr.Zero)
            throw new InvalidOperationException("Native callback/platform QPC is unavailable.");

        ClockFrequency = frequency;
        quit = module.Bind<QuitFn>("smf_session_quit");
    }

    private void CheckMain()
    {
        if (Thread.CurrentThread.ManagedThreadId != mainThread)
            throw new InvalidOperationException("Session ABI client belongs to Unity main.");
    }

    public long ClockNow()
    {
        CheckMain();
        return nativeNow();
    }

    public void FenceProcessExit()
    {
        CheckMain();
        if (quit() != 0) throw new InvalidOperationException("Linux native process-exit fence failed.");
    }

    public int Submit(Command command, SceneContext context)
    {
        CheckMain();
        if (command.Session != startedSession)
        {
            int started = startSession(window, command.Session);
            if (started != 0) return started;

            startedSession = command.Session;
            fenceDelivered = 0;
            fencePublished = 0;
            warmupMarkers.Clear();
        }

        if (command.Operation == Operation.PrepareHiddenGeneration || command.Operation == Operation.PrepareReplacementGeneration)
        {
            // The first fence may predate native start; publish it before the prepare is validated.
            if (command.ContentRevision > fencePublished)
            {
                int published = publishFence(command.Session, command.ContentRevision);
                if (published != 0) return published;
                fencePublished = command.ContentRevision;
            }
        }

        var packet = new CommandPacket
        {
            Size = 88,
            Version = 1,
            Session = command.Session,
            Serial = command.Serial,
            Generation = command.Generation,
            Content = command.ContentRevision,
            Previous = command.PreviousGeneration,
            AfterFrame = command.AfterFrame,
            Operation = (uint)command.Operation,
            Flags = command.HasWorld ? 1u : 0u,
            Width = context.Width,
            Height = context.Height,
            EmptyR = 0,
            EmptyG = 0,
            EmptyB = 0,
            EmptyA = 1, // reserved clear colour, still part of the ABI
        };

        int result = sendCommand(ref packet, 88);
        if (result == 0)
        {
            pending = command;
            routingEvidence = Evidence.None;
            routingFrame = 0;
        }

        return result;
    }

    public int PublishWorldFence(WorldFence fence)
    {
        CheckMain();
        if (fence.Session != startedSession) return 1;
        if (fence.ContentRevision <= fencePublished) return 0;

        int result = publishFence(fence.Session, fence.ContentRevision);
        if (result == 0) fencePublished = fence.ContentRevision;
        return result;
    }

    public int PollWorldFence(out WorldFence fence)
    {
        CheckMain();
        fence = default;
        if (startedSession == 0) return 1;

        int result = ReadStatus(out StatusPacket state);
        if (result != 0) return result;
        if (state.ContentAcknowledged == 0 || state.ContentAcknowledged <= fenceDelivered) return 1;

        fenceDelivered = state.ContentAcknowledged;
        fence = new WorldFence { Session = state.Session, ContentRevision = fenceDelivered };
        return 0;
    }

    public int PollAcknowledgement(out Acknowledgement acknowledgement)
    {
        CheckMain();
        acknowledgement = default;
        if (!pending.HasValue) return 1;

        Command wanted = pending.Value;
        if (wanted.Operation == Operation.StopWorker)
        {
            int joinResult = pollJoined(wanted.Session);
            if (joinResult != 0) return joinResult;
        }

        var packet = new AckPacket { Size = 80, Version = 1 };
        int result = readAck(wanted.Session, wanted.Serial, ref packet, 80);
        if (result != 0) return result;
        if (packet.Size != 80 || packet.Version != 1 || packet.Session != wanted.Session || packet.Serial != wanted.Serial ||
            packet.Generation != wanted.Generation || packet.Operation != (uint)wanted.Operation || packet.Content != wanted.ContentRevision)
            throw new InvalidOperationException("Native acknowledgment does not match the pending ticket.");
        if (packet.Disposition < 1 || packet.Disposition > 3) throw new InvalidOperationException("Unknown native acknowledgment disposition.");

        Evidence evidence = (Evidence)(packet.Evidence & 0x1FFFu);
        ulong sourceFrame = packet.SourceFrame;
        bool success = packet.Disposition == 1 && packet.Result == 0;

        if (success && wanted.Operation == Operation.RestoreNativeRouting)
        {
            // Bit 16 means native has fenced; the main-side routing report supplies the rest.
            if ((packet.Evidence & (1u << 16)) == 0 || routingEvidence == Evidence.None) return 1;
            evidence |= routingEvidence;
            // The render thread rebinds the original drawable after the main report, so keep the later frame.
            sourceFrame = Math.Max(routingFrame, packet.SourceFrame);
        }

        if (success && wanted.Operation == Operation.PrepareHiddenGeneration)
        {
            // Stay pending until native has actually shown the marker frame for this generation.
            int read = ReadStatus(out StatusPacket state);
            if (read != 0) return read;
            if (!warmupMarkers.TryGetValue(wanted.Generation, out ulong markerFrame) ||
                (state.Flags & 2u) == 0 ||
                state.NativeRevealFrame < sourceFrame ||
                state.NativeRevealFrame < markerFrame ||
                state.NativeSubmittedGeneration != wanted.Generation ||
                state.NativeSubmittedContent != wanted.ContentRevision ||
                state.NativeSubmittedRestoreSerial != 0)
                return 1;
            evidence |= Evidence.NativeFullUiMaintained;
        }

        if (success && (evidence & wanted.Required) != wanted.Required) return 1;

        acknowledgement = new Acknowledgement
        {
            Session = packet.Session,
            Serial = packet.Serial,
            Generation = packet.Generation,
            Operation = (Operation)packet.Operation,
            Evidence = evidence,
            Frame = sourceFrame,
            Success = success,
            Superseded = packet.Disposition == 2,
            Error = success ? null : "Native operation: disposition " + packet.Disposition + ", HRESULT 0x" + packet.Result.ToString("X8"),
        };

        pending = null;
        return 0;
    }

    public int RoutingRestored(Command ticket, ulong frame, Evidence evidence)
    {
        CheckMain();
        if (!pending.HasValue || pending.Value.Session != ticket.Session || pending.Value.Serial != ticket.Serial ||
            pending.Value.Operation != Operation.RestoreNativeRouting || frame == 0 ||
            (evidence & ticket.Required) != ticket.Required)
            throw new InvalidOperationException("Main routing report does not match the accepted cancellation fence.");

        routingFrame = frame;
        routingEvidence = evidence;
        return 0;
    }

    public int QueuePreGui(ref FrameBundle bundle, out NativeDispatch dispatch)
    {
        CheckMain();
        var packet = new PreGuiPacket
        {
            Size = 64,
            Version = 1,
            Session = bundle.Key.Session,
            Content = bundle.Key.Content,
            Generation = bundle.Key.Generation,
            SourceFrame = bundle.Key.SourceFrame,
            Width = bundle.Width,
            Height = bundle.Height,
            Bootstrap = bundle.HudTexture,
            Flags = NativeFlags(bundle.Flags) & 1u,
        };

        int result = queuePreGui(ref packet, 64, out IntPtr ticket, out int token);
        dispatch = new NativeDispatch { Ticket = ticket, Token = token };
        return result;
    }

    public int QueueFrame(ref FrameBundle bundle, out NativeDispatch dispatch)
    {
        CheckMain();
        var packet = new FramePacket
        {
            Size = 416,
            Version = 3,
            Session = bundle.Key.Session,
            Content = bundle.Key.Content,
            Generation = bundle.Key.Generation,
            SourceFrame = bundle.Key.SourceFrame,
            World = bundle.WorldTexture,
            Hud = bundle.HudTexture,
            EofQpc = bundle.EofClock,
            Flags = NativeFlags(bundle.Flags),
            WorldDispatches = bundle.WorldDispatches,
            Pose = bundle.Pose,
            CoverageTexture = bundle.CoverageTexture,
            CoverageSerial = bundle.CoverageSerial,
            CoverageWidth = bundle.CoverageWidth,
            CoverageHeight = bundle.CoverageHeight,
            CoverageFlags = bundle.CoverageFlags,
            CoverageReserved = bundle.CoverageReserved,
            CoverageA = bundle.CoverageA,
            CoverageB = bundle.CoverageB,
            CoverageC = bundle.CoverageC,
            CoverageD = bundle.CoverageD,
            CoverageE = bundle.CoverageE,
            CoverageF = bundle.CoverageF,
        };

        int result = queueFrame(ref packet, 416, out IntPtr ticket, out int token);
        dispatch = new NativeDispatch { Ticket = ticket, Token = token };
        return result;
    }

    public int QueueNativeFrame(ref NativeFrameMarker marker, out NativeDispatch dispatch)
    {
        CheckMain();
        var packet = new NativeFramePacket
        {
            Size = 64,
            Version = 1,
            Session = marker.Session,
            RestoreSerial = marker.RestoreSerial,
            SourceFrame = marker.SourceFrame,
            Content = marker.Content,
            Generation = marker.Generation,
            Width = marker.Width,
            Height = marker.Height,
            Flags = marker.BeginOnly ? 1u : 0u,
        };

        int result = queueNativeFrame(ref packet, 64, out IntPtr ticket, out int token);
        dispatch = new NativeDispatch { Ticket = ticket, Token = token };
        if (result == 0 && !marker.BeginOnly && marker.RestoreSerial == 0 && marker.Generation != 0 && !warmupMarkers.ContainsKey(marker.Generation))
            warmupMarkers.Add(marker.Generation, marker.SourceFrame);

        return result;
    }

    public int Cancel(NativeDispatch dispatch)
    {
        CheckMain();
        return cancelTicket(dispatch.Ticket, dispatch.Token);
    }

    // Also read by reflection for failure diagnostics; keep the name.
    private int ReadStatus(out StatusPacket state)
    {
        state = new StatusPacket { Size = 472, Version = 1 };
        if (startedSession == 0) return 1;

        int result = queryStatus(ref state, 472);
        if (result == 0 && (state.Size != 472 || state.Version != 1 || state.Session != startedSession))
            throw new InvalidOperationException("Session status ABI/session mismatch.");
        return result;
    }

    public bool TryReadPresentation(out PresentationSample sample)
    {
        CheckMain();
        sample = default;

        int read = ReadStatus(out StatusPacket state);
        if (read < 0) throw new InvalidOperationException("Linux presentation status failed: " + read);
        if (read != 0 || state.Result < 0 || state.ActiveGeneration == 0) return false;

        sample = new PresentationSample(state.Session, state.ActiveGeneration, state.WorkerCommit,
            ClockNow(), ClockFrequency, PresentationMetric.CompletedSwaps);
        return true;
    }

    public int ReadPlatformStatus(out PlatformPacket packet)
    {
        CheckMain();
        packet = new PlatformPacket { Size = 528, Version = 1 };

        int result = queryPlatformStatus(ref packet, 528);
        if (result == 0 && (packet.Size != 528 || packet.Version != 1))
            throw new InvalidOperationException("Linux platform diagnostics ABI mismatch.");
        return result;
    }

    private static uint NativeFlags(FrameFlags flags)
    {
        uint native = (flags & FrameFlags.HasMap) != 0 ? 1u : 0u;
        if ((flags & FrameFlags.WorldDispatchCompleted) != 0) native |= 2u;
        if ((flags & FrameFlags.WorldDispatchAbsent) != 0) native |= 4u;
        if ((flags & FrameFlags.FlipY) != 0) native |= 8u | 16u;
        return native;
    }

    // Sizes and offsets are the contract with SessionBridge.h.
    public static void AssertAbi()
    {
        RequireSize<CameraPose>(264);
        RequireSize<CommandPacket>(88);
        RequireSize<PreGuiPacket>(64);
        RequireSize<FramePacket>(416);
        RequireSize<NativeFramePacket>(64);
        RequireSize<AckPacket>(80);
        RequireSize<GenerationStatus>(128);
        RequireSize<StatusPacket>(472);
        RequireSize<PlatformPacket>(528);

        if ((int)Marshal.OffsetOf(typeof(CameraPose), nameof(CameraPose.RootX)) != 232 ||
            (int)Marshal.OffsetOf(typeof(FramePacket), nameof(FramePacket.Pose)) != 72 ||
            (int)Marshal.OffsetOf(typeof(FramePacket), nameof(FramePacket.CoverageTexture)) != 336 ||
            (int)Marshal.OffsetOf(typeof(FramePacket), nameof(FramePacket.CoverageSerial)) != 344 ||
            (int)Marshal.OffsetOf(typeof(FramePacket), nameof(FramePacket.CoverageA)) != 368)
            throw new InvalidOperationException("Session ABI field offsets differ from SessionBridge.h.");
    }

    private static void RequireSize<T>(int expected)
    {
        if (Marshal.SizeOf(typeof(T)) != expected) throw new InvalidOperationException(typeof(T).Name + " ABI size mismatch.");
    }

    [StructLayout(LayoutKind.Sequential, Pack = 8)]
    private struct CommandPacket
    {
        public uint Size;
        public uint Version;
        public ulong Session;
        public ulong Serial;
        public ulong Generation;
        public ulong Content;
        public ulong Previous;
        public ulong AfterFrame;
        public uint Operation;
        public uint Flags;
        public uint Width;
        public uint Height;
        public float EmptyR;
        public float EmptyG;
        public float EmptyB;
        public float EmptyA;
    }

    [StructLayout(LayoutKind.Sequential, Pack = 8)]
    private struct PreGuiPacket
    {
        public uint Size;
        public uint Version;
        public ulong Session;
        public ulong Content;
        public ulong Generation;
        public ulong SourceFrame;
        public uint Width;
        public uint Height;
        public ulong Bootstrap;
        public uint Flags;
        public uint Reserved;
    }

    [StructLayout(LayoutKind.Sequential, Pack = 8)]
    private struct FramePacket
    {
        public uint Size;
        public uint Version;
        public ulong Session;
        public ulong Content;
        public ulong Generation;
        public ulong SourceFrame;
        public ulong World;
        public ulong Hud;
        public long EofQpc;
        public uint Flags;
        public uint WorldDispatches;
        public CameraPose Pose;
        public ulong CoverageTexture;
        public ulong CoverageSerial;
        public uint CoverageWidth;
        public uint CoverageHeight;
        public uint CoverageFlags;
        public uint CoverageReserved;
        public double CoverageA;
        public double CoverageB;
        public double CoverageC;
        public double CoverageD;
        public double CoverageE;
        public double CoverageF;
    }

    [StructLayout(LayoutKind.Sequential, Pack = 8)]
    private struct NativeFramePacket
    {
        public uint Size;
        public uint Version;
        public ulong Session;
        public ulong RestoreSerial;
        public ulong SourceFrame;
        public ulong Content;
        public ulong Generation;
        public uint Width;
        public uint Height;
        public uint Flags;
        public uint Reserved;
    }

    [StructLayout(LayoutKind.Sequential, Pack = 8)]
    private struct AckPacket
    {
        public uint Size;
        public uint Version;
        public ulong Session;
        public ulong Serial;
        public ulong Generation;
        public ulong Content;
        public ulong SourceFrame;
        public uint Operation;
        public uint Evidence;
        public int Result;
        public uint Disposition;
        public ulong CommitSerial;
        public long CompletedQpc;
    }

    [StructLayout(LayoutKind.Sequential, Pack = 8)]
    private struct GenerationStatus
    {
        public ulong Generation;
        public ulong Content;
        public ulong LastFrame;
        public ulong PreparedFrame;
        public ulong Frames;
        public ulong SourceCommit;
        public ulong CompletedCommit;
        public ulong Attachment;
        public ulong AttachmentCompleted;
        public ulong RetiredSerial;
        public uint State;
        public uint Width;
        public uint Height;
        public uint Flags;
        public ulong RetireRequested;
        public ulong StagedFrame;
        public uint BaseFormat;
        public uint WorldFormat;
        public uint HudFormat;
        public uint Reserved;
    }

    [StructLayout(LayoutKind.Sequential, Pack = 8)]
    private struct StatusPacket
    {
        public uint Size;
        public uint Version;
        public ulong Session;
        public ulong ContentFence;
        public ulong ContentAcknowledged;
        public ulong OperationFence;
        public ulong ActiveGeneration;
        public ulong ActiveFrame;
        public ulong LastAckSerial;
        public uint MainThread;
        public uint RenderThread;
        public uint WorkerThread;
        public uint CompletionThread;
        public uint WorkerState;
        public uint Stage;
        public int Result;
        public uint Flags;
        public ulong SourceCommit;
        public ulong SourceCompleted;
        public ulong WorkerCommit;
        public ulong WorkerCompleted;
        public ulong DroppedFrames;
        public ulong QueuedCallbacks;
        public GenerationStatus Generation0;
        public GenerationStatus Generation1;
        public ulong NativeOrderedFrame;
        public ulong NativeOrderedRestoreSerial;
        public ulong NativeSubmittedFrame;
        public ulong NativeRevealFrame;
        public ulong NativeSubmittedGeneration;
        public ulong NativeSubmittedContent;
        public ulong NativeSubmittedRestoreSerial;
        public ulong NativePresentSerial;
        public ulong NativeBackbuffer;
    }

    [StructLayout(LayoutKind.Sequential, Pack = 8, CharSet = CharSet.Ansi)]
    public struct PlatformPacket
    {
        public uint Size;
        public uint Version;
        public ulong Publication;
        public ulong PublishedNs;
        public ulong Original;
        public ulong SourceDrawable;
        public ulong HiddenDrawable;
        public ulong SourceContext;
        public ulong WorkerContext;
        public uint Phase;
        public uint SourceThread;
        public uint WorkerThread;
        public uint Flags;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string SourceRenderer;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string WorkerRenderer;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 64)] public string WorkerVendor;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string WorkerVersion;
    }

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate int FindWindowFn(out ulong unityWindow);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate int StartFn(ulong window, ulong session);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate int CommandFn(ref CommandPacket packet, uint bytes);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate int FenceFn(ulong session, ulong revision);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate int AckFn(ulong session, ulong serial, ref AckPacket packet, uint bytes);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate int StatusFn(ref StatusPacket packet, uint bytes);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate int PlatformStatusFn(ref PlatformPacket packet, uint bytes);

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
}
