using System;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Threading;
using SimplyMoreFPS;

namespace Smf.Camera;

// One camera per module, driven from the renderer's worker thread. The session id is a
// plain counter rather than a pointer, and no callback ever enters Unity.
public static unsafe class CameraKernel
{
    private enum Operation
    {
        Create,
        Adopt,
        Configure,
        Step,
        Release
    }

    private sealed class Session
    {
        internal readonly ulong Id;
        internal readonly int OwnerThread;
        internal ulong Epoch;
        internal ulong PoseSequence = 1;
        internal ulong InputSequence;
        internal int MapId;
        internal double Clock;
        internal KernelSettings Settings;
        internal CameraMotion Motion;
        internal bool Faulted;
        internal KernelTrajectory LastTrajectory;

        internal Session(ulong id, KernelInit init, KernelSettings settings, CameraMotion motion)
        {
            Id = id;
            OwnerThread = Environment.CurrentManagedThreadId;
            Epoch = init.Epoch;
            MapId = init.MapId;
            Clock = init.MonotonicSeconds;
            Settings = settings;
            Motion = motion;
        }

        internal KernelPose Snapshot(KernelResult result)
        {
            CameraPose pose = Motion.Pose;
            return new KernelPose
            {
                Version = Contract.Version,
                Size = KernelPose.Bytes,
                Result = (int)result,
                Session = Id,
                Epoch = Epoch,
                PoseSequence = PoseSequence,
                InputSequence = InputSequence,
                SettingsRevision = Settings.Revision,
                MapId = MapId,
                X = pose.X,
                Z = pose.Z,
                RootSize = pose.Size,
                DesiredSize = Motion.DesiredSize,
                VelocityX = Motion.VelocityX,
                VelocityZ = Motion.VelocityZ,
                ProjectionHalfHeight = Motion.ProjectionHalfHeight,
                ActivePanId = (ulong)Motion.ActivePanId,
                FinishedPanId = (ulong)Motion.LastFinishedPanId,
                PanFlags = Motion.LastPanCompleted ? 1u : 0u
            };
        }
    }

    private static Session? active;
    private static ulong nextSessionId;
    private static int callGate;

#if SMF_APPLE_ARM64_ABI
    private const string ExportPrefix = "smf_managed_camera_";
#else
    private const string ExportPrefix = "smf_camera_";
#endif

    [UnmanagedCallersOnly(EntryPoint = ExportPrefix + "create", CallConvs = new[] { typeof(CallConvCdecl) })]
    public static int Create(KernelInit* init, uint initBytes, KernelSettings* settings, uint settingsBytes, KernelPose* output, uint outputBytes)
        => Execute(Operation.Create, 0, 0, init, initBytes, settings, settingsBytes, null, 0, output, outputBytes);

    [UnmanagedCallersOnly(EntryPoint = ExportPrefix + "adopt", CallConvs = new[] { typeof(CallConvCdecl) })]
    public static int Adopt(ulong session, KernelInit* init, uint initBytes, KernelSettings* settings, uint settingsBytes, KernelPose* output, uint outputBytes)
        => Execute(Operation.Adopt, session, 0, init, initBytes, settings, settingsBytes, null, 0, output, outputBytes);

    [UnmanagedCallersOnly(EntryPoint = ExportPrefix + "configure", CallConvs = new[] { typeof(CallConvCdecl) })]
    public static int Configure(ulong session, ulong epoch, KernelSettings* settings, uint settingsBytes, KernelPose* output, uint outputBytes)
        => Execute(Operation.Configure, session, epoch, null, 0, settings, settingsBytes, null, 0, output, outputBytes);

    [UnmanagedCallersOnly(EntryPoint = ExportPrefix + "step", CallConvs = new[] { typeof(CallConvCdecl) })]
    public static int Step(ulong session, KernelInput* input, uint inputBytes, KernelPose* output, uint outputBytes)
        => Execute(Operation.Step, session, 0, null, 0, null, 0, input, inputBytes, output, outputBytes);

    [UnmanagedCallersOnly(EntryPoint = ExportPrefix + "release", CallConvs = new[] { typeof(CallConvCdecl) })]
    public static int Release(ulong session, KernelPose* output, uint outputBytes)
        => Execute(Operation.Release, session, 0, null, 0, null, 0, null, 0, output, outputBytes);

    private static int Execute(Operation operation, ulong session, ulong epoch,
        KernelInit* init, uint initBytes, KernelSettings* settings, uint settingsBytes,
        KernelInput* input, uint inputBytes, KernelPose* output, uint outputBytes)
    {
        // The caller owns pointer validity; nothing here can make a bad native address safe.
        if (output == null || outputBytes != KernelPose.Bytes) return (int)KernelResult.InvalidArgument;

        bool entered = false;
        try
        {
            *output = ErrorPose(KernelResult.InternalError);
            if (Interlocked.CompareExchange(ref callGate, 1, 0) != 0)
            {
                *output = ErrorPose(KernelResult.Busy);
                return (int)KernelResult.Busy;
            }

            entered = true;

            KernelResult result = Dispatch(operation, session, epoch, init, initBytes, settings, settingsBytes, input, inputBytes);
            *output = (int)result >= 0 && active != null ? active.Snapshot(result) : ErrorPose(result);
            return (int)result;
        }
        catch (Exception)
        {
            *output = ErrorPose(KernelResult.InternalError);
            return (int)KernelResult.InternalError;
        }
        finally
        {
            if (entered) Volatile.Write(ref callGate, 0);
        }
    }

    private static KernelPose ErrorPose(KernelResult result) => new KernelPose
    {
        Version = Contract.Version,
        Size = KernelPose.Bytes,
        MapId = -1,
        Result = (int)result
    };

    private static KernelResult Dispatch(Operation operation, ulong session, ulong epoch,
        KernelInit* initPtr, uint initBytes, KernelSettings* settingsPtr, uint settingsBytes,
        KernelInput* inputPtr, uint inputBytes)
    {
        Session? s = active;

        if (operation == Operation.Create)
        {
            if (s != null) return KernelResult.Busy;
            return CreateSession(initPtr, initBytes, settingsPtr, settingsBytes);
        }

        if (s == null || session == 0 || session != s.Id) return KernelResult.BadSession;
        if (s.OwnerThread != Environment.CurrentManagedThreadId) return KernelResult.WrongThread;

        if (operation == Operation.Release)
        {
            // Stop accepting calls before the native worker exits; the AOT module itself is never unloaded.
            active = null;
            return KernelResult.Ok;
        }

        if (operation == Operation.Adopt) return AdoptEpoch(s, initPtr, initBytes, settingsPtr, settingsBytes);
        if (s.Faulted) return KernelResult.Faulted;
        if (operation == Operation.Configure) return ApplySettings(s, epoch, settingsPtr, settingsBytes);
        if (inputPtr == null || inputBytes != KernelInput.Bytes) return KernelResult.InvalidArgument;
        return StepMotion(s, *inputPtr);
    }

    private static KernelResult CreateSession(KernelInit* initPtr, uint initBytes, KernelSettings* settingsPtr, uint settingsBytes)
    {
        if (!ReadPackets(initPtr, initBytes, settingsPtr, settingsBytes, out KernelInit init, out KernelSettings settings))
            return KernelResult.InvalidArgument;
        AssertLayout();
        if (!NewMotion(init, settings, out CameraMotion motion)) return KernelResult.InvalidArgument;

        ulong id = checked(nextSessionId + 1);
        Session created = new Session(id, init, settings, motion);
        nextSessionId = id;
        active = created;
        return KernelResult.Ok;
    }

    private static KernelResult AdoptEpoch(Session s, KernelInit* initPtr, uint initBytes, KernelSettings* settingsPtr, uint settingsBytes)
    {
        if (!ReadPackets(initPtr, initBytes, settingsPtr, settingsBytes, out KernelInit init, out KernelSettings settings))
            return KernelResult.InvalidArgument;
        if (init.Epoch <= s.Epoch) return KernelResult.WrongEpoch;
        if (!NewMotion(init, settings, out CameraMotion fresh)) return KernelResult.InvalidArgument;

        // Nothing is committed until the whole new epoch has validated.
        ulong sequence = checked(s.PoseSequence + 1);
        s.Motion = fresh;
        s.Settings = settings;
        s.Epoch = init.Epoch;
        s.MapId = init.MapId;
        s.Clock = init.MonotonicSeconds;
        s.InputSequence = 0;
        s.PoseSequence = sequence;
        s.Faulted = false;
        s.LastTrajectory = default;
        return KernelResult.Ok;
    }

    private static KernelResult ApplySettings(Session s, ulong epoch, KernelSettings* settingsPtr, uint settingsBytes)
    {
        if (epoch != s.Epoch) return KernelResult.WrongEpoch;
        if (settingsPtr == null || settingsBytes != KernelSettings.Bytes) return KernelResult.InvalidArgument;

        KernelSettings packet = *settingsPtr;
        if (!Contract.Valid(packet)) return KernelResult.InvalidArgument;
        if (packet.Revision <= s.Settings.Revision) return KernelResult.Stale;
        if (!Contract.TrySettings(packet, s.Motion.Pose, s.Motion.DesiredSize, out CameraSettings values))
            return KernelResult.InvalidArgument;

        // Settings changes keep the pose, clock and inertia as they are.
        s.Motion.Settings = values;
        s.Settings = packet;
        return KernelResult.Ok;
    }

    private static KernelResult StepMotion(Session s, KernelInput input)
    {
        if (!Contract.Valid(input)) return KernelResult.InvalidArgument;
        if (input.Epoch != s.Epoch) return KernelResult.WrongEpoch;
        if (input.SettingsRevision != s.Settings.Revision) return KernelResult.InvalidArgument;
        // A repeated sequence must not re-apply its wheel, key or drag pulse.
        if (input.Sequence <= s.InputSequence) return KernelResult.Stale;
        if (input.MonotonicSeconds < s.Clock) return KernelResult.InvalidArgument;

        double dt = input.MonotonicSeconds - s.Clock;
        if (!Contract.Finite(dt)) return KernelResult.InvalidArgument;

        // A clock-only step consumes elapsed time without moving, so an adopted seed pose
        // survives exactly even where ordinary motion would constrain it.
        if ((input.Flags & (uint)InputFlags.ClockOnly) != 0)
        {
            s.InputSequence = input.Sequence;
            s.Clock = input.MonotonicSeconds;
            return KernelResult.Ok;
        }

        CameraPose before = s.Motion.Pose;
        long activePanBefore = s.Motion.ActivePanId;
        long finishedPanBefore = s.Motion.LastFinishedPanId;
        bool panCompletedBefore = s.Motion.LastPanCompleted;

        KernelTrajectory command = input.Trajectory;
        bool newCommand = command.Id > s.LastTrajectory.Id;

        if (command.Id != 0)
        {
            if (command.Id < s.LastTrajectory.Id) return KernelResult.Stale;
            if (!newCommand && !Contract.Same(command, s.LastTrajectory)) return KernelResult.InvalidArgument;
            if (newCommand)
            {
                try
                {
                    Contract.ValidateProfile(s.Motion.Settings, command.SourceRootSize, command.SourceRootSize);
                    Contract.ValidateProfile(s.Motion.Settings, command.TargetRootSize, command.TargetRootSize);
                }
                catch (ArgumentException)
                {
                    return KernelResult.InvalidArgument;
                }
                catch (ArithmeticException)
                {
                    return KernelResult.InvalidArgument;
                }
            }
        }

        bool scriptedPan = newCommand || (command.Id != 0 && s.Motion.ActivePanId != 0);
        if (!scriptedPan && !Contract.FiniteDrag(input, s.Settings, s.Motion.ProjectionHalfHeight))
            return KernelResult.InvalidArgument;

        try
        {
            if (command.Id == 0)
            {
                s.Motion.CancelPan();
            }
            else if (newCommand)
            {
                s.Motion.Reset(new CameraPose(command.SourceX, command.SourceZ, command.SourceRootSize));
                s.Motion.PanTo((long)command.Id, new CameraPose(command.TargetX, command.TargetZ, command.TargetRootSize), command.DurationSeconds);
                s.LastTrajectory = command;
                // The first delivery catches up from the command's absolute start; later ones use the plain clock delta.
                dt = Math.Min(command.DurationSeconds, input.MonotonicSeconds - command.StartSeconds);
            }

            CameraInput controls = Contract.ToInput(input, s.Settings);
            bool jumpToTarget = scriptedPan && !controls.MotionBlocked &&
                (controls.PanX != 0 || controls.PanZ != 0 || controls.DragX != 0 || controls.DragY != 0);

            // Vanilla finishes a pan at its exact target when keys or a middle drag interrupt it;
            // wheel zoom and edge scrolling do not.
            if (jumpToTarget) s.Motion.CompletePan();
            if (scriptedPan) controls.MotionBlocked = true;
            // A zero-duration pan already sits at its exact target, which may be outside the zoom range.
            if (!jumpToTarget && !(newCommand && command.DurationSeconds == 0))
                s.Motion.Update(controls, dt, s.Settings.MapWidth, s.Settings.MapHeight);

            if (!Contract.FiniteState(s.Motion))
            {
                s.Faulted = true;
                return KernelResult.Faulted;
            }

            CameraPose after = s.Motion.Pose;
            bool changed = after.X != before.X || after.Z != before.Z || after.Size != before.Size ||
                s.Motion.ActivePanId != activePanBefore || s.Motion.LastFinishedPanId != finishedPanBefore ||
                s.Motion.LastPanCompleted != panCompletedBefore;
            if (changed) s.PoseSequence = checked(s.PoseSequence + 1);

            s.InputSequence = input.Sequence;
            s.Clock = input.MonotonicSeconds;
            return KernelResult.Ok;
        }
        catch
        {
            // Update may have half-mutated the motion; refuse further steps until a newer epoch is adopted.
            s.Faulted = true;
            throw;
        }
    }

    private static bool ReadPackets(KernelInit* initPtr, uint initBytes, KernelSettings* settingsPtr, uint settingsBytes,
        out KernelInit init, out KernelSettings settings)
    {
        init = default;
        settings = default;
        if (initPtr == null || initBytes != KernelInit.Bytes || settingsPtr == null || settingsBytes != KernelSettings.Bytes)
            return false;

        init = *initPtr;
        settings = *settingsPtr;
        return Contract.Valid(init) && Contract.Valid(settings);
    }

    private static bool NewMotion(KernelInit init, KernelSettings settings, out CameraMotion motion)
    {
        motion = null!;
        CameraPose pose = new CameraPose(init.X, init.Z, init.RootSize);
        if (!Contract.TrySettings(settings, pose, pose.Size, out CameraSettings values)) return false;

        motion = new CameraMotion(values, pose);
        return true;
    }

    // The C# structs must match camera_kernel.h byte for byte.
    private static void AssertLayout()
    {
        bool matches = sizeof(KernelInit) == KernelInit.Bytes &&
            sizeof(KernelSettings) == KernelSettings.Bytes &&
            sizeof(KernelInput) == KernelInput.Bytes &&
            sizeof(KernelPose) == KernelPose.Bytes &&
            sizeof(KernelCurve) == KernelCurve.Bytes &&
            sizeof(KernelProfilePack) == KernelProfilePack.Bytes &&
            sizeof(KernelTrajectory) == KernelTrajectory.Bytes &&
            sizeof(KernelAxisBounds) == KernelAxisBounds.Bytes &&
            sizeof(KernelMovementBounds) == KernelMovementBounds.Bytes;
        if (!matches) throw new InvalidOperationException("Native camera struct size mismatch.");
    }
}
