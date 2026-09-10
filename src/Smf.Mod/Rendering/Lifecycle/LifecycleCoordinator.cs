#nullable disable
using System;
using System.Threading;

namespace SimplyMoreFPS.Rendering.Lifecycle;

public enum Phase
{
    Off,
    Preparing,
    Activating,
    Active,
    InvalidatingWorld,
    Rebuilding,
    RetiringUnusedGpu,
    ReleasingUnusedMain,
    RetiringPreviousGpu,
    ReleasingPreviousMain,
    RestoringNative,
    AwaitingNativeFrame,
    Detaching,
    RetiringGpu,
    ReleasingMain,
    StoppingWorker,
    Exited
}

public enum Operation
{
    PrepareHiddenGeneration,
    InvalidateWorld,
    PrepareReplacementGeneration,
    ActivateGeneration,
    RetireGenerationGpu,
    ReleaseGenerationMain,
    RestoreNativeRouting,
    AwaitNativeFrame,
    DetachComposite,
    RetireSessionGpu,
    ReleaseSessionMain,
    StopWorker
}

[Flags]
public enum Evidence : uint
{
    None = 0,
    NativeFullUiMaintained = 1,
    CompositeComplete = 2,
    CompositeActivated = 4,
    OldWorldInvalidated = 8,
    NativeRoutingRestored = 16,
    CaptureScopesClosed = 32,
    CameraReleased = 64,
    NativeFullFrameAvailable = 128,
    CompositeDetached = 256,
    RenderOwnersRetired = 512,
    MainResourcesReleased = 1024,
    WorkerJoined = 2048,
    NeverActivated = 4096
}

// Plain values only; the host owns the context snapshots and native handles.
public struct Command
{
    public ulong Session;
    public ulong Serial;
    public ulong Generation;
    public ulong ContentRevision;
    public ulong PreviousGeneration;
    public ulong AfterFrame;
    public Operation Operation;
    public bool HasWorld;
    public Evidence Required;
}

public struct Acknowledgement
{
    public ulong Session;
    public ulong Serial;
    public ulong Generation;
    public ulong Frame;
    public Operation Operation;
    public Evidence Evidence;
    public bool Success;
    public bool Superseded;
    public string Error; // main-only struct, not a C ABI packet
}

public struct WorldFence
{
    public ulong Session;
    public ulong ContentRevision;
}

// Main-thread state machine. The host delivers owner acknowledgements here;
// nothing in this class touches Unity, native code or another thread.
public sealed class LifecycleCoordinator
{
    private readonly int mainThreadId = Thread.CurrentThread.ManagedThreadId;
    private ulong serial;
    private ulong nextGeneration;
    private ulong wantedContent;
    private ulong activeContent;
    private ulong candidateContent;
    private ulong previousContent;
    private ulong activeGeneration;
    private ulong candidateGeneration;
    private ulong previousGeneration;
    private ulong nativeRoutingFrame;
    private ulong sentWorldFence;
    private ulong acknowledgedWorldFence;
    private bool wantedWorld;
    private bool activeWorld;
    private bool candidateWorld;
    private bool previousWorld;
    private bool userEnabled;
    private bool renderingAllowed = true;
    private bool quitting;
    private bool faultLatched;
    private Command? pending;
    private bool preparationSubmitted;
    private Acknowledgement? reply;

    public Phase State { get; private set; } = Phase.Off;

    public ulong Session
    {
        get; private set;
    }

    public int DrawDepth
    {
        get; private set;
    }

    public bool WaitingForOwnerRecovery
    {
        get; private set;
    }

    public bool ResourcesAbandonedToProcessExit
    {
        get; private set;
    }

    public long StaleAcknowledgements
    {
        get; private set;
    }

    public string LastFailure
    {
        get; private set;
    }

    public bool Faulted => faultLatched;

    public bool UserEnabled => userEnabled;
    public bool RenderingAllowed => renderingAllowed;

    public Command? Pending => pending;

    public LifecycleCoordinator(ulong previousSession = 0)
    {
        Session = previousSession;
    }

    private void AssertMainThread()
    {
        if (Thread.CurrentThread.ManagedThreadId != mainThreadId)
            throw new InvalidOperationException("Lifecycle state belongs to the Unity main thread.");
    }

    // Only flags change here, so this is safe to call from the settings window's draw.
    public void SetEnabled(bool enabled)
    {
        AssertMainThread();
        if (enabled != userEnabled && WaitingForOwnerRecovery)
            RetryAfterOwnerRecovery();

        // An explicit off/on cycle is the only way to retry after a fault.
        if (enabled && !userEnabled)
        {
            faultLatched = false;
            LastFailure = null;
        }

        userEnabled = enabled;
    }

    // Temporary camera ownership changes must not reset a renderer fault.
    public void SetRenderingAllowed(bool allowed)
    {
        AssertMainThread();
        renderingAllowed = allowed;
    }

    public void ChangeContent(ulong revision, bool hasWorld)
    {
        AssertMainThread();
        if (revision == 0 || revision < wantedContent || (revision == wantedContent && hasWorld != wantedWorld))
            throw new ArgumentException("Context revisions must be monotonic and immutable.");

        wantedContent = revision;
        wantedWorld = hasWorld;
    }

    // Safe to publish during a draw: it only freezes the displayed bundle at its
    // current pose while a replacement prepares, and never touches a target.
    public WorldFence? TakeWorldFence()
    {
        AssertMainThread();
        if (Session == 0 || State == Phase.Off || State == Phase.Exited || IsStopping() || sentWorldFence == wantedContent)
            return null;

        sentWorldFence = wantedContent;
        return new WorldFence { Session = Session, ContentRevision = wantedContent };
    }

    public void AcknowledgeWorldFence(WorldFence value)
    {
        AssertMainThread();
        if (value.Session != Session ||
            value.ContentRevision == 0 ||
            value.ContentRevision > sentWorldFence ||
            value.ContentRevision > wantedContent ||
            value.ContentRevision < acknowledgedWorldFence)
        {
            ++StaleAcknowledgements;
            return;
        }

        acknowledgedWorldFence = value.ContentRevision;
    }

    public void EnterDraw()
    {
        AssertMainThread();
        DrawDepth = checked(DrawDepth + 1);
    }

    public void LeaveDraw()
    {
        AssertMainThread();
        if (DrawDepth == 0)
            throw new InvalidOperationException("Unmatched draw exit.");
        --DrawDepth;
    }

    public void ReportFailure(string error)
    {
        AssertMainThread();
        faultLatched = true;
        if (LastFailure == null)
            LastFailure = error ?? "Owner failure";

        // A managed exception never gets an acknowledgement, so a stop ticket
        // must be marked recoverable instead of waiting on it forever.
        if (IsStopping())
            WaitingForOwnerRecovery = true;
    }

    public void RequestQuit()
    {
        AssertMainThread();
        quitting = true;
    }

    private bool IsPendingPreparation(Command value)
    {
        if (!pending.HasValue)
            return false;

        Command current = pending.Value;
        return current.Session == value.Session &&
            current.Serial == value.Serial &&
            current.Generation == value.Generation &&
            current.ContentRevision == value.ContentRevision &&
            current.Operation == value.Operation &&
            (value.Operation == Operation.PrepareHiddenGeneration || value.Operation == Operation.PrepareReplacementGeneration);
    }

    public void MarkPreparationSubmitted(Command value)
    {
        AssertMainThread();
        if (!IsPendingPreparation(value))
            throw new InvalidOperationException("Accepted preparation does not match the pending ticket.");

        preparationSubmitted = true;
    }

    public void SupersedeUnsubmittedPreparation(Command value, bool hasMainResources)
    {
        AssertMainThread();
        if (DrawDepth != 0 ||
            !IsPendingPreparation(value) ||
            preparationSubmitted ||
            reply.HasValue ||
            value.ContentRevision == wantedContent ||
            (State != Phase.Preparing && State != Phase.Rebuilding))
        {
            throw new InvalidOperationException("Only an obsolete, unsubmitted preparation can be superseded locally.");
        }

        // This cancels main's unsent intent, it is not an owner acknowledgement.
        // Anything already allocated still needs a real retirement command and its ACK.
        pending = null;

        if (hasMainResources)
        {
            State = Phase.RetiringUnusedGpu;
        }
        else
        {
            candidateGeneration = 0;
            State = activeGeneration == 0 ? Phase.Preparing : Phase.Rebuilding;
        }
    }

    public void Acknowledge(Acknowledgement value)
    {
        AssertMainThread();
        if (WaitingForOwnerRecovery ||
            !pending.HasValue ||
            value.Session != pending.Value.Session ||
            value.Serial != pending.Value.Serial ||
            value.Generation != pending.Value.Generation ||
            value.Operation != pending.Value.Operation ||
            reply.HasValue)
        {
            ++StaleAcknowledgements;
            return;
        }

        // Applied at the next safe boundary, never inside a draw.
        reply = value;
    }

    public void RetryAfterOwnerRecovery()
    {
        AssertMainThread();
        sentWorldFence = acknowledgedWorldFence;
        if (!WaitingForOwnerRecovery)
            return;

        WaitingForOwnerRecovery = false;
        pending = null;
        reply = null;
    }

    public void AbandonForProcessExit(bool processExitConfirmed)
    {
        AssertMainThread();
        if (!quitting || !processExitConfirmed)
            throw new InvalidOperationException("Only a confirmed process exit may abandon native resources.");

        // Nothing is destroyed or unloaded; the OS reclaims it at process teardown.
        ResourcesAbandonedToProcessExit = true;
        pending = null;
        reply = null;
        State = Phase.Exited;
    }

    // Hands out at most one command per safe boundary. The host runs it once,
    // records the ticket before submitting async work, and never infers an ACK.
    public Command? AdvanceAtSafeBoundary(bool captureTargetsRestored)
    {
        AssertMainThread();
        if (DrawDepth != 0 || State == Phase.Exited)
            return null;

        bool stop = !userEnabled || !renderingAllowed || quitting || faultLatched;
        if (stop && State != Phase.Off && !IsStopping())
            BeginStop();
        if (reply.HasValue)
            AcceptReply();
        if (WaitingForOwnerRecovery || pending.HasValue)
            return null;

        // AcceptReply may have latched a fault, so decide again before picking a phase.
        stop = !userEnabled || !renderingAllowed || quitting || faultLatched;
        if (stop && State != Phase.Off && !IsStopping())
            BeginStop();

        // A fault stops through the same retirement path as disabling, and stays
        // latched until an explicit off/on so Update cannot keep restarting a broken owner.
        if (State == Phase.Off)
        {
            if (quitting)
            {
                State = Phase.Exited;
                return null;
            }

            if (stop || wantedContent == 0)
                return null;

            Session = checked(Session + 1);
            sentWorldFence = acknowledgedWorldFence = 0;
            State = Phase.Preparing;
        }

        if (State == Phase.Active)
        {
            if (activeContent == wantedContent)
                return null;
            State = Phase.InvalidatingWorld;
        }

        Operation operation;
        ulong generation = activeGeneration;
        ulong content = wantedContent;
        bool hasWorld = wantedWorld;

        switch (State)
        {
            case Phase.Preparing:
            case Phase.Rebuilding:
                if (candidateGeneration == 0)
                {
                    candidateGeneration = checked(++nextGeneration);
                    candidateContent = wantedContent;
                    candidateWorld = wantedWorld;
                }

                generation = candidateGeneration;
                content = candidateContent;
                hasWorld = candidateWorld;
                operation = State == Phase.Preparing ? Operation.PrepareHiddenGeneration : Operation.PrepareReplacementGeneration;
                break;

            case Phase.Activating:
                if (candidateContent != wantedContent)
                {
                    State = Phase.RetiringUnusedGpu;
                    return AdvanceAtSafeBoundary(captureTargetsRestored);
                }

                if (acknowledgedWorldFence != candidateContent)
                    return null;

                generation = candidateGeneration;
                content = candidateContent;
                hasWorld = candidateWorld;
                operation = Operation.ActivateGeneration;
                break;

            case Phase.InvalidatingWorld:
                operation = Operation.InvalidateWorld;
                break;

            case Phase.RetiringUnusedGpu:
            case Phase.ReleasingUnusedMain:
                generation = candidateGeneration;
                content = candidateContent;
                hasWorld = candidateWorld;
                operation = State == Phase.RetiringUnusedGpu ? Operation.RetireGenerationGpu : Operation.ReleaseGenerationMain;
                break;

            case Phase.RetiringPreviousGpu:
            case Phase.ReleasingPreviousMain:
                generation = previousGeneration;
                content = previousContent;
                hasWorld = previousWorld;
                operation = State == Phase.RetiringPreviousGpu ? Operation.RetireGenerationGpu : Operation.ReleaseGenerationMain;
                break;

            case Phase.RestoringNative:
                operation = Operation.RestoreNativeRouting;
                break;

            case Phase.AwaitingNativeFrame:
                operation = Operation.AwaitNativeFrame;
                break;

            case Phase.Detaching:
                operation = Operation.DetachComposite;
                break;

            case Phase.RetiringGpu:
                operation = Operation.RetireSessionGpu;
                break;

            case Phase.ReleasingMain:
                operation = Operation.ReleaseSessionMain;
                break;

            case Phase.StoppingWorker:
                operation = Operation.StopWorker;
                break;

            default:
                throw new InvalidOperationException("Unrecognized lifecycle phase.");
        }

        bool releasesMain = operation == Operation.ReleaseGenerationMain || operation == Operation.ReleaseSessionMain;
        if (!captureTargetsRestored && releasesMain)
            return null;

        preparationSubmitted = false;
        pending = new Command
        {
            Session = Session,
            Serial = checked(++serial),
            Generation = generation,
            ContentRevision = content,
            HasWorld = hasWorld,
            PreviousGeneration = operation == Operation.ActivateGeneration ? activeGeneration : previousGeneration,
            AfterFrame = nativeRoutingFrame,
            Operation = operation,
            Required = Required(operation)
        };
        return pending;
    }

    private bool IsStopping() => State >= Phase.RestoringNative && State <= Phase.StoppingWorker;

    private void BeginStop()
    {
        // The RestoreNativeRouting ticket fences every older operation of this
        // session in the host, so a late creation still retires with the session.
        pending = null;
        reply = null;
        WaitingForOwnerRecovery = false;
        State = Phase.RestoringNative;
        nativeRoutingFrame = 0;
    }

    private void AcceptReply()
    {
        Command command = pending.Value;
        Acknowledgement ack = reply.Value;
        reply = null;

        if (ack.Superseded &&
            candidateContent != wantedContent &&
            (State == Phase.Preparing || State == Phase.Rebuilding || State == Phase.Activating))
        {
            // The owner rejected an obsolete generation before it went live. A live
            // generation must report its real result and retire by the previous-generation path.
            pending = null;
            State = Phase.RetiringUnusedGpu;
            return;
        }

        bool needsFrame = command.Operation == Operation.PrepareHiddenGeneration ||
            command.Operation == Operation.PrepareReplacementGeneration ||
            command.Operation == Operation.AwaitNativeFrame;
        bool valid = ack.Success &&
            !ack.Superseded &&
            (ack.Evidence & command.Required) == command.Required &&
            (!needsFrame || ack.Frame > 0) &&
            (command.Operation != Operation.AwaitNativeFrame || ack.Frame > nativeRoutingFrame);

        if (!valid)
        {
            if (LastFailure == null)
                LastFailure = ack.Error ?? "Owner ACK lacked the required frame or ownership evidence.";
            faultLatched = true;
            if (IsStopping())
            {
                WaitingForOwnerRecovery = true;
                return;
            }

            pending = null;
            return;
        }

        pending = null;

        switch (State)
        {
            case Phase.Preparing:
            case Phase.Rebuilding:
                State = Phase.Activating;
                break;

            case Phase.Activating:
                previousGeneration = activeGeneration;
                previousContent = activeContent;
                previousWorld = activeWorld;
                activeGeneration = candidateGeneration;
                activeContent = candidateContent;
                activeWorld = candidateWorld;
                candidateGeneration = 0;
                State = previousGeneration == 0 ? Phase.Active : Phase.RetiringPreviousGpu;
                break;

            case Phase.InvalidatingWorld:
                State = Phase.Rebuilding;
                break;

            case Phase.RetiringUnusedGpu:
                State = Phase.ReleasingUnusedMain;
                break;

            case Phase.ReleasingUnusedMain:
                candidateGeneration = 0;
                State = activeGeneration == 0 ? Phase.Preparing : Phase.Rebuilding;
                break;

            case Phase.RetiringPreviousGpu:
                State = Phase.ReleasingPreviousMain;
                break;

            case Phase.ReleasingPreviousMain:
                previousGeneration = 0;
                State = Phase.Active;
                break;

            case Phase.RestoringNative:
                nativeRoutingFrame = ack.Frame;
                // Only the native owner can rule out a delayed activation; main's
                // generation counter cannot see one that is still waiting for its ACK.
                State = (ack.Evidence & Evidence.NeverActivated) != 0 ? Phase.Detaching : Phase.AwaitingNativeFrame;
                break;

            case Phase.AwaitingNativeFrame:
                State = Phase.Detaching;
                break;

            case Phase.Detaching:
                State = Phase.RetiringGpu;
                break;

            case Phase.RetiringGpu:
                State = Phase.ReleasingMain;
                break;

            case Phase.ReleasingMain:
                State = Phase.StoppingWorker;
                break;

            case Phase.StoppingWorker:
                activeGeneration = candidateGeneration = previousGeneration = 0;
                activeContent = 0;
                State = Phase.Off;
                break;

            default:
                throw new InvalidOperationException("ACK does not fit the lifecycle phase.");
        }
    }

    public static Evidence Required(Operation operation)
    {
        switch (operation)
        {
            case Operation.PrepareHiddenGeneration:
                return Evidence.NativeFullUiMaintained | Evidence.CompositeComplete;
            case Operation.PrepareReplacementGeneration:
                return Evidence.CompositeComplete;
            case Operation.ActivateGeneration:
                return Evidence.CompositeActivated;
            case Operation.InvalidateWorld:
                return Evidence.OldWorldInvalidated;
            case Operation.RestoreNativeRouting:
                return Evidence.NativeRoutingRestored | Evidence.CaptureScopesClosed | Evidence.CameraReleased;
            case Operation.AwaitNativeFrame:
                return Evidence.NativeFullFrameAvailable;
            case Operation.DetachComposite:
                return Evidence.CompositeDetached;
            case Operation.RetireGenerationGpu:
            case Operation.RetireSessionGpu:
                return Evidence.RenderOwnersRetired;
            case Operation.ReleaseGenerationMain:
            case Operation.ReleaseSessionMain:
                return Evidence.MainResourcesReleased;
            case Operation.StopWorker:
                return Evidence.WorkerJoined;
            default:
                throw new ArgumentOutOfRangeException(nameof(operation));
        }
    }
}
