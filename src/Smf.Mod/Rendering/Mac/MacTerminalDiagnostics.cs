#nullable disable
using System;
using System.Runtime.InteropServices;
using System.Text;

namespace SimplyMoreFPS.Rendering;

public sealed partial class MacRendererApi
{
    // Read only after a failure. Both exports are optional and read-only;
    // neither consumes the native fault.
    private string ReadTerminalFailureDiagnostics()
    {
        RequireMainThread();
        ulong session = startedSession;
        if (session == 0) return "terminal=unavailable-no-started-session\nsourceRejection=unavailable-no-started-session";
        return ReadTerminalStatusDiagnostics(session) + "\n" + ReadSourceRejectionDiagnostics(session);
    }

    private string ReadTerminalStatusDiagnostics(ulong session)
    {
        const string label = "terminal";
        try
        {
            AssertDiagnosticSize<TerminalDiagnosticPacket>(192);
            AssertDiagnosticOffset<TerminalDiagnosticPacket>(nameof(TerminalDiagnosticPacket.SourceEpochs), 56);
            AssertDiagnosticOffset<TerminalDiagnosticPacket>(nameof(TerminalDiagnosticPacket.SourceFailures), 168);

            var read = module.Bind<TerminalDiagnosticFn>("smf_mac_terminal_status");
            var value = new TerminalDiagnosticPacket { Size = 192, Version = 1 };
            int result = read(session, ref value, 192);

            if (result != 0) return label + "=unavailable requestedSession=" + session + " result=" + result;
            if (value.Size != 192 || value.Version != 1 || value.Session != session)
                return label + "=invalid-ABI-or-session requestedSession=" + session + " " + FormatFields(value);

            string fault = Enum.IsDefined(typeof(TerminalFault), value.Fault) ? ((TerminalFault)value.Fault).ToString() : "Unknown";
            return label + " result=0 FaultName=" + fault + " " + FormatFields(value);
        }
        catch (EntryPointNotFoundException)
        {
            return label + "=unavailable-in-native-build";
        }
        catch (Exception error)
        {
            return label + "=read-failed requestedSession=" + session + "\n" + RendererDiagnostics.FormatException(error);
        }
    }

    private string ReadSourceRejectionDiagnostics(ulong session)
    {
        const string label = "sourceRejection";
        try
        {
            AssertDiagnosticSize<SourceRejectionDiagnosticPacket>(400);
            AssertDiagnosticSize<DiagnosticSourceTarget>(64);
            AssertDiagnosticOffset<SourceRejectionDiagnosticPacket>(nameof(SourceRejectionDiagnosticPacket.Target), 40);
            AssertDiagnosticOffset<SourceRejectionDiagnosticPacket>(nameof(SourceRejectionDiagnosticPacket.Device), 112);
            AssertDiagnosticOffset<SourceRejectionDiagnosticPacket>(nameof(SourceRejectionDiagnosticPacket.CommandStatus), 224);
            AssertDiagnosticOffset<SourceRejectionDiagnosticPacket>(nameof(SourceRejectionDiagnosticPacket.PropertiesRead), 368);

            var read = module.Bind<SourceRejectionDiagnosticFn>("smf_mac_terminal_source_rejection");
            var value = new SourceRejectionDiagnosticPacket { Size = 400, Version = 1 };
            int result = read(session, ref value, 400);

            if (result != 0) return label + "=unavailable requestedSession=" + session + " result=" + result;
            if (value.Size != 400 || value.Version != 1 || value.Target.Size != 64
                || value.Target.Version != 1 || value.Target.Session != session)
                return label + "=invalid-ABI-or-session requestedSession=" + session + " " + FormatFields(value);

            string reason = Enum.IsDefined(typeof(SourceReject), value.Reason) ? ((SourceReject)value.Reason).ToString() : "Unknown";
            return label + " result=0 ReasonName=" + reason + " " + FormatFields(value);
        }
        catch (EntryPointNotFoundException)
        {
            return label + "=unavailable-in-native-build";
        }
        catch (Exception error)
        {
            return label + "=read-failed requestedSession=" + session + "\n" + RendererDiagnostics.FormatException(error);
        }
    }

    private static void AssertDiagnosticSize<T>(int bytes)
    {
        if (Marshal.SizeOf(typeof(T)) != bytes)
            throw new InvalidOperationException(typeof(T).Name + " diagnostic size must be " + bytes + ".");
    }

    private static void AssertDiagnosticOffset<T>(string field, int bytes)
    {
        if (Marshal.OffsetOf(typeof(T), field).ToInt64() != bytes)
            throw new InvalidOperationException(typeof(T).Name + "." + field + " diagnostic offset must be " + bytes + ".");
    }

    private static string FormatFields<T>(T value)
    {
        var text = new StringBuilder();
        foreach (var field in typeof(T).GetFields())
        {
            object scalar = field.GetValue(value);
            if (scalar is DiagnosticSourceTarget target)
            {
                foreach (var targetField in typeof(DiagnosticSourceTarget).GetFields())
                {
                    text.Append("Target.").Append(targetField.Name).Append('=').Append(targetField.GetValue(target)).Append(' ');
                }
            }
            else
            {
                text.Append(field.Name).Append('=').Append(scalar).Append(' ');
            }
        }

        return text.ToString().TrimEnd();
    }

    // Mirrors the native terminal status packet. Pointer-sized values are
    // identities for logging only; never dereference them.
    [StructLayout(LayoutKind.Sequential, Pack = 8)]
    private struct TerminalDiagnosticPacket
    {
        public uint Size, Version;
        public ulong Session, Queue, Layer;
        public uint Fault, Stopping, Buffers, Drawables, Hooks, InFlight;
        public ulong SourceEpochs, Copies, PresentCalls, PresentedPositive, PresentedZero;
        public ulong GpuCompleted, Witnesses, Rejected, StaleCallbacks, PoolPressure;
        public ulong UnknownRoutes, ClassConflicts, QueueConflicts, SourceWrites;
        public ulong SourceFailures, ClassRestoreConflicts, SourceThread;
    }

    [StructLayout(LayoutKind.Sequential, Pack = 8)]
    private struct DiagnosticSourceTarget
    {
        public uint Size, Version;
        public ulong Session, ContentRevision, Generation, SourceFrame, NativeRenderBuffer;
        public uint Width, Height, Flags, Reserved;
    }

    // Mirrors the native source rejection packet; every observed scalar is kept.
    [StructLayout(LayoutKind.Sequential, Pack = 8)]
    private struct SourceRejectionDiagnosticPacket
    {
        public uint Size, Version, Reason, Stage;
        public ulong FailureOrdinal, ObservedNs, RenderThread;
        public DiagnosticSourceTarget Target;
        public ulong RestoreSerial;
        public ulong Device, Command, CommandAfter, Queue, CommandDevice, QueueDevice;
        public ulong SourceDevice, OriginalDevice, BeforePass, AfterPass, BeforeTexture, AfterTexture, SourceTexture, OriginalLayer;
        public ulong CommandStatus, SourceFormat, OriginalFormat, SourceWidth, SourceHeight, SourceType;
        public ulong SampleCount, ArrayLength, MipmapLevels, StorageMode, FramebufferOnly;
        public ulong BeforeLevel, BeforeSlice, BeforeDepth, AfterLevel, AfterSlice, AfterDepth, Reserved;
        public ulong PropertiesRead, CommandAfterEnd, QueueAfterEnd, StatusAfterEnd;
    }

    private enum TerminalFault : uint
    {
        None,
        Pool,
        UnknownRoute,
        ClassConflict,
        QueueConflict,
        Identity
    }

    private enum SourceReject : uint
    {
        None = 0,
        NoDevice,
        NoCommand,
        CommandDevice,
        NoQueue,
        QueueDevice,
        SourceDevice,
        ChangedCommand,
        CommandAlreadySubmitted,
        BeforeAttachment,
        AfterAttachment,
        AttachmentLevel,
        AttachmentSlice,
        AttachmentDepth,
        NoSource,
        TextureType,
        SampleCount,
        ArrayLength,
        FramebufferOnly,
        Width,
        Height,
        PixelFormat,
        OriginalDevice,
        QueueHook,
        SourceLease,
        ChangedCommandAfterEnd,
        QueueAfterEnd,
        SubmittedAfterEnd,
        Exception,
        PassPresence,
        BeforeLevel,
        BeforeSlice,
        BeforeDepth,
        TrackBuffer,
        TextureHook,
        MissingGpuLease,
        SourceArrayFull
    }

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate int TerminalDiagnosticFn(ulong session, ref TerminalDiagnosticPacket value, uint bytes);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate int SourceRejectionDiagnosticFn(ulong session, ref SourceRejectionDiagnosticPacket value, uint bytes);
}
